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

You need two directories for two branches: the `master` branch and `skewSynthetic`
branch. We will use `master` branch to prepare dataset, and use `skewSynthetic`
branch to do experiments.

0. Clone this repo, switch to master branch, and compile.

```
	MEMTIER_DIR=${HOME}/memtier_benchmark
	git clone https://github.com/PlatformLab/memtier_skewsyn.git ${MEMTIER_DIR}
	cd ${MEMTIER_DIR}
	git fetch
	git checkout master
	autoreconf -ivf
	./configure
	make
```

1. Then clone the skewSynthetic branch.

```
	MEMTIER_SKEWSYN_DIR=${HOME}/memtier_skewsyn
	git clone https://github.com/PlatformLab/memtier_skewsyn.git ${MEMTIER_SKEWSYN_DIR}
	cd ${MEMTIER_SKEWSYN_DIR}
	git fetch
	git checkout skewSynthetic
```

2. Recursively clone [Arachne super repository](https://github.com/PlatformLab/arachne-all)
inside memtier_skewsyn top level directory.
```
     git clone --recursive https://github.com/PlatformLab/arachne-all.git ${MEMTIER_SKEWSYN_DIR}/arachne-all
```

3. Build the Arachne library with `./buildAll.sh` in the top level directory. We only use PerfUtil part of it,
so no need to start core arbiter on the client machine.
```
    cd ${MEMTIER_SKEWSYN_DIR}/arachne-all
    ./buildAll.sh
```

4. Build memtier_skewsyn in top level directory
```
	autoreconf -ivf
	./configure
	make
```

5. Run benchmarks:
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

2) Colocation benchmark, uses 2 (or more) client machines:
```
./runSynthetic.sh <server> <key-min> <key-max> <data-size> <iterations> <synthetic_bench> <num-videos> <prefix: arachne/origin> [list of clients...]
```
For example:
```
./runSynthetic.sh ${server} 1000000 9000000 200 1 workloads/Synthetic16.bench 0 arachne_0vid ${client1}
```
