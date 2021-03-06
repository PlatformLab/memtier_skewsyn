# memtier_skewsyn (Under Construction)

Memcached skew and colocation workload benchmarking tool, modified from
[memtier_benchmark](https://github.com/RedisLabs/memtier_benchmark).
This repo contains two major branches:
the original memtier_benchmark with `loadonly.sh` script (`master` branch);
and the skew & colocation benchmark we used for the paper
(`skewSynthetic` branch).

## Note
This repo is under construction, tests may be failing.
If you are going to reproduce the experiments (especially with background video
task), please contact us if you encountered any problem.

The main differences from memtier_benchmark are:
1. It sends out requests independently without buffering. Original memtier_benchmark
will buffer the pipelined requests and send out together.

2. We support Poisson and Uniform inter request time, plus the support for goal
QPS.

3. We add support of skewed load -- we can offer a different fraction of load on
one of the memcached worker thread.

4. It can start background video processes remotely on the server machine.

### How do I use memtier_skewsyn benchmark?

1. Clone this repo to your target directory ${MEMTIER_SKEWSYN_DIR}.
The default branch is `skewSynthetic` branch.

    ```
	git clone https://github.com/PlatformLab/memtier_skewsyn.git ${MEMTIER_SKEWSYN_DIR}
	cd ${MEMTIER_SKEWSYN_DIR}
    ```

2. Use the `scripts/prepare.sh` to install PerfUtils and compile memtier
    ```
    ./scripts/prepare.sh
    ```
    It will clone `PerfUtils` into memtier_skewsyn directory and build everything.

3. Run benchmarks:
You can use this benchmark directly from command line, or you can reproduce
our experiments from the scripts in `${MEMTIER_SKEWSYN_DIR}/scripts/` directory.
By default, logs will be saved in `${MEMTIER_SKEWSYN_DIR}/exp_logs`

    1) Skew benchmark, only uses 1 client machine:
    ```
    ./runSkew.sh <server> <key-min> <key-max> <data-size>  <iterations> <skew_bench> <log directory prefix>
    ```
    For example:
    ```
    ./runSkew.sh ${server} 1000000 9000000 200 1 workloads/Skew16.bench arachne_test
    ```

    2) Colocation benchmark, uses 2 (or more) client machines (we suppose memtier_skewsyn is installed in
    the same path, or machines share the directory via nfs):
    ```
    ./runSynthetic.sh <server> <key-min> <key-max> <data-size> <iterations> <synthetic_bench> <video[0/1]> <log directory prefix> [list of clients...]
    ```
    For example:
    ```
    ./runSynthetic.sh ${server} 1000000 9000000 200 1 workloads/Synthetic16.bench 0 arachne_0vid ${client1}
    ```
    If you set `<video>` option to 0, it will run without background video processes.
    If you are interested in the video colocation workload, please follow
    instructions in the [following section](#how-to-play-with-video-colocation-workload).

4. Parse logs:

    For skew benchmark, the logs locate at
    `${MEMTIER_SKEWSYN_DIR}/exp_logs/<log directory prefix>_iters<iterations>_skew_logs/`
    directory.

    For colocation benchmark, the logs are at
    `${MEMTIER_SKEWSYN_DIR}/exp_logs/<log directory prefix>_iters<iterations>_synthetic_logs/`

    Inside the directory there are two directories: `latency/` and `throughput/`.
    `latency/` contains the latency log files on the main client machine.
    `throughput/` has throughput logs for both main client machine and secondary client
    machines. The structures are as follows:

    ```
    latency/ (skew workload does not record latencies)
    |---- latency_iter1.csv
    |---- latency_iter2.csv
    |...

    throughput/
    |---- qps_iter1.csv
    |---- qps_iter1_${client1}.csv (for colocation workload)
    |---- qps_iter1_${client2}.csv (for colocation workload)
    |...
    ```

### How to play with video colocation workload?

In order to reproduce colocation benchmark, you need to install video dependencies,
inluding x264 and the raw video, on your memcached server machine.

0. Put `${MEMTIER_SKEWSYN_DIR}/scripts/videoScripts` directory on your server machine,
suppose the directory would be `${videoDir}`.

1. In `${videoDir`}, install PerfUtils and x264 by running the script:
    ```
    ./prepareVideo.sh
    source ~/.bashrc
    ```
    This script will download the video into `${videoDir}/input`, and install PerfUtils
    in `${videoDir}/PerfUtils`, install nasm and x264 in `${videoDir}/install`.
    It will also automatically update the `$PATH` in `~/.bashrc`. By default, logs
    will be saved to `${videoDir}/exp_logs`.
    This script can take a couple of minutes, especially the video downloading
    part.

2. Test video encoding alone by running:
    ```
    ./runVideo.sh <InputFile.y4m> <prefix> <outputPrefix> <maxIter>
    ```
    For example:
    ```
    ./runVideo.sh input/sintel-1280.y4m a exp_logs/test 1
    ```
    This will produce the video baseline results (video alone, no colocation
    with memcached).  `exp_logs/test_iter1.log` is the throughput logged per
    200us, and `exp_logs/test_timetrace_iter1.log` has the coretrace log (which
    thread on which core).

3. Run colocation benchmark. Our machines are connected via NFS, so the
`videoDir` will be in `${MEMTIER_SKEWSYN_DIR}/scripts/videoScripts` directory.
If you have a different path on your memcached server machine, you need to
modify `videoPATH` parameter in the `runSynthetic.sh` file. Then simply run:
    ```
    ./runSynthetic.sh ${server} 1000000 9000000 200 1 workloads/Synthetic16.bench 1 arachne_1vid ${client1}
    ```
    You will find the corresponding log directories in
    `${videoDir}/exp_logs/<log directory prefix>_iters<iterations>_synthetic_logs/` and
    `${MEMTIER_SKEWSYN_DIR}/exp_logs/<log directory prefix>_iters<iterations>_synthetic_logs/`.
    The log directory structures are the same as we described before.

