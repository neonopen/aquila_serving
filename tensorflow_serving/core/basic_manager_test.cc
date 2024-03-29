/* Copyright 2016 Google Inc. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow_serving/core/basic_manager.h"

#include <algorithm>
#include <functional>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "tensorflow/core/lib/core/blocking_counter.h"
#include "tensorflow/core/lib/core/error_codes.pb.h"
#include "tensorflow/core/lib/core/errors.h"
#include "tensorflow/core/lib/core/status_test_util.h"
#include "tensorflow/core/lib/strings/strcat.h"
#include "tensorflow_serving/core/availability_helpers.h"
#include "tensorflow_serving/core/servable_state_monitor.h"
#include "tensorflow_serving/core/test_util/availability_test_util.h"
#include "tensorflow_serving/core/test_util/fake_loader.h"
#include "tensorflow_serving/core/test_util/mock_loader.h"
#include "tensorflow_serving/util/any_ptr.h"
#include "tensorflow_serving/util/event_bus.h"
#include "tensorflow_serving/util/threadpool_executor.h"

namespace tensorflow {
namespace serving {
namespace {

using ::testing::_;
using ::testing::AnyOf;
using ::testing::HasSubstr;
using ::testing::InSequence;
using ::testing::Invoke;
using ::testing::InvokeWithoutArgs;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::UnorderedElementsAre;
using ::testing::UnorderedElementsAreArray;
using test_util::FakeLoader;
using test_util::WaitUntilServableManagerStateIsOneOf;

constexpr char kServableName[] = "kServableName";
constexpr char kServableName2[] = "kServableName2";
constexpr char kServableName3[] = "kServableName3";

constexpr int kNumVersionsPerServable = 2;

constexpr int kNumThreads = 10;

// Creates a ServableData around a FakeLoader.
ServableData<std::unique_ptr<Loader>> CreateServable(
    const ServableId& id, const Status load_status = Status::OK()) {
  std::unique_ptr<Loader> loader(new FakeLoader(id.version, load_status));
  return CreateServableData(id, std::move(loader));
}

// We parameterize this test to either run with or without a thread-pool.
class BasicManagerTest : public ::testing::TestWithParam<int> {
 protected:
  BasicManagerTest()
      : num_load_unload_threads_(GetParam()),
        servable_event_bus_(EventBus<ServableState>::CreateEventBus()),
        servable_state_monitor_(servable_event_bus_.get()) {
    BasicManager::Options options;
    options.num_load_unload_threads = num_load_unload_threads_;
    options.servable_event_bus = servable_event_bus_.get();
    options.max_num_load_retries = 10;
    options.load_retry_interval_micros = 0;
    TF_CHECK_OK(BasicManager::Create(std::move(options), &basic_manager_));
  }

  void SetUp() override {
    // We load the manager with two different servable streams, each with two
    // versions 0 and 1.
    std::set<ServableId> loaded_servables;
    for (const char* servable_name : {kServableName, kServableName2}) {
      for (int i = 1; i <= kNumVersionsPerServable; ++i) {
        const ServableId id = {servable_name, i};
        basic_manager_->ManageServable(CreateServable(id));
        basic_manager_->LoadServable(
            id, [](const Status& status) { TF_ASSERT_OK(status); });
        loaded_servables.insert(id);
      }
    }
    WaitUntilServablesAvailable(loaded_servables, basic_manager_.get());
  }

  int num_load_unload_threads_;
  std::shared_ptr<EventBus<ServableState>> servable_event_bus_;
  ServableStateMonitor servable_state_monitor_;
  std::unique_ptr<BasicManager> basic_manager_;
};

INSTANTIATE_TEST_CASE_P(WithOrWithoutThreadPool, BasicManagerTest,
                        ::testing::Values(0 /* WithoutThreadPool */,
                                          kNumThreads));

TEST_P(BasicManagerTest, ServableHandleNotFoundMissingLoaderName) {
  ServableHandle<int64> handle;
  const Status status = basic_manager_->GetServableHandle(
      ServableRequest::Latest(strings::StrCat(kServableName, "missing")),
      &handle);
  ASSERT_FALSE(status.ok()) << status;
  EXPECT_EQ(error::NOT_FOUND, status.code());
}

TEST_P(BasicManagerTest, ServableHandleNotFoundMissingVersion) {
  // This version is missing.
  const int64 missing_version = 100;
  ServableHandle<int64> handle;
  const Status status = basic_manager_->GetServableHandle(
      ServableRequest::Specific(kServableName, missing_version), &handle);
  ASSERT_FALSE(status.ok()) << status;
  EXPECT_EQ(error::NOT_FOUND, status.code());
}

TEST_P(BasicManagerTest, ServableHandleLatest) {
  const ServableId id = {kServableName, kNumVersionsPerServable + 1};
  basic_manager_->ManageServable(CreateServable(id));
  basic_manager_->LoadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServablesAvailable({id}, basic_manager_.get());

  ServableHandle<int64> handle;
  const Status status = basic_manager_->GetServableHandle(
      ServableRequest::Latest(kServableName), &handle);
  TF_ASSERT_OK(status);
  EXPECT_EQ(kNumVersionsPerServable + 1, *handle);
}

// Tests the case where the latest version of a servable available is 0.
TEST_P(BasicManagerTest, ServableHandleLatestVersionIsZero) {
  const ServableId id = {kServableName3, 1};
  basic_manager_->ManageServable(CreateServable(id));
  basic_manager_->LoadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServablesAvailable({id}, basic_manager_.get());

  ServableHandle<int64> handle;
  const Status status = basic_manager_->GetServableHandle(
      ServableRequest::Latest(kServableName3), &handle);
  TF_ASSERT_OK(status);
  EXPECT_EQ(1, *handle);
}

TEST_P(BasicManagerTest, ServableHandleSpecificVersion) {
  ServableHandle<int64> handle;
  const Status status = basic_manager_->GetServableHandle(
      ServableRequest::Specific(kServableName2, 1), &handle);
  TF_ASSERT_OK(status);
  EXPECT_EQ(1, *handle);
}

TEST_P(BasicManagerTest, ListAvailableServableIds) {
  const std::vector<ServableId> expected_before = {{kServableName, 1},
                                                   {kServableName, 2},
                                                   {kServableName2, 1},
                                                   {kServableName2, 2}};
  EXPECT_THAT(basic_manager_->ListAvailableServableIds(),
              UnorderedElementsAreArray(expected_before));

  // Set stream kServableName to have servables 7 and unload 0 & 1, but 7 errors
  // on load, so never moves to a loaded state.
  const ServableId id = {kServableName, 7};
  std::unique_ptr<Loader> loader(
      new FakeLoader(7, errors::Internal("An error.")));
  basic_manager_->ManageServable(CreateServableData(id, std::move(loader)));
  basic_manager_->LoadServable(id, [](const Status& status) {
    EXPECT_EQ(errors::Internal("An error."), status);
  });
  basic_manager_->UnloadServable(
      {kServableName, 1}, [](const Status& status) { TF_ASSERT_OK(status); });
  basic_manager_->UnloadServable(
      {kServableName, 2}, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_, id,
                                       {ServableState::ManagerState::kEnd});
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_,
                                       {kServableName, 1},
                                       {ServableState::ManagerState::kEnd});
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_,
                                       {kServableName, 2},
                                       {ServableState::ManagerState::kEnd});

  const std::vector<ServableId> expected_after = {{kServableName2, 1},
                                                  {kServableName2, 2}};
  EXPECT_THAT(basic_manager_->ListAvailableServableIds(),
              UnorderedElementsAreArray(expected_after));
}

TEST_P(BasicManagerTest, GetAvailableServableHandles) {
  // Scoped to destruct handles at the end of it.
  {
    const std::map<ServableId, ServableHandle<int64>> handles_before =
        basic_manager_->GetAvailableServableHandles<int64>();
    ASSERT_EQ(kNumVersionsPerServable * 2, handles_before.size());

    const std::vector<ServableId> expected_ids_before = {{kServableName, 1},
                                                         {kServableName, 2},
                                                         {kServableName2, 1},
                                                         {kServableName2, 2}};
    for (const ServableId& expected_id : expected_ids_before) {
      const auto found_it = handles_before.find(expected_id);
      ASSERT_TRUE(found_it != handles_before.end());
      EXPECT_EQ(expected_id.version, *found_it->second);
    }
  }

  // Set stream kServableName to have servables 7 and unload 0 & 1, but 7 errors
  // on load, so never moves to a loaded state.
  const ServableId id = {kServableName, 7};
  std::unique_ptr<Loader> loader(
      new FakeLoader(7, errors::Internal("An error.")));
  basic_manager_->ManageServable(CreateServableData(id, std::move(loader)));
  basic_manager_->LoadServable(id, [](const Status& status) {
    EXPECT_EQ(errors::Internal("An error."), status);
  });
  basic_manager_->UnloadServable(
      {kServableName, 1}, [](const Status& status) { TF_ASSERT_OK(status); });
  basic_manager_->UnloadServable(
      {kServableName, 2}, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_, id,
                                       {ServableState::ManagerState::kEnd});
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_,
                                       {kServableName, 1},
                                       {ServableState::ManagerState::kEnd});
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_,
                                       {kServableName, 2},
                                       {ServableState::ManagerState::kEnd});

  {
    const std::map<ServableId, ServableHandle<int64>> handles_after =
        basic_manager_->GetAvailableServableHandles<int64>();
    ASSERT_EQ(kNumVersionsPerServable, handles_after.size());

    const std::vector<ServableId> expected_ids_after = {{kServableName2, 1},
                                                        {kServableName2, 2}};
    for (const ServableId& expected_id : expected_ids_after) {
      const auto found_it = handles_after.find(expected_id);
      ASSERT_TRUE(found_it != handles_after.end());
      EXPECT_EQ(expected_id.version, *found_it->second);
    }
  }
}

TEST_P(BasicManagerTest, GetAvailableServableHandlesWrongType) {
  const std::map<ServableId, ServableHandle<int>> wrong_type_handles =
      basic_manager_->GetAvailableServableHandles<int>();
  EXPECT_EQ(0, wrong_type_handles.size());
}

TEST_P(BasicManagerTest, GetManagedServableNames) {
  EXPECT_THAT(basic_manager_->GetManagedServableNames(),
              UnorderedElementsAre(kServableName, kServableName2));
}

TEST_P(BasicManagerTest,
       GetManagedServableStateSnapshotsWithoutAdditionalState) {
  const ServableId id = {kServableName, 7};
  basic_manager_->ManageServable(
      ServableData<std::unique_ptr<Loader>>(id, errors::Internal("An error.")));
  const std::vector<ServableStateSnapshot<>> expected = {
      {{kServableName, 1}, LoaderHarness::State::kReady, {}},
      {{kServableName, 2}, LoaderHarness::State::kReady, {}},
      {{kServableName, 7}, LoaderHarness::State::kError, {}}};
  EXPECT_THAT(basic_manager_->GetManagedServableStateSnapshots(kServableName),
              UnorderedElementsAreArray(expected));
}

TEST_P(BasicManagerTest, GetManagedServableStateSnapshotsWithAdditionalState) {
  basic_manager_->ManageServableWithAdditionalState(
      CreateServable({kServableName3, 0}), std::unique_ptr<int>(new int(0)));
  basic_manager_->ManageServableWithAdditionalState(
      CreateServable({kServableName3, 1}), std::unique_ptr<int>(new int(1)));
  basic_manager_->ManageServableWithAdditionalState(
      ServableData<std::unique_ptr<Loader>>({kServableName3, 2},
                                            errors::Internal("An error.")),
      std::unique_ptr<int>(new int(2)));
  const std::vector<ServableStateSnapshot<int>> expected = {
      {{kServableName3, 0}, LoaderHarness::State::kNew, {0}},
      {{kServableName3, 1}, LoaderHarness::State::kNew, {1}},
      {{kServableName3, 2}, LoaderHarness::State::kError, {2}}};
  EXPECT_THAT(
      basic_manager_->GetManagedServableStateSnapshots<int>(kServableName3),
      UnorderedElementsAreArray(expected));
}

TEST_P(BasicManagerTest, MultipleManageCallsUsesFirstServable) {
  const ServableId id = {kServableName, 1};
  std::unique_ptr<Loader> first_loader(
      new FakeLoader(1, errors::Internal("An error.")));

  basic_manager_->ManageServable(
      CreateServableData(id, std::move(first_loader)));
  // Different servable returned.
  std::unique_ptr<Loader> second_loader(
      new FakeLoader(2, errors::Internal("An error.")));
  basic_manager_->ManageServable(
      CreateServableData(id, std::move(second_loader)));

  ServableHandle<int64> handle;
  TF_ASSERT_OK(basic_manager_->GetServableHandle(
      ServableRequest::Specific(kServableName, 1), &handle));
  EXPECT_EQ(1, *handle);
}

// Tests to ensure the manager doesn't try to load or serve an incoming
// erroneous servable.
TEST_P(BasicManagerTest, ErroneousServable) {
  const ServableId id = {kServableName, 3};
  basic_manager_->ManageServable(
      ServableData<std::unique_ptr<Loader>>(id, errors::Unknown("error")));

  ServableHandle<int64> handle;
  Status status = basic_manager_->GetServableHandle(
      ServableRequest::Specific(kServableName, 3), &handle);
  EXPECT_FALSE(status.ok()) << status;
  basic_manager_->LoadServable(id, [](const Status& status) {
    EXPECT_EQ(errors::Unknown("error"), status);
  });

  status = basic_manager_->GetServableHandle(
      ServableRequest::Specific(kServableName, 3), &handle);
  EXPECT_FALSE(status.ok()) << status;
}

// Tests to ensure that the deletion of a loader/servable occurs in a manager
// thread, and not a request thread.
TEST_P(BasicManagerTest, DestructOnNonServingThread) {
  const ServableId id = {kServableName, 7};
  basic_manager_->ManageServable(
      CreateServableData(id, std::unique_ptr<Loader>(new FakeLoader(7))));
  basic_manager_->LoadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServablesAvailable({id}, basic_manager_.get());

  std::unique_ptr<ServableHandle<int64>> latest_handle(
      new ServableHandle<int64>());
  const Status status = basic_manager_->GetServableHandle(
      ServableRequest::Latest(kServableName), latest_handle.get());
  TF_ASSERT_OK(status);
  EXPECT_EQ(7, **latest_handle);

  Notification done_unload_servable;
  std::unique_ptr<Thread> unload_servable(
      Env::Default()->StartThread({}, "UnloadServable", [&]() {
        // Unload the servable.
        basic_manager_->UnloadServable(
            id, [](const Status& status) { TF_ASSERT_OK(status); });
        WaitUntilServableManagerStateIsOneOf(
            servable_state_monitor_, id, {ServableState::ManagerState::kEnd});
        // The servable has been deleted in this thread if there is no
        // thread-pool for load/unload.
        if (num_load_unload_threads_ == 0) {
          EXPECT_TRUE(FakeLoader::was_deleted_in_this_thread());
        }
        done_unload_servable.Notify();
      }));

  // This will unblock the UnloadServable.
  latest_handle.reset();
  done_unload_servable.WaitForNotification();
  // The servable wasn't deleted in this thread.
  ASSERT_FALSE(FakeLoader::was_deleted_in_this_thread());
}

TEST_P(BasicManagerTest, AdditionalState) {
  const ServableId id = {kServableName, 3};
  std::unique_ptr<int> state(new int(1));
  basic_manager_->ManageServableWithAdditionalState(CreateServable(id),
                                                    std::move(state));

  EXPECT_EQ(1, *basic_manager_->GetAdditionalServableState<int>(id));
  EXPECT_EQ(nullptr, basic_manager_->GetAdditionalServableState<float>(id));
}

TEST_P(BasicManagerTest, NoAdditionalState) {
  const ServableId id = {kServableName, 3};
  basic_manager_->ManageServable(CreateServable(id));

  // Will return nullptr when there is no metadata set.
  EXPECT_EQ(nullptr, basic_manager_->GetAdditionalServableState<int>(id));
  EXPECT_EQ(nullptr, basic_manager_->GetAdditionalServableState<float>(id));
}

TEST_P(BasicManagerTest, OutOfOrderLoadServable) {
  const ServableId id = {kServableName, 3};
  basic_manager_->LoadServable(id, [](const Status& status) {
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(error::NOT_FOUND, status.code());
    EXPECT_THAT(status.error_message(), HasSubstr("is not being managed"));
  });
}

TEST_P(BasicManagerTest, MultipleLoadServables) {
  const ServableId id = {kServableName, 3};
  basic_manager_->ManageServable(CreateServable(id));
  basic_manager_->LoadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServablesAvailable({id}, basic_manager_.get());
  basic_manager_->LoadServable(id, [](const Status& status) {
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(error::FAILED_PRECONDITION, status.code());
    EXPECT_THAT(status.error_message(),
                HasSubstr("cannot be transitioned to load-requested"));
  });
}

TEST_P(BasicManagerTest, MultipleUnloadServables) {
  const ServableId id = {kServableName, 3};
  basic_manager_->ManageServable(CreateServable(id));
  basic_manager_->LoadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServablesAvailable({id}, basic_manager_.get());
  basic_manager_->UnloadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_, id,
                                       {ServableState::ManagerState::kEnd});
  basic_manager_->UnloadServable(id, [](const Status& status) {
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(error::NOT_FOUND, status.code());
    EXPECT_THAT(status.error_message(), HasSubstr("is not being managed"));
  });
}

TEST_P(BasicManagerTest, UnloadWithoutManage) {
  const ServableId id = {kServableName, 3};
  basic_manager_->UnloadServable(id, [](const Status& status) {
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(error::NOT_FOUND, status.code());
    EXPECT_THAT(status.error_message(), HasSubstr("is not being managed"));
  });
}

TEST_P(BasicManagerTest, UnloadWithoutLoad) {
  const ServableId id = {kServableName, 3};
  basic_manager_->ManageServable(CreateServable(id));
  basic_manager_->UnloadServable(id, [](const Status& status) {
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(error::FAILED_PRECONDITION, status.code());
    EXPECT_THAT(status.error_message(),
                HasSubstr("cannot be transitioned to unload-requested"));
  });
}

MATCHER_P(EqualsServableState, servable_state, servable_state.DebugString()) {
  if (arg == servable_state) {
    return true;
  }
  *result_listener << arg.DebugString();
  return false;
}

TEST_P(BasicManagerTest, EventBusErroneousVersion) {
  const ServableId id = {kServableName, 3};
  basic_manager_->ManageServable(
      ServableData<std::unique_ptr<Loader>>(id, errors::Unknown("error")));

  const ServableState expected_published_state = {
      id, ServableState::ManagerState::kEnd, errors::Unknown("error")};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(expected_published_state));
}

TEST_P(BasicManagerTest, EventBusErrorOnLoad) {
  const ServableId id = {kServableName, 7};
  std::unique_ptr<Loader> loader(
      new FakeLoader(7, errors::Internal("Error on load.")));
  basic_manager_->ManageServable({id, std::move(loader)});

  const ServableState start_state = {id, ServableState::ManagerState::kStart,
                                     Status::OK()};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(start_state));

  basic_manager_->LoadServable(id, [](const Status& status) {});
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_, id,
                                       {ServableState::ManagerState::kEnd});

  const ServableState error_state = {id, ServableState::ManagerState::kEnd,
                                     errors::Internal("Error on load.")};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(error_state));
}

TEST_P(BasicManagerTest, EventBusServableLifecycle) {
  const ServableId id = {kServableName, 7};
  test_util::MockLoader* loader = new NiceMock<test_util::MockLoader>();
  basic_manager_->ManageServable({id, std::unique_ptr<Loader>(loader)});

  const ServableState start_state = {id, ServableState::ManagerState::kStart,
                                     Status::OK()};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(start_state));

  Notification load_called;
  Notification load_continue;
  EXPECT_CALL(*loader, Load(_))
      .WillOnce(InvokeWithoutArgs([&]() {
        load_called.Notify();
        load_continue.WaitForNotification();
        return Status::OK();
      }));

  std::unique_ptr<Thread> load_thread(
      Env::Default()->StartThread(ThreadOptions(), "LoadThread", [&]() {
        basic_manager_->LoadServable(id, [](const Status& status) {});
      }));

  load_called.WaitForNotification();

  const ServableState loading_state = {
      id, ServableState::ManagerState::kLoading, Status::OK()};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(loading_state));

  load_continue.Notify();
  WaitUntilServablesAvailable({id}, basic_manager_.get());

  const ServableState available_state = {
      id, ServableState::ManagerState::kAvailable, Status::OK()};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(available_state));

  Notification unload_called;
  Notification unload_continue;
  EXPECT_CALL(*loader, Unload())
      .WillOnce(Invoke([&]() {
        unload_called.Notify();
        unload_continue.WaitForNotification();
      }));
  // Scoped to ensure UnloadServable() is scheduled.
  std::unique_ptr<Thread> unload_thread(
      Env::Default()->StartThread(ThreadOptions(), "UnloadThread", [&]() {
        basic_manager_->UnloadServable(id, [](const Status& status) {});
      }));

  unload_called.WaitForNotification();

  const ServableState unloading_state = {
      id, ServableState::ManagerState::kUnloading, Status::OK()};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(unloading_state));

  unload_continue.Notify();
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_, id,
                                       {ServableState::ManagerState::kEnd});

  const ServableState end_state = {id, ServableState::ManagerState::kEnd,
                                   Status::OK()};
  EXPECT_THAT(*servable_state_monitor_.GetState(id),
              EqualsServableState(end_state));
}

// Tests whether there are any errors if we don't have an event bus configured.
TEST_P(BasicManagerTest, NoEventBus) {
  BasicManager::Options options;
  // Single threaded execution.
  options.num_load_unload_threads = 0;
  // No event bus.
  options.servable_event_bus = nullptr;
  std::unique_ptr<BasicManager> manager;
  TF_ASSERT_OK(BasicManager::Create(std::move(options), &manager));

  const ServableId id = {kServableName, 7};
  std::unique_ptr<Loader> loader(new FakeLoader(7));
  manager->ManageServable({id, std::move(loader)});
  manager->LoadServable(id, [](const Status& status) { TF_ASSERT_OK(status); });
  manager->UnloadServable(id,
                          [](const Status& status) { TF_ASSERT_OK(status); });
}

TEST_P(BasicManagerTest, LoadsThenUnloads) {
  std::set<ServableId> servables;
  // Scoped so that all loads can be scheduled before proceeding.
  {
    ThreadPoolExecutor load_executor(Env::Default(), "LoadServables",
                                     kNumThreads);
    for (int i = 0; i < 20; ++i) {
      const ServableId id = {kServableName3, i};
      servables.insert(id);
      load_executor.Schedule([this, id, &servables]() {
        basic_manager_->ManageServable(CreateServable(id));
        basic_manager_->LoadServable(
            id, [](const Status& status) { TF_ASSERT_OK(status); });
      });
    }
  }

  // At this point, all loads may not have completed, so we wait for them.
  WaitUntilServablesAvailable(servables, basic_manager_.get());

  {
    ThreadPoolExecutor unload_executor(Env::Default(), "UnloadServables",
                                       kNumThreads);
    // Doing in reverse.
    for (int i = 19; i >= 0; --i) {
      unload_executor.Schedule([this, i]() {
        const ServableId id = {kServableName3, i};
        basic_manager_->UnloadServable(
            id, [](const Status& status) { TF_ASSERT_OK(status); });
      });
    }
  }
}

TEST_P(BasicManagerTest, InterleavedLoadsAndUnloads) {
  ThreadPoolExecutor executor(Env::Default(), "InterleavedLoadsAndUnloads",
                              kNumThreads);
  for (int i = 0; i < 20; ++i) {
    executor.Schedule([this, i]() {
      const ServableId id = {kServableName3, i};
      basic_manager_->ManageServable(CreateServable(id));
      Notification load_done;
      basic_manager_->LoadServable(id, [&load_done](const Status& status) {
        TF_ASSERT_OK(status);
        load_done.Notify();
      });
      load_done.WaitForNotification();
      basic_manager_->UnloadServable(
          id, [](const Status& status) { TF_ASSERT_OK(status); });
    });
  }
}

TEST_P(BasicManagerTest, ConcurrentLoadsOnlyOneSucceeds) {
  const ServableId id = {kServableName3, 0};
  mutex status_mu;
  std::vector<Status> statuses(4);
  {
    ThreadPoolExecutor load_executor(Env::Default(), "LoadServables",
                                     kNumThreads);
    for (int i = 0; i < 4; ++i) {
      load_executor.Schedule([this, id, i, &statuses, &status_mu]() {
        basic_manager_->ManageServable(CreateServable(id));
        basic_manager_->LoadServable(
            id, [i, &statuses, &status_mu](const Status& status) {
              mutex_lock l(status_mu);
              statuses[i] = status;
            });
      });
    }
  }

  // At this point, all loads may not have completed. Deleting BasicManager
  // would wait for all the scheduled loads to complete before deleting it.
  basic_manager_.reset();

  int num_status_ok = 0;
  for (int i = 0; i < 4; ++i) {
    mutex_lock l(status_mu);
    if (!statuses[i].ok()) {
      EXPECT_EQ(error::FAILED_PRECONDITION, statuses[i].code());
      EXPECT_THAT(statuses[i].error_message(),
                  HasSubstr("cannot be transitioned to load-requested"));
    } else {
      ++num_status_ok;
    }
  }
  EXPECT_EQ(1, num_status_ok);
}

TEST_P(BasicManagerTest, ConcurrentUnloadsOnlyOneSucceeds) {
  const ServableId id = {kServableName3, 0};
  basic_manager_->ManageServable(CreateServable(id));
  basic_manager_->LoadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
  // At this point, all loads may not have completed, so we wait for them.
  WaitUntilServablesAvailable({id}, basic_manager_.get());

  mutex status_mu;
  std::vector<Status> statuses(4);
  {
    ThreadPoolExecutor load_executor(Env::Default(), "LoadServables",
                                     kNumThreads);
    for (int i = 0; i < 4; ++i) {
      load_executor.Schedule([this, id, i, &statuses, &status_mu]() {
        basic_manager_->UnloadServable(
            id, [i, &statuses, &status_mu](const Status& status) {
              mutex_lock l(status_mu);
              statuses[i] = status;
            });
      });
    }
  }

  // At this point, all unloads may not have completed. Deleting BasicManager
  // would wait for all the scheduled unloads to complete before deleting it.
  basic_manager_.reset();

  int num_status_ok = 0;
  for (int i = 0; i < 4; ++i) {
    mutex_lock l(status_mu);
    // The error can be either of 2.
    if (!statuses[i].ok()) {
      ASSERT_THAT(statuses[i].code(),
                  AnyOf(error::NOT_FOUND, error::FAILED_PRECONDITION));
      if (statuses[i].code() == error::NOT_FOUND) {
        EXPECT_THAT(statuses[i].error_message(),
                    HasSubstr("not being managed"));
      } else {
        EXPECT_THAT(statuses[i].error_message(),
                    HasSubstr("cannot be transitioned to unload-requested"));
      }
    } else {
      ++num_status_ok;
    }
  }
  EXPECT_EQ(1, num_status_ok);
}

TEST_P(BasicManagerTest, RetryOnLoadErrorFinallySucceeds) {
  const ServableId id = {kServableName, 7};
  test_util::MockLoader* loader = new NiceMock<test_util::MockLoader>();
  basic_manager_->ManageServable({id, std::unique_ptr<Loader>(loader)});
  EXPECT_CALL(*loader, Load(_))
      .WillOnce(Return(errors::Internal("Load error.")))
      .WillRepeatedly(Return(Status::OK()));
  basic_manager_->LoadServable(
      id, [](const Status& status) { TF_ASSERT_OK(status); });
}

TEST_P(BasicManagerTest, RetryOnLoadErrorFinallyFails) {
  const ServableId id = {kServableName, 7};
  test_util::MockLoader* loader = new NiceMock<test_util::MockLoader>();
  basic_manager_->ManageServable({id, std::unique_ptr<Loader>(loader)});
  EXPECT_CALL(*loader, Load(_))
      .WillRepeatedly(Return(errors::Internal("Load error.")));
  basic_manager_->LoadServable(id, [](const Status& status) {
    EXPECT_EQ(errors::Internal("Load error."), status);
  });
}

// Tests cancelling load retries.
TEST_P(BasicManagerTest, RetryOnLoadErrorCancelledLoad) {
  const ServableId id = {kServableName, 7};
  test_util::MockLoader* loader = new NiceMock<test_util::MockLoader>();
  basic_manager_->ManageServable({id, std::unique_ptr<Loader>(loader)});

  Notification load_called;
  Notification load_should_return;
  EXPECT_CALL(*loader, Load(_))
      .WillOnce(InvokeWithoutArgs([&load_called, &load_should_return]() {
        load_called.Notify();
        load_should_return.WaitForNotification();
        return errors::Internal("Load error.");
      }))
      .WillRepeatedly(Return(Status::OK()));
  std::unique_ptr<Thread> load_thread(
      Env::Default()->StartThread(ThreadOptions(), "LoadServable", [&]() {
        basic_manager_->LoadServable(id, [](const Status& status) {
          EXPECT_EQ(errors::Internal("Load error."), status);
        });
      }));
  load_called.WaitForNotification();
  basic_manager_->CancelLoadServableRetry(id);
  load_should_return.Notify();
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_, id,
                                       {ServableState::ManagerState::kEnd});
}

TEST_P(BasicManagerTest, LoadAfterCancelledLoad) {
  const ServableId id = {kServableName, 7};
  test_util::MockLoader* loader = new NiceMock<test_util::MockLoader>();
  basic_manager_->ManageServable({id, std::unique_ptr<Loader>(loader)});

  Notification load_called;
  Notification load_should_return;
  EXPECT_CALL(*loader, Load(_))
      .WillOnce(InvokeWithoutArgs([&load_called, &load_should_return]() {
        load_called.Notify();
        load_should_return.WaitForNotification();
        return errors::Internal("Load error.");
      }))
      .WillRepeatedly(Return(Status::OK()));

  std::unique_ptr<Thread> load_thread(
      Env::Default()->StartThread(ThreadOptions(), "LoadServable", [&]() {
        basic_manager_->LoadServable(id, [](const Status& status) {
          EXPECT_EQ(errors::Internal("Load error."), status);
        });
      }));
  load_called.WaitForNotification();
  basic_manager_->CancelLoadServableRetry(id);
  load_should_return.Notify();
  WaitUntilServableManagerStateIsOneOf(servable_state_monitor_, id,
                                       {ServableState::ManagerState::kEnd});

  basic_manager_->LoadServable(id, [](const Status& status) {
    EXPECT_EQ(errors::Internal("Load error."), status);
  });
}

// Creates a ResourceAllocation proto with 'quantity' units of RAM.
ResourceAllocation CreateResourceQuantity(const int quantity) {
  ResourceAllocation allocation;
  auto* ram_resource = allocation.add_resource_quantities();
  ram_resource->mutable_resource()->set_device("main");
  ram_resource->mutable_resource()->set_kind("ram");
  ram_resource->set_quantity(quantity);
  return allocation;
}

// Creates a resource tracker that deals with just a single resource (RAM) and
// initially has 'total_ram_resources' quantity of that resource.
std::unique_ptr<ResourceTracker> CreateSimpleResourceTracker(
    const int resource_quantity) {
  std::unique_ptr<ResourceUtil> util(new ResourceUtil({{{"main", 1}}}));
  std::unique_ptr<ResourceTracker> tracker;
  TF_CHECK_OK(ResourceTracker::Create(CreateResourceQuantity(resource_quantity),
                                      std::move(util), &tracker));
  return tracker;
}

class ResourceConstrainedBasicManagerTest : public ::testing::Test {
 protected:
  ResourceConstrainedBasicManagerTest()
      : servable_event_bus_(EventBus<ServableState>::CreateEventBus()),
        servable_state_monitor_(servable_event_bus_.get()) {
    BasicManager::Options options;
    // Seed the manager with ten resource units.
    options.resource_tracker = CreateSimpleResourceTracker(10);
    options.servable_event_bus = servable_event_bus_.get();
    // Allow up to two load/unload requests to be processed concurrently.
    options.num_load_unload_threads = 2;
    // We don't want retries.
    options.max_num_load_retries = 0;
    TF_CHECK_OK(BasicManager::Create(std::move(options), &basic_manager_));
  }

  std::shared_ptr<EventBus<ServableState>> servable_event_bus_;
  ServableStateMonitor servable_state_monitor_;
  std::unique_ptr<BasicManager> basic_manager_;
};

// A loader whose Load() method calls into a blocking counter. It requires 5
// resource units, i.e. half of the total system resources.
class BarrierLoader : public Loader {
 public:
  explicit BarrierLoader(BlockingCounter* counter) : counter_(counter) {}
  ~BarrierLoader() override = default;

  Status EstimateResources(ResourceAllocation* estimate) const override {
    *estimate = CreateResourceQuantity(5);
    return Status::OK();
  }

  Status Load(const ResourceAllocation& available_resources) override {
    counter_->DecrementCount();
    counter_->Wait();
    return Status::OK();
  }

  void Unload() override {}

  AnyPtr servable() override { return AnyPtr(); }

 private:
  BlockingCounter* const counter_;

  TF_DISALLOW_COPY_AND_ASSIGN(BarrierLoader);
};

TEST_F(ResourceConstrainedBasicManagerTest, ConcurrentLoads) {
  // Two loads that each require half the system resources should be handled
  // concurrently (i.e. the manager should not serialize them needlessly).
  // BarrierLoader verifies that the Load() calls occur concurrently.
  int kNumLoaders = 2;
  BlockingCounter barrier(kNumLoaders);
  for (int i = 0; i < kNumLoaders; ++i) {
    std::unique_ptr<Loader> loader(new BarrierLoader(&barrier));
    const ServableId id = {"barrier", i};
    basic_manager_->ManageServable(CreateServableData(id, std::move(loader)));
    basic_manager_->LoadServable(
        id, [](const Status& status) { TF_EXPECT_OK(status); });
  }
  // Force the manager to finish before deleting 'barrier'.
  basic_manager_.reset();
}

TEST_F(ResourceConstrainedBasicManagerTest, InsufficientResources) {
  // A first loader that succeeds and consumes all of the serving system's
  // resources.
  const ServableId hogging_id = {"hogging", 0};
  test_util::MockLoader* hogging_loader = new NiceMock<test_util::MockLoader>;
  ON_CALL(*hogging_loader, EstimateResources(_))
      .WillByDefault(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(10 /* = total system resources */);
        return Status::OK();
      }));
  EXPECT_CALL(*hogging_loader, Load(_)).WillOnce(Return(Status::OK()));
  basic_manager_->ManageServable(
      CreateServableData(hogging_id, std::unique_ptr<Loader>(hogging_loader)));
  Notification hogging_loaded;
  basic_manager_->LoadServable(hogging_id,
                               [&hogging_loaded](const Status& status) {
                                 TF_EXPECT_OK(status);
                                 hogging_loaded.Notify();
                               });
  hogging_loaded.WaitForNotification();

  // A second loader that gets rejected due to insufficient resources.
  const ServableId rejected_id = {"rejected", 0};
  test_util::MockLoader* rejected_loader = new NiceMock<test_util::MockLoader>;
  ON_CALL(*rejected_loader, EstimateResources(_))
      .WillByDefault(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(1);
        return Status::OK();
      }));
  basic_manager_->ManageServable(CreateServableData(
      rejected_id, std::unique_ptr<Loader>(rejected_loader)));
  basic_manager_->LoadServable(rejected_id, [](const Status& status) {
    ASSERT_FALSE(status.ok());
    ASSERT_EQ(error::RESOURCE_EXHAUSTED, status.code());
  });
}

TEST_F(ResourceConstrainedBasicManagerTest, ResourcesReleasedIfLoadFails) {
  // A first loader that fails. Its resource reservation should get released.
  const ServableId failing_id = {"failing", 0};
  test_util::MockLoader* failing_loader = new NiceMock<test_util::MockLoader>;
  ON_CALL(*failing_loader, EstimateResources(_))
      .WillByDefault(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(10);
        return Status::OK();
      }));
  EXPECT_CALL(*failing_loader, Load(_))
      .WillOnce(Return(errors::Unknown("Load failure")));
  basic_manager_->ManageServable(
      CreateServableData(failing_id, std::unique_ptr<Loader>(failing_loader)));
  Notification failing_failed;
  basic_manager_->LoadServable(failing_id,
                               [&failing_failed](const Status& status) {
                                 EXPECT_FALSE(status.ok());
                                 failing_failed.Notify();
                               });
  failing_failed.WaitForNotification();

  // A second loader that succeeds. The failure of the first loader should
  // enable this one to get loaded (versus rejection with a resource exhaustion
  // error).
  const ServableId succeeding_id = {"succeeding", 0};
  test_util::MockLoader* succeeding_loader =
      new NiceMock<test_util::MockLoader>;
  ON_CALL(*succeeding_loader, EstimateResources(_))
      .WillByDefault(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(10);
        return Status::OK();
      }));
  EXPECT_CALL(*succeeding_loader, Load(_)).WillOnce(Return(Status::OK()));
  basic_manager_->ManageServable(CreateServableData(
      succeeding_id, std::unique_ptr<Loader>(succeeding_loader)));
  basic_manager_->LoadServable(
      succeeding_id, [](const Status& status) { TF_EXPECT_OK(status); });
}

TEST_F(ResourceConstrainedBasicManagerTest,
       ResourcesReleasedIfEstimateDecreasesAfterLoad) {
  // A first loader that succeeds and then lowers its resource estimate.
  const ServableId overestimating_id = {"overestimating", 0};
  test_util::MockLoader* overestimating_loader =
      new NiceMock<test_util::MockLoader>;
  {
    InSequence sequence;
    EXPECT_CALL(*overestimating_loader, EstimateResources(_))
        .WillOnce(Invoke([](ResourceAllocation* estimate) {
          *estimate = CreateResourceQuantity(10);
          return Status::OK();
        }))
        .RetiresOnSaturation();
    EXPECT_CALL(*overestimating_loader, Load(_)).WillOnce(Return(Status::OK()));
    EXPECT_CALL(*overestimating_loader, EstimateResources(_))
        .WillOnce(Invoke([](ResourceAllocation* estimate) {
          *estimate = CreateResourceQuantity(5 /* lower estimate after load */);
          return Status::OK();
        }))
        .RetiresOnSaturation();
  }
  basic_manager_->ManageServable(CreateServableData(
      overestimating_id, std::unique_ptr<Loader>(overestimating_loader)));
  Notification overestimating_loaded;
  basic_manager_->LoadServable(overestimating_id,
                               [&overestimating_loaded](const Status& status) {
                                 TF_EXPECT_OK(status);
                                 overestimating_loaded.Notify();
                               });
  overestimating_loaded.WaitForNotification();

  // A second loader that succeeds. The re-estimation of the first loader should
  // enable this one to get loaded (versus rejection with a resource exhaustion
  // error).
  const ServableId succeeding_id = {"succeeding", 0};
  test_util::MockLoader* succeeding_loader =
      new NiceMock<test_util::MockLoader>;
  ON_CALL(*succeeding_loader, EstimateResources(_))
      .WillByDefault(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(5);
        return Status::OK();
      }));
  EXPECT_CALL(*succeeding_loader, Load(_)).WillOnce(Return(Status::OK()));
  basic_manager_->ManageServable(CreateServableData(
      succeeding_id, std::unique_ptr<Loader>(succeeding_loader)));
  basic_manager_->LoadServable(
      succeeding_id, [](const Status& status) { TF_EXPECT_OK(status); });
}

TEST_F(ResourceConstrainedBasicManagerTest, ResourcesReleasedAfterUnload) {
  const ServableId unloading_id = {"unloading", 0};
  test_util::MockLoader* unloading_loader = new NiceMock<test_util::MockLoader>;
  ON_CALL(*unloading_loader, EstimateResources(_))
      .WillByDefault(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(10);
        return Status::OK();
      }));
  Notification load_done;
  EXPECT_CALL(*unloading_loader, Load(_))
      .WillOnce(Invoke([&load_done](const ResourceAllocation& allocation) {
        load_done.Notify();
        return Status::OK();
      }));
  basic_manager_->ManageServable(CreateServableData(
      unloading_id, std::unique_ptr<Loader>(unloading_loader)));
  basic_manager_->LoadServable(
      unloading_id, [](const Status& status) { TF_EXPECT_OK(status); });
  load_done.WaitForNotification();
  Notification unload_started;
  Notification finish_unload;
  EXPECT_CALL(*unloading_loader, Unload())
      .WillOnce(Invoke([&unload_started, &finish_unload] {
        unload_started.Notify();
        finish_unload.WaitForNotification();
      }));
  basic_manager_->UnloadServable(
      unloading_id, [](const Status& status) { TF_EXPECT_OK(status); });
  unload_started.WaitForNotification();

  // A second loader that succeeds. The unloading of the first loader should
  // enable this one to get loaded (versus rejection with a resource exhaustion
  // error).
  const ServableId succeeding_id = {"succeeding", 0};
  test_util::MockLoader* succeeding_loader =
      new NiceMock<test_util::MockLoader>;
  EXPECT_CALL(*succeeding_loader, EstimateResources(_))
      .WillOnce(Invoke([&finish_unload](ResourceAllocation* estimate) {
        finish_unload.Notify();
        *estimate = CreateResourceQuantity(10);
        return Status::OK();
      }))
      .WillOnce(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(10);
        return Status::OK();
      }));
  EXPECT_CALL(*succeeding_loader, Load(_)).WillOnce(Return(Status::OK()));
  basic_manager_->ManageServable(CreateServableData(
      succeeding_id, std::unique_ptr<Loader>(succeeding_loader)));
  basic_manager_->LoadServable(
      succeeding_id, [](const Status& status) { TF_EXPECT_OK(status); });

  // Force the manager to finish before deleting the notifications.
  basic_manager_.reset();
}

TEST_F(ResourceConstrainedBasicManagerTest, FirstLoadDeniedSecondOneApproved) {
  // A first loader that gets rejected due to insufficient resources.
  const ServableId denied_id = {"denied", 0};
  test_util::MockLoader* denied_loader = new NiceMock<test_util::MockLoader>;
  Notification denied_estimate_started;
  Notification finish_denied_estimate;
  EXPECT_CALL(*denied_loader, EstimateResources(_))
      .WillOnce(Invoke([&denied_estimate_started,
                        &finish_denied_estimate](ResourceAllocation* estimate) {
        denied_estimate_started.Notify();
        finish_denied_estimate.WaitForNotification();
        *estimate = CreateResourceQuantity(11 /* more than the system total */);
        return Status::OK();
      }));
  // Load won't be called because resources are not enough to load it.
  EXPECT_CALL(*denied_loader, Load(_)).Times(0);
  basic_manager_->ManageServable(
      CreateServableData(denied_id, std::unique_ptr<Loader>(denied_loader)));

  // A second loader that succeeds.
  const ServableId succeeding_id = {"succeeding", 0};
  test_util::MockLoader* succeeding_loader =
      new NiceMock<test_util::MockLoader>;
  ON_CALL(*succeeding_loader, EstimateResources(_))
      .WillByDefault(Invoke([](ResourceAllocation* estimate) {
        *estimate = CreateResourceQuantity(10);
        return Status::OK();
      }));
  basic_manager_->ManageServable(CreateServableData(
      succeeding_id, std::unique_ptr<Loader>(succeeding_loader)));

  Status denied_load_status;
  // Place the first servable into a load request decision phase.
  basic_manager_->LoadServable(
      denied_id, [&denied_load_status](const Status& status) {
        denied_load_status = status;
        ASSERT_FALSE(status.ok());
        EXPECT_EQ(error::RESOURCE_EXHAUSTED, status.code());
      });
  denied_estimate_started.WaitForNotification();
  // The second servable's Load() call shouldn't occur until after the first
  // servable's load request exits its decision phase.
  EXPECT_CALL(*succeeding_loader, Load(_))
      .WillOnce(Invoke([&finish_denied_estimate](
          const ResourceAllocation& available_resources) {
        // Ensure that the first servable's load request has been given
        // permission to exit its decision phase.
        EXPECT_TRUE(finish_denied_estimate.HasBeenNotified());
        return Status::OK();
      }));

  // Scoping ensures that the thread is run by the end of this scope.
  {
    // Have to run this in a thread otherwise we enter a deadlock because
    // LoadServable() locks a mutex which is already locked by the denied
    // servable's decision phase, and is waiting for finish_denied_estimate to
    // be notified.
    std::unique_ptr<Thread> load_servable(
        Env::Default()->StartThread({}, "LoadServable", [&]() {
          basic_manager_->LoadServable(succeeding_id, [](const Status& status) {
            TF_EXPECT_OK(status);
          });
        }));

    finish_denied_estimate.Notify();
  }

  // Force the manager to finish before deleting the notifications.
  basic_manager_.reset();

  const ServableState expected_error_state = {
      denied_id, ServableState::ManagerState::kEnd, denied_load_status};
  EXPECT_THAT(*servable_state_monitor_.GetState(denied_id),
              EqualsServableState(expected_error_state));
}

}  // namespace
}  // namespace serving
}  // namespace tensorflow
