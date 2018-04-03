#!/bin/bash

# This script is used to load data into memcached server

scriptname=`basename "$0"`
if [[ "$#" -ne 7 ]]; then
    echo "Usage: $scriptname <server> <key-min> <key-max> <data-size> " \
         "<iterations> <skew_bench> <prefix: arachne/origin>"
    echo "Example: $scriptname n1 100000 200000 32 5 workloads/Skew.bench arachne"
    exit
fi

# Go to the correct directory
dirPATH=$(dirname $(dirname $(readlink -f $0)))
cd ${dirPATH}

server=$1
keymin=$2
keymax=$3
datasize=$4
iters=$5
benchfile=$6
prefix=$7

clients=16
threads=32
ratio="0:1"
pipeline=100
irdist="POISSON"

logdir=${prefix}_iters${iters}_logs
qpsprefix="qps"
latencyprefix="latency"
runlog=${prefix}_iters${iters}_runlog.log
echo "Saving logs to: $log_dir"
mkdir -p $logdir
rm $logdir/* # Clear previous logs

# Execute experiments multiple times
for iter in `seq 1 $iters`;
do
    logqpsfile=${qpsprefix}_iter${iter}.csv
    latencyfile=${latencyprefix}_iter${iter}.csv
    cmd="./memtier_benchmark -s $server -p 11211 -P memcache_binary \
        --clients $clients --threads $threads --ratio $ratio \
        --pipeline=$pipeline \
        --key-minimum=$keymin --key-maximum=$keymax \
        --data-size=$datasize --random-data --hide-histogram \
        --key-pattern=R:R --run-count=1 --distinct-client-seed --randomize \
        --test-time=1 -b \
        --config-file=$benchfile --ir-dist=$irdist --log-dir=$logdir \
        --log-qpsfile=$logqpsfile --log-latencyfile=$latencyfile"

    # execute the commnad
    echo $cmd
    $cmd >> $runlog 2>&1
done

# Move throughput logs and latency logs to different directory
cd ${dirPATH}
qpsdir=$logdir/throughput
latencydir=$logdir/latency
mkdir -p $qpsdir
mkdir -p $latencydir

# Clean those directories
rm $qpsdir/*
rm $latencydir/*
mv $logdir/${qpsprefix}_iter* $qpsdir
mv $logdir/${latencyprefix}_iter* $latencydir

# Merge the data
cmd="scripts/mergeStats.py ${qpsdir} ${prefix}_iters${iters}_qps"
echo $cmd
$cmd >> $runlog 2>&1

cmd="scripts/mergeStats.py ${latencydir} ${prefix}_iters${iters}_latency"
echo $cmd
$cmd >> $runlog 2>&1
