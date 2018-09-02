#!/bin/bash

## Copy the top level videoScripts directory to the server machine.
## This script will help you to download the video and install x264
## in the same directory as this script.
## Run this on the server machine!
## Video logs will be in the exp_logs/

# 1. install PerfUtils in the same directory
# Get the current memtier_skewsyn installation path
scriptPATH=$(dirname $(readlink -f $0))
cd ${scriptPATH}

PERFUTIL_DIR=${scriptPATH}/PerfUtils
echo "Installing PerfUtils to ${PERFUTIL_DIR}"

git clone https://github.com/PlatformLab/PerfUtils.git ${PERFUTIL_DIR}
pushd PerfUtils
make -j8
popd

# 2. download the video
echo "Downloading the video"

videoPATH=${scriptPATH}/input
mkdir -p ${videoPATH}
wget -O ${videoPATH}/sintel-1280-raw.y4m "https://xiph-media.net/sintel/sintel-1280.y4m"

sudo apt-get install ffmpeg
ffmpeg -i ${videoPATH}/sintel-1280-raw.y4m -filter:v "crop=1280:544:0:0" ${videoPATH}/sintel-1280.y4m

# 3. Install x264 in the same directory
installPATH=${scriptPATH}/install

nasmPATH=${scriptPATH}/nasm-2.13
echo "Installing nasm to ${installPATH}"
mkdir -p ${nasmPATH}
pushd nasm-2.13
wget -P ${scriptPATH} http://www.nasm.us/pub/nasm/releasebuilds/2.13.03/nasm-2.13.03.tar.xz
tar -xf ${scriptPATH}/nasm-2.13.03.tar.xz --strip-components=1
./configure --prefix=${installPATH}
make -j8
make install
popd
rm -rf ${scriptPATH}/nasm-2.13*

export PATH=${installPATH}/bin:$PATH

cat >> $HOME/.bashrc <<EOM
export PATH=${installPATH}/bin:\$PATH
EOM

x264PATH=${scriptPATH}/x264
echo "Installing x264 to ${installPATH}"

git clone https://github.com/PlatformLab/x264.git ${x264PATH}
pushd x264
git fetch
git checkout AddPerSecondLogging
# Default use NICE value.
./Install.sh ${installPATH} ${PERFUTIL_DIR} 1
popd

logDir=${scriptPATH}/exp_logs
mkdir -p ${logDir}
