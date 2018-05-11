#!/bin/bash

## This script will help you to download PerfUtils and build the memtier benchmark

# First, install PerfUtils
# Get the current memtier_skewsyn installation path
MEMTIER_DIR=$(dirname $(dirname $(readlink -f $0)))
cd ${MEMTIER_DIR}

PERFUTIL_DIR=${MEMTIER_DIR}/PerfUtils
echo "Installing PerfUtils to ${PERFUTIL_DIR}"

git clone https://github.com/PlatformLab/PerfUtils.git ${PERFUTIL_DIR}
pushd PerfUtils
make -j8
popd

# Second, build memtier_skewsyn (memtier_benchmark) itself
echo "Compiling memtier_skewsyn"
autoreconf -ivf
./configure
make -j8
