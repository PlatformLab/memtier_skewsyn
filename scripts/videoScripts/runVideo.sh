#!/bin/bash

trap "trap - SIGTERM && kill -- -$$" SIGINT SIGTERM SIGTSTP

echo $$ > /tmp/x264.pid

scriptname=`basename "$0"`
if [[ "$#" -lt 3 ]]; then
    echo "Usage: $scriptname <InputFile.y4m> <prefix> <outputPrefix> <maxIter>"
    exit
fi

iters=1
if [[ "$#" -ge 4 ]]; then
    iters=$4
fi

inputfile=$1
prefix=$2
outPrefix=$3
outfile=$outPrefix.mkv

echo "Running video transcode on ${inputfile}, ${iters} iterations"

for iter in `seq 1 $iters`;
do
    echo "Start iter ${iter}..."
    logfile=${outPrefix}_iter${iter}.log
    tracefile=${outPrefix}_timetrace_iter${iter}.log

    x264 $inputfile -o $outfile --qpslogfile $logfile --timetrace $tracefile --coretrace $prefix

    rm $outfile # We don't need the output file!

    echo "Finish iter ${iter}..."
done

