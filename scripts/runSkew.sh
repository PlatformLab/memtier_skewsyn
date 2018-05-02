#!/bin/bash

# This script is used to run skewed workload

scriptname=`basename "$0"`
if [[ "$#" -lt 7 ]]; then
    echo "Usage: $scriptname <server> <key-min> <key-max> <data-size> " \
         "<iterations> <skew_bench> <prefix: arachne/origin>" \
         "[list of clients...]"
    echo "Example: $scriptname n1 100000 200000 32 5 workloads/Skew.bench arachne n4..."
    exit
fi

# Go to the correct directory
dirPATH=$(dirname $(dirname $(readlink -f $0)))
cd ${dirPATH}

originalMemtier="$HOME/memtier_benchmark"
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
rm $runlog

# Load data into Memcached and warm up
cmd="bash $originalMemtier/loadonly.sh $server $keymin $keymax $datasize $keyprefix"
echo $cmd
$cmd >> $runlog 2>&1

read -p "Press enter to continue"

# Execute experiments multiple times
for iter in `seq 1 $iters`;
do
    # $9... will be client addresses, start clients!
    if [[ "$#" -gt 7 ]]; then
        # shift previous ones
        for argn in `seq 1 7`;
        do
            shift
        done

        clientlogdir=$HOME/memtier_benchmark_skewsyn/$logdir
        clientbench=$HOME/memtier_benchmark_skewsyn/$benchfile

        for client in "$@";
        do
            echo "Starting client: $client ..."
            clientqpsfile=${qpsprefix}_iter${iter}_${client}.csv
            clientRunLog=$HOME/memtier_benchmark_skewsyn/${runlog}_${client}
            cmd="$HOME/memtier_benchmark_skewsyn/memtier_benchmark -s $server \
                 -p 11211 -P memcache_binary --clients $clients --threads $threads \
                 --ratio $ratio --pipeline=$pipeline \
                 --key-prefix=$keyprefix \
                 --key-minimum=$keymin --key-maximum=$keymax \
                 --data-size=$datasize --random-data --hide-histogram \
                 --key-pattern=$keypattern --run-count=1 \
                 --distinct-client-seed --randomize \
                 --test-time=1 -b \
                 --config-file=$clientbench --ir-dist=$irdist \
                 --log-dir=${clientlogdir} \
                 --log-qpsfile=$clientqpsfile"
            #echo $cmd
            sshcmd="ssh -p 5515 $client \"nohup $cmd > $clientRunLog 2>&1 < /dev/null &\""
            echo $sshcmd
            ssh -p 5515 $client "nohup $cmd > $clientRunLog 2>&1 < /dev/null & "
        done
    fi

    logqpsfile=${qpsprefix}_iter${iter}.csv
    #latencyfile=${latencyprefix}_iter${iter}.csv
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
        #--log-latencyfile=$latencyfile \

    # execute the commnad
    echo $cmd
    # $cmd >> $runlog 2>&1
    $cmd 2>&1 | tee -a $runlog
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
