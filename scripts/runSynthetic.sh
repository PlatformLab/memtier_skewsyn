#!/bin/bash

# This script is used to run synthetic workload
cleanup_coretrace()
{
    server=$1
    echo "Cleanup corestats on the server"
    cmd="kill -SIGUSR2 \$(pidof memcached)"
    sshcmd="ssh -p 22 $server \"nohup $cmd > /dev/null 2>&1 < /dev/null &\""
    echo $sshcmd
    ssh -p 22 $server "nohup $cmd > /dev/null 2>&1 < /dev/null & "
}

scriptname=`basename "$0"`
if [[ "$#" -lt 8 ]]; then
    echo "Usage: $scriptname <server> <key-min> <key-max> <data-size>" \
         "<iterations> <synthetic_bench> <video[0/1]> <prefix: arachne/origin>" \
         "[list of clients...]"
    echo "Example: $scriptname n1 1000000 9000000 200 1 workloads/Synthetic16.bench 0 arachne_0vid n3"
    exit
fi

# Go to the correct directory
dirPATH=$(dirname $(dirname $(readlink -f $0)))
scriptPATH=$(dirname $(readlink -f $0))
cd ${dirPATH}

videoPATH=${scriptPATH}/videoScripts

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
rm -f $runlog

# Load data into Memcached and warm up
cmd="bash $scriptPATH/loadonly.sh $server $keymin $keymax $datasize $keyprefix"
echo $cmd
$cmd >> $runlog 2>&1

# read -p "Press enter to continue. (may need to cleanup coretrace on server.)"
# Wait the memcached fully ramped down.
sleep 5
cleanup_coretrace $server

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
    fi

    # Start the main master node
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
        --videos=$videos --video-path=${videoPATH}"

    # execute the commnad
    echo $cmd
    $cmd 2>&1 | tee -a $runlog
done

cleanup_coretrace $server

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
