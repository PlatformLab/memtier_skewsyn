# memtier_skewsyn (Under Construction)

Memcached skew and colocation workload benchmarking tool, modified from
[memtier_benchmark](https://github.com/RedisLabs/memtier_benchmark).
This repo contains two major branches:
the original memtier_benchmark with `loadonly.sh` script (`master` branch);
and the skew & colocation benchmark we used for the paper
(`skewSynthetic` branch).

## Note
If you are going to reproduce the experiments (especially with background video
task), please contact us.

The main differences from memtier_benchmark are:
1. It sends out requests independently without buffering. Original memtier_benchmark
will buffer the pipelined requests and send out together.

2. We support Poisson and Uniform inter request time, plus the support for goal
QPS.

3. We add support of skewed load -- we can offer a different fraction of load on
one of the memcached worker thread.

4. It can start background video processes remotely on the server machine.

### How do I use memtier_skewsyn benchmark?

1. Clone this repo and swith to the skewSynthetic branch.

```
	MEMTIER_SKEWSYN_DIR=${HOME}/memtier_skewsyn
	git clone https://github.com/PlatformLab/memtier_skewsyn.git ${MEMTIER_SKEWSYN_DIR}
	cd ${MEMTIER_SKEWSYN_DIR}
	git fetch
	git checkout skewSynthetic
```

2. Use the `scripts/prepare.sh` to install PerfUtils and compile memtier
```
    ./${MEMTIER_SKEWSYN_DIR}/scripts/prepare.sh
```
It will clone `PerfUtils` into memtier_skewsyn directory and build everything.

3. Run benchmarks:
You can use this benchmark directly from command line, or you can reproduce
our experiments from the scripts in `${MEMTIER_SKEWSYN_DIR}/scripts/` directory.
By default, logs will be saved in `${MEMTIER_SKEWSYN_DIR}/exp_logs`

    1) Skew benchmark, only uses 1 client machine:
    ```
    ./runSkew.sh <server> <key-min> <key-max> <data-size>  <iterations> <skew_bench> <log directory prefix: arachne/origin>
    ```
    For example:
    ```
    ./runSkew.sh ${server} 1000000 9000000 200 1 workloads/Skew16.bench arachne_test
    ```

    2) Colocation benchmark, uses 2 (or more) client machines (we suppose memtier_skewsyn is installed in
    the same path, or machines share the directory via nfs):
    ```
    ./runSynthetic.sh <server> <key-min> <key-max> <data-size> <iterations> <synthetic_bench> <num-videos> <prefix: arachne/origin> [list of clients...]
    ```
    For example:
    ```
    ./runSynthetic.sh ${server} 1000000 9000000 200 1 workloads/Synthetic16.bench 0 arachne_0vid ${client1}
    ```

### How to play with video colocation workload?

In order to reproduce colocation benchmark, you need to install video dependencies,
inluding x264 and the raw video, on your memcached server machine.

0. Put `${MEMTIER_SKEWSYN_DIR}/scripts/videoScripts` directory on your server,
suppose the directory would be ${videoDir}.

1. In `${videoDir`}, install PerfUtils and x264 by running the script:
```
./prepareVideo.sh
source ~/.bashrc
```
This script will download the video into `${videoDir}/input`, and install PerfUtils
in `${videoDir}/PerfUtils`, install nasm and x264 in `${videoDir}/install`.
It will also automatically update the `$PATH` in `~/.bashrc`. By default, logs
will be saved to `${videoDir}/exp_logs`.

2. Test video encoding alone by running:
```
    ./runVideo.sh <InputFile.y4m> <prefix> <outputPrefix> <maxIter>
```
For example:
```
    ./runVideo.sh input/sintel-1280.y4m a exp_logs/test 1
```
This will produce the video baseline data. `exp_logs/test_iter1.log` is the
throughput per 200us, and `exp_logs/test_timetrace_iter1.log` has the
coretrace log (which thread on which core).
