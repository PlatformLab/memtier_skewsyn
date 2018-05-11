#!/bin/bash

# This script is used to run synthetic workload

scriptname=`basename "$0"`
if [[ "$#" -lt 8 ]]; then
    echo "Usage: $scriptname <server> <key-min> <key-max> <data-size>" \
         "<iterations> <synthetic_bench> <num-videos> <prefix: arachne/origin>" \
         "[list of clients...]"
    echo "Example: $scriptname n1 100000 200000 32 5 workloads/LoadEstimator.bench 1 arachne n3 n4..."
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
videos=$7
prefix=$8

clients=20
threads=16
ratio="0:1"
pipeline=10
irdist="POISSON"
keyprefix="memtierxxxxxxxxxxxxxxx-" # Pad key to achieve 30-byte
keypattern="G:G"

logdir=exp_logs/${prefix}_iters${iters}_synthetic_logs
qpsprefix="qps"
latencyprefix="latency"
runlog=exp_logs/${prefix}_iters${iters}_synthetic_runlog.log
echo "Saving logs to: $logdir"
mkdir -p $logdir
rm -rf $logdir/* # Clear previous logs
rm $runlog

# Load data into Memcached and warm up
cmd="bash $scriptPATH/loadonly.sh $server $keymin $keymax $datasize $keyprefix"
echo $cmd
$cmd >> $runlog 2>&1

read -p "Press enter to continue"

# Execute experiments multiple times
for iter in `seq 1 $iters`;
do
    # $9... will be client addresses, start clients!
    if [[ "$#" -gt 8 ]]; then
        # shift previous ones
        for argn in `seq 1 8`;
        do
            shift
        done

        clientlogdir=$dirPATH/$logdir
        clientbench=$dirPATH/$benchfile
        mkdir -p $clientlogdir
        rm -rf $clientlogdir/*

        for client in "$@";
        do
            echo "Starting client: $client ..."
            clientqpsfile=${qpsprefix}_iter${iter}_${client}.csv
            clientRunLog=$dirPATH/${runlog}_${client}
            cmd="$dirPATH/memtier_benchmark -s $server \
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
            sshcmd="ssh -p 22 $client \"nohup $cmd > $clientRunLog 2>&1 < /dev/null &\""
            echo $sshcmd
            ssh -p 22 $client "nohup $cmd > $clientRunLog 2>&1 < /dev/null & "
        done
        # Change parameters for the master
        clients=1
        threads=8
        pipeline=1
        irdist="UNIFORM"
        benchfile=workloads/Synthetic16-master.bench
        #benchfile=workloads/Synthetic16_1sec-master.bench
    fi

    # Master usually run in non-blocking mode
    logqpsfile=${qpsprefix}_iter${iter}.csv
    latencyfile=${latencyprefix}_iter${iter}.csv
    cmd="./memtier_benchmark -s $server -p 11211 -P memcache_binary \
        --clients $clients --threads $threads --ratio $ratio \
        --pipeline=$pipeline \
        --key-prefix=$keyprefix \
        --key-minimum=$keymin --key-maximum=$keymax \
        --data-size=$datasize --random-data --hide-histogram \
        --key-pattern=$keypattern --run-count=1 \
        --distinct-client-seed --randomize \
        --test-time=1 -b \
        --config-file=$benchfile --ir-dist=$irdist --log-dir=$logdir \
        --log-qpsfile=$logqpsfile --log-latencyfile=$latencyfile \
        --videos=$videos"

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
#cmd="scripts/mergeStats.py ${qpsdir} ${prefix}_iters${iters}_qps"
#echo $cmd
#$cmd >> $runlog 2>&1
#
#cmd="scripts/mergeStats.py ${latencydir} ${prefix}_iters${iters}_latency"
#echo $cmd
#$cmd >> $runlog 2>&1
