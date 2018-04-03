#!/bin/bash

# This script is used to load data into memcached server

scriptname=`basename "$0"`
if [[ "$#" -ne 4 ]]; then
    echo "Usage: $scriptname <server> <key-min> <key-max> <data-size>"
    echo "Example: $scriptname n1 100000 200000 32"
    exit
fi

server=$1
keymin=$2
keymax=$3
datasize=$4
let "reqs = ($keymax - $keymin) / 16 + 1"

cmd="./memtier_benchmark -s $server -p 11211 -P memcache_binary --clients=1 \
    --threads 16 --ratio 1:0 --run-count=1 --pipeline=100 \
    --key-minimum=$keymin --key-maximum=$keymax \
    --data-size=$datasize --random-data --hide-histogram \
    --key-pattern=P:P --requests=$reqs"

# execute the commnad
echo $cmd
$cmd

# Then warm up the dataset
let "reqs = $reqs * 2"
cmd="./memtier_benchmark -s $server -p 11211 -P memcache_binary --clients=1 \
    --threads 16 --ratio 0:1 --run-count=1 --pipeline=100 \
    --key-minimum=$keymin --key-maximum=$keymax \
    --data-size=$datasize --random-data --hide-histogram \
    --key-pattern=P:P --requests=$reqs"

#execute the command
echo $cmd
$cmd
