#!/bin/bash

# This script is used to load data into memcached server

scriptname=`basename "$0"`
if [[ "$#" -lt 4 ]]; then
    echo "Usage: $scriptname <server> <key-min> <key-max> <data-size> (<key-prefix>)"
    echo "Example: $scriptname n1 100000 200000 32 memtier-"
    exit
fi

# Go to the correct directory
dirPATH=$(dirname $(dirname $(readlink -f $0)))
cd ${dirPATH}

server=$1
keymin=$2
keymax=$3
datasize=$4
keyprefix="memtier-"
if [[ "$#" -ge 5 ]]; then
keyprefix=$5
fi
threads=16

let "reqs = ($keymax - $keymin) / 16 + 1"

cmd="./memtier_benchmark -s $server -p 11211 -P memcache_binary --clients=1 \
    --threads ${threads} --ratio 1:0 --run-count=1 --pipeline=100 \
    --key-minimum=$keymin --key-maximum=$keymax \
    --data-size=$datasize --random-data --hide-histogram \
    --key-pattern=P:P --requests=$reqs --key-prefix=$keyprefix"

# execute the commnad
echo $cmd
$cmd

# Then warm up the dataset
let "reqs = $reqs * 2"
cmd="./memtier_benchmark -s $server -p 11211 -P memcache_binary --clients=1 \
    --threads ${threads} --ratio 0:1 --run-count=1 --pipeline=100 \
    --key-minimum=$keymin --key-maximum=$keymax --key-prefix=$keyprefix \
    --data-size=$datasize --random-data --hide-histogram \
    --key-pattern=P:P --requests=$reqs"

#execute the command
echo $cmd
$cmd
