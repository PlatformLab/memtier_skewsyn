#!/bin/bash

# This script is used to run skewed workload

scriptname=`basename "$0"`
if [[ "$#" -lt 7 ]]; then
    echo "Usage: $scriptname <server> <key-min> <key-max> <data-size> " \
         "<iterations> <skew_bench> <logDirPrefix>"
    echo "Example: $scriptname n1 1000000 9000000 200 1 workloads/Skew16.bench arachne_test"
    exit
fi

# Go to the correct directory
dirPATH=$(dirname $(dirname $(readlink -f $0)))
scriptPATH=$(dirname $(readlink -f $0))
cd ${dirPATH}

server=$1
keymin=$2
keymax=$3
datasize=$4
iters=$5
benchfile=$6
prefix=$7

clients=32
threads=16
ratio="0:1"
pipeline=100
irdist="POISSON"
keyprefix="memtierxxxxxxxxxxxxxxx-" # Pad key to achieve 30-byte
keypattern="G:G"

videos=0 # No background videos

logdir=exp_logs/${prefix}_iters${iters}_skew_logs
qpsprefix="qps"
latencyprefix="latency"
runlog=exp_logs/${prefix}_iters${iters}_skew_runlog.log
echo "Saving logs to: $logdir"
mkdir -p $logdir
rm -rf $logdir/* # Clear previous logs
rm -f $runlog

# Load data into Memcached and warm up
cmd="bash ${scriptPATH}/loadonly.sh $server $keymin $keymax $datasize $keyprefix"
echo $cmd
$cmd >> $runlog 2>&1

read -p "Press enter to continue"

# Execute experiments multiple times
for iter in `seq 1 $iters`;
do

    logqpsfile=${qpsprefix}_iter${iter}.csv
    cmd="./memtier_benchmark -s $server -p 11211 -P memcache_binary \
        --clients $clients --threads $threads --ratio $ratio \
        --pipeline=$pipeline \
        --key-minimum=$keymin --key-maximum=$keymax \
        --data-size=$datasize --random-data --hide-histogram \
        --key-prefix=$keyprefix \
        --key-pattern=$keypattern --run-count=1 --distinct-client-seed --randomize \
        --test-time=1 -b \
        --config-file=$benchfile --ir-dist=$irdist --log-dir=$logdir \
        --log-qpsfile=$logqpsfile \
        --videos=$videos"

    # execute the commnad
    echo $cmd
    $cmd 2>&1 | tee -a $runlog
done

# Move throughput logs and latency logs to different directory
cd ${dirPATH}
qpsdir=$logdir/throughput
latencydir=$logdir/latency
mkdir -p $qpsdir
mkdir -p $latencydir

# Clean those directories
rm -f $qpsdir/*
rm -f $latencydir/*
mv $logdir/${qpsprefix}_iter* $qpsdir > /dev/null 2>&1
mv $logdir/${latencyprefix}_iter* $latencydir > /dev/null 2>&1

# Merge the data
#cmd="scripts/mergeStats.py ${qpsdir} ${prefix}_iters${iters}_qps"
#echo $cmd
#$cmd >> $runlog 2>&1
#
#cmd="scripts/mergeStats.py ${latencydir} ${prefix}_iters${iters}_latency"
#echo $cmd
#$cmd >> $runlog 2>&1
