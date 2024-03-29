#                                                              #
# ARE YOU READY FOR THE BIGGEST ORDEAL OF YOUR LIFE?? HOPE SO! #
#                                                              #
​
# initial used space: 749 mb
​
# for this we assume you have an EBS volume mounted to /data/
# before you begin, make sure you mount the volume
# deal with the mounted drive:
cd /data
# (1) Add cuda
wget http://developer.download.nvidia.com/compute/cuda/7_0/Prod/local_installers/cuda_7.0.28_linux.run
# (2) scp cudnn over
# scp -i ~/.ssh/nick_gpu_instance.pem <filename> ubuntu@<server_ip>:/data
# e.g.,
# scp -i ~/.ssh/nick_gpu_instance.pem /Users/davidlea/Downloads/cudnn-6.5-linux-x64-v2.tgz ubuntu@10.0.83.215:/data
# (3) make the tmp directory point to /data
# this helps deal with the ABSOFUCKINGLUTELY COLOSSAL space requirements
# of bazel and tensorflow
sudo mkdir /data/tmp && sudo chmod 777 /data/tmp && sudo rm -rf /tmp && sudo ln -s /data/tmp /tmp
​
​
​
# NOTE: This will require more than the default (8gb) amount of space afforded to new instances. Make sure you increase it!
​
# Install various packages
sudo apt-get update && sudo apt-get upgrade -y
# used space: 866 mb
sudo apt-get install -y build-essential curl libfreetype6-dev libpng12-dev libzmq3-dev pkg-config python-pip python-dev git python-numpy python-scipy swig software-properties-common  python-dev default-jdk zip zlib1g-dev ipython autoconf libtool libjpeg8-dev
# upgrade six
sudo pip install --upgrade six
# used space: 1.4 gb?!?
​
# installing grpcio isn't sufficient if you intend on compiling new *_pb2.py files. You need to build from source.
cd /data && git clone https://github.com/grpc/grpc.git && cd grpc
git submodule update --init
make -j8 && sudo make install
# clean up
make clean
​
# now you can install grpcio
sudo pip install grpcio
# used space: 1.5 gb
​
# upgrade Pillow
sudo pip install --upgrade Pillow
​
# NOTE: THIS MAY NOT BE NECESSARY
# upgrade numpy so that it has the tobytes method
sudo pip install numpy --upgrade
​

# --- WARNING ---
# WILL RESTART AFTER YOU ISSUE THIS COMMAND
# ---------------
# Blacklist Noveau which has some kind of conflict with the nvidia driver
# Also, you'll have to reboot (annoying you have to do this in 2016!)
echo -e "blacklist nouveau\nblacklist lbm-nouveau\noptions nouveau modeset=0\nalias nouveau off\nalias lbm-nouveau off\n" | sudo tee /etc/modprobe.d/blacklist-nouveau.conf && echo options nouveau modeset=0 | sudo tee -a /etc/modprobe.d/nouveau-kms.conf && sudo update-initramfs -u && sudo reboot 
​
​
# --- WARNING ---
# WILL RESTART AFTER YOU ISSUE THIS COMMAND
# ---------------
# NOTE: /dev/xvdf and /dev/xvdb are the most common. make sure you change it appropriately!
sudo mount /dev/xvdb /data 
# Some other annoying thing we have to do
sudo apt-get install -y linux-image-extra-virtual && sudo reboot # Not sure why this is needed
​
# remount the EBS volume:
# NOTE: /dev/xvdf and /dev/xvdb are the most common. make sure you change it appropriately!
sudo mount /dev/xvdb /data
​
​
# Install latest Linux headers
sudo apt-get install -y linux-source linux-headers-`uname -r` 
​
​
# Install CUDA 7.0 (note – don't use any other version)
# you should've already put it in /data
cd /data && chmod +x cuda_7.0.28_linux.run && ./cuda_7.0.28_linux.run -extract=`pwd`/nvidia_installers
# accept everything it wants to do 
cd nvidia_installers && sudo ./NVIDIA-Linux-x86_64-346.46.run 
sudo modprobe nvidia
sudo ./cuda-linux64-rel-7.0.28-19326674.run # accept the EULA, accept the defaults
# used space: 3.9 gb
​
# trasfer cuDNN over from elsewhere (you can't download it directly)
cd /data && tar -xzf cudnn-6.5-linux-x64-v2.tgz 
sudo cp cudnn-6.5-linux-x64-v2/libcudnn* /usr/local/cuda/lib64 && sudo cp cudnn-6.5-linux-x64-v2/cudnn.h /usr/local/cuda/include/
​
​
# OPTIONAL
# To increase free space, remove cuda install file & nvidia_installers
cd /data
rm -v cuda_7.0.28_linux.run
rm -rfv nvidia_installers/
​
​
# update to java 8 -- is this the best way to do this?
sudo add-apt-repository ppa:webupd8team/java
sudo apt-get update && sudo apt-get install oracle-java8-set-default
​
​
# install Bazel
cd /data && git clone https://github.com/bazelbuild/bazel.git
# note you can check the tags with git tag -l, you need at least 0.2.0
cd bazel && git checkout tags/0.2.1 && ./compile.sh
sudo cp output/bazel /usr/bin
# used space: 4.5 gb
​
# add CUDA stuff to your ~/.bashrc
echo 'export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/usr/local/cuda/lib64"' >> ~/.bashrc
echo 'export CUDA_HOME=/usr/local/cuda' >> ~/.bashrc
# it appears as thought the default install location is not in the LD Library path for whatever the fuck reason, so 
# modify your bashrc again with:
echo 'export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:/usr/local/lib"' >> ~/.bashrc
# then source it
source ~/.bashrc
​
# NOTES:
# this is only necessary if you will be bazel-build'ing new models, since you have to protoc their compilers, too.
​
# while they instal protocol buffers for you, you need protocol buffer compiler > 3.0.0 alpha so let's get that too (blarg)
cd /data
wget https://github.com/google/protobuf/releases/download/v3.0.0-beta-2/protobuf-python-3.0.0-beta-2.tar.gz
tar xvzf protobuf-python-3.0.0-beta-2.tar.gz
cd protobuf-3.0.0-beta-2 && ./configure && make -j8 && sudo make install
​
# used space: 
​
# install tensorflow / tensorflow serving
cd /data && git clone --recurse-submodules https://github.com/neon-lab/aquila_serving.git
cd aquila_serving/aquila && git checkout mod_for_serving && cd ..
# IMPORTANT:
# Tensorflow Serving will fail to build on AWS w/ CUDA without editing
# ./tensorflow/third_party/gpus/crosstool/CROSSTOOL
# add the line:
# cxx_builtin_include_directory: "/usr/local/cuda-7.0/include"
cd tensorflow
# configure tensorflow; unofficial settings are necessary given the GRID compute cap of 3.0
TF_UNOFFICIAL_SETTING=1 ./configure  # accept the defaults; build with gpu support; set the compute capacity to 3.0
cd ..
​
# assemble Aquila's *_pb2.py files
# NOTES:
# You may have to repeat this if you're going to be instantiating new .proto files.
# navigate to the directory which contains the .proto files
cd tensorflow_serving/aquila/
protoc -I ./ --python_out=. --grpc_out=. --plugin=protoc-gen-grpc=`which grpc_python_plugin` ./aquila_inference.proto
​
# Build TF-Serving
cd /data/aquila_serving
# build the whole source tree - this will take a bit
bazel --output_base=/data/.cache/bazel/_bazel_$USER build -c opt --config=cuda tensorflow_serving/...
​
​
# convert tensorflow into a pip repo
cd tensorflow
bazel --output_base=/data/.cache/bazel/_bazel_$USER build -c opt --config=cuda //tensorflow/tools/pip_package:build_pip_package
bazel-bin/tensorflow/tools/pip_package/build_pip_package /tmp/tensorflow_pkg
# install it with pip for some reason
​
# this filename may change.
sudo pip install /tmp/tensorflow_pkg/tensorflow-0.7.1-py2-none-linux_x86_64.whl
​
# clone inception
curl -O http://download.tensorflow.org/models/image/imagenet/inception-v3-2016-03-01.tar.gz
tar xzf inception-v3-2016-03-01.tar.gz
bazel-bin/tensorflow_serving/example/inception_export --checkpoint_dir=inception-v3 --export_dir=inception-export
​
# aquila:
# real  0m13.621s
# user  0m0.994s
# sys   0m0.161s
​
# this assumes you have an images directory with a text file called batch in it.
bazel-bin/tensorflow_serving/example/inception_inference --port=9000 inception-export &> inception_log &
bazel-bin/tensorflow_serving/example/inception_client --server=localhost:9000 --prep_method=resize --concurrency=10 --image_list_file=/data/images/batch
​
​
# let's test it with mnist
bazel-bin/tensorflow_serving/example/mnist_inference_2 --port=9000 /tmp/monitored &> mnist_log &
bazel-bin/tensorflow_serving/example/mnist_client --num_tests=1000 --server=localhost:9000 --concurrency=10
​
​
# test image;
# https://raw.githubusercontent.com/google/inception/master/dog.jpg
​
# (it's a dalmation)
# the correct classification should be:
9.371209 : dalmatian, coach dog, carriage dog
3.357578 : ocarina, sweet potato
3.080092 : kuvasz
2.961285 : Bernese mountain dog
2.452607 : basketball
​
# real    0m8.205s
# user    0m0.928s
# sys     0m0.111s
​
# %s Inference:
#       32630.767578 : shopping basket
#       31133.976562 : guillotine
#       30024.972656 : daisy
#       29822.853516 : corn
#       29509.896484 : Yorkshire terrier
# %s Inference:
#         32630.767578 : shopping basket
#         31133.976562 : guillotine
#         30024.972656 : daisy
#         29822.853516 : corn
#         29509.896484 : Yorkshire terrier
# %s Inference:
#         32630.767578 : shopping basket
#         31133.976562 : guillotine
#         30024.972656 : daisy
#         29822.853516 : corn
#         29509.896484 : Yorkshire terrier
​
​
# getting closer!
​
# /data/images/dog.jpg Inference:
#         7.105422 : American Staffordshire terrier, Staffordshire terrier, American pit bull terrier, pit bull terrier
#         6.657583 : Labrador retriever
#         6.477783 : bull mastiff
#         6.417630 : Great Dane
#         5.608005 : boxer
# /data/images/dog1.jpg Inference:
#         7.105422 : American Staffordshire terrier, Staffordshire terrier, American pit bull terrier, pit bull terrier
#         6.657583 : Labrador retriever
#         6.477783 : bull mastiff
#         6.417630 : Great Dane
#         5.608005 : boxer
# /data/images/dog2.jpg Inference:
#         7.105422 : American Staffordshire terrier, Staffordshire terrier, American pit bull terrier, pit bull terrier
#         6.657583 : Labrador retriever
#         6.477783 : bull mastiff
#         6.417630 : Great Dane
#         5.608005 : boxer
# /data/images/dog3.jpg Inference:
#         7.105422 : American Staffordshire terrier, Staffordshire terrier, American pit bull terrier, pit bull terrier
#         6.657583 : Labrador retriever
#         6.477783 : bull mastiff
#         6.417630 : Great Dane
#         5.608005 : boxer
​
​