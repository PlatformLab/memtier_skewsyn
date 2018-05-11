/*
 * Copyright (C) 2011-2017 Redis Labs Ltd.
 *
 * This file is part of memtier_benchmark.
 *
 * memtier_benchmark is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * memtier_benchmark is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with memtier_benchmark.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <chrono>
#include <iostream>
#include <pthread.h>
#include <random>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <getopt.h>
#include <assert.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <stdexcept>

#include "client.h"
#include "JSON_handler.h"
#include "obj_gen.h"
#include "memtier_benchmark.h"

using PerfUtils::Cycles;

bool master_finished = false;

// QPS for each clieint on each server thread
std::vector<double> qpsPerClient;

// A global array to store SET latencies
uint64_t* setLatencies = NULL;

// Atomic variable to store current index in the SET array
std::atomic<uint32_t> setArrayIndex;

// A global array to store GET latencies
uint64_t* getLatencies = NULL;

// Atomic variable to store current index in the GET array
std::atomic<uint32_t> getArrayIndex;

// The values of arrayIndex for SET/GET array before each change in the load,
// which we can use later to store latencies as well as total throughput.
static std::vector<uint64_t> setIndices;
static std::vector<uint64_t> getIndices;

// An array to store the time stamp  of each interval start, which we can use
// this array to calculate the duration of each time interval
static std::vector<uint64_t> timeStamps;

// Power multiplier for the latency entries
int ARRAY_EXP = 26; // We can record at most 2^26 = 67108864 latencies
size_t MAX_ENTRIES;

struct Interval {
    int64_t timeToRun; // The time (in ns) we spend on this interval
    double requestsPerSecond;
    double skewFactor; // Proportion of the QPS to the first server thread
} *intervals;

static size_t numIntervals; // Num of intervals in the config file

static int log_level = 0;
static double currGoalQPS = 0.0;
static double currentSkew = 0.0;

// Maximum number of iterations of video processes
static int maxIters = 3;

// Convert timeval to uint64_t
static uint64_t timeval_to_ts(struct timeval a)
{
    return (uint64_t)a.tv_sec * 1000000 + (uint64_t)a.tv_usec;
}

void benchmark_log_file_line(int level, const char *filename, unsigned int line, const char *fmt, ...)
{
    if (level > log_level)
        return;
    
    va_list args;    
    char fmtbuf[1024];

    snprintf(fmtbuf, sizeof(fmtbuf)-1, "%s:%u: ", filename, line);
    strcat(fmtbuf, fmt);
    
    va_start(args, fmt);
    vfprintf(stderr, fmtbuf, args);
    va_end(args);
}

void benchmark_log(int level, const char *fmt, ...)
{
    if (level > log_level)
        return;

    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
}

static void start_video_decoding(const char* videoFile, const char* prefix,
                                 struct benchmark_config *cfg) {
    char cmd[1000];
    sprintf(cmd, "ssh -p 22 %s \" nohup mkdir -p /scratch/mydata/logs/%s \" & ",
            cfg->server, cfg->log_dir);
    if (system(cmd) == -1) {
        fprintf(stderr, "Fail to mkdir for logs \n");
        exit(-1);
    } else {
        fprintf(stderr, "Success: mkdir /scrach/mydata/logs/%s \n", cfg->log_dir);
    }

    // Get rid of the extension of the file
    std::string filePrefix(cfg->log_qps_file);
    filePrefix.erase(filePrefix.find_last_of("."), std::string::npos);

    sprintf(cmd, "ssh -p 22 %s "
            "\" nohup /scratch/mydata/scripts/DecodeWithConfig.sh "
            "/scratch/mydata/input/%s %s "
            "/scratch/mydata/logs/%s/%s_%s %d > /dev/null "
            "2> /dev/null < /dev/null &\"",
            cfg->server,
            videoFile, prefix, cfg->log_dir, filePrefix.c_str(), videoFile,
            maxIters);
    fprintf(stderr, "%s \n", cmd);
    if (system(cmd) == -1) {
        fprintf(stderr, "Fail to start video processes \n");
        exit(-1);
    } else {
        fprintf(stderr, "Success: started video decoding %s \n", videoFile);
    }
}

static void stop_video_decoding(struct benchmark_config *cfg) {
    char cmd[1000];

    // First, kill the script
    sprintf(cmd, "ssh -p 22 %s \"nohup kill -9 \\$(cat /tmp/x264.pid)\" &",
            cfg->server);
    fprintf(stderr, "%s \n", cmd);
    if (system(cmd) == -1) {
        fprintf(stderr, "Fail to stop video script \n");
        exit(-1);
    } else {
        fprintf(stderr, "Success: stopped video script. \n");
    }

    // Need to kill all video processes!
    sprintf(cmd, "ssh -p 22 %s \"nohup kill -s SIGINT \\$(pidof x264)\" &",
            cfg->server);
    fprintf(stderr, "%s \n", cmd);
    if (system(cmd) == -1) {
        fprintf(stderr, "Fail to stop video processes \n");
        exit(-1);
    } else {
        fprintf(stderr, "Success: stopped all video decoding processes. \n");
    }
}

static void config_print(FILE *file, struct benchmark_config *cfg)
{
    char tmpbuf[512];
    
    fprintf(file,
        "server = %s\n"
        "port = %u\n"
        "unix socket = %s\n"
        "protocol = %s\n"
        "out_file = %s\n"
        "client_stats = %s\n"
        "run_count = %u\n"
        "debug = %u\n"
        "requests = %u\n"
        "clients = %u\n"
        "threads = %u\n"
        "test_time = %u\n"
        "ratio = %u:%u\n"
        "pipeline = %u\n"
        "data_size = %u\n"
        "data_offset = %u\n"
        "random_data = %s\n"
        "data_size_range = %u-%u\n"
        "data_size_list = %s\n"
        "data_size_pattern = %s\n"
        "expiry_range = %u-%u\n"
        "data_import = %s\n"
        "data_verify = %s\n"
        "verify_only = %s\n"
        "generate_keys = %s\n"
        "key_prefix = %s\n"
        "key_minimum = %llu\n"
        "key_maximum = %llu\n"
        "key_pattern = %s\n"
        "key_stddev = %f\n"
        "key_median = %f\n"
        "reconnect_interval = %u\n"
        "multi_key_get = %u\n"
        "authenticate = %s\n"
        "select-db = %d\n"
        "no-expiry = %s\n"
        "wait-ratio = %u:%u\n"
        "num-slaves = %u-%u\n"
        "wait-timeout = %u-%u\n"
        "json-out-file = %s\n",
        cfg->server,
        cfg->port,
        cfg->unix_socket,
        cfg->protocol,
        cfg->out_file,
        cfg->client_stats,
        cfg->run_count,
        cfg->debug,
        cfg->requests,
        cfg->clients,
        cfg->threads,
        cfg->test_time,
        cfg->ratio.a, cfg->ratio.b,
        cfg->pipeline,
        cfg->data_size,
        cfg->data_offset,
        cfg->random_data ? "yes" : "no",
        cfg->data_size_range.min, cfg->data_size_range.max,
        cfg->data_size_list.print(tmpbuf, sizeof(tmpbuf)-1),
        cfg->data_size_pattern,
        cfg->expiry_range.min, cfg->expiry_range.max,
        cfg->data_import,
        cfg->data_verify ? "yes" : "no",
        cfg->verify_only ? "yes" : "no",
        cfg->generate_keys ? "yes" : "no",
        cfg->key_prefix,
        cfg->key_minimum,
        cfg->key_maximum,
        cfg->key_pattern,
        cfg->key_stddev,
        cfg->key_median,
        cfg->reconnect_interval,
        cfg->multi_key_get,
        cfg->authenticate ? cfg->authenticate : "",
        cfg->select_db,
        cfg->no_expiry ? "yes" : "no",
        cfg->wait_ratio.a, cfg->wait_ratio.b,
        cfg->num_slaves.min, cfg->num_slaves.max,
        cfg->wait_timeout.min, cfg->wait_timeout.max,
        cfg->json_out_file);
}

static void config_print_to_json(json_handler * jsonhandler, struct benchmark_config *cfg)
{
    char tmpbuf[512];
    
    jsonhandler->open_nesting("configuration");  

    jsonhandler->write_obj("server"            ,"\"%s\"",      	cfg->server);
    jsonhandler->write_obj("port"              ,"%u",          	cfg->port);
    jsonhandler->write_obj("unix socket"       ,"\"%s\"",      	cfg->unix_socket);
    jsonhandler->write_obj("protocol"          ,"\"%s\"",      	cfg->protocol);
    jsonhandler->write_obj("out_file"          ,"\"%s\"",      	cfg->out_file);
    jsonhandler->write_obj("client_stats"      ,"\"%s\"",      	cfg->client_stats);
    jsonhandler->write_obj("run_count"         ,"%u",          	cfg->run_count);
    jsonhandler->write_obj("debug"             ,"%u",          	cfg->debug);
    jsonhandler->write_obj("requests"          ,"%u",          	cfg->requests);
    jsonhandler->write_obj("clients"           ,"%u",          	cfg->clients);
    jsonhandler->write_obj("threads"           ,"%u",          	cfg->threads);
    jsonhandler->write_obj("test_time"         ,"%u",          	cfg->test_time);
    jsonhandler->write_obj("ratio"             ,"\"%u:%u\"",   	cfg->ratio.a, cfg->ratio.b);
    jsonhandler->write_obj("pipeline"          ,"%u",          	cfg->pipeline);
    jsonhandler->write_obj("data_size"         ,"%u",          	cfg->data_size);
    jsonhandler->write_obj("data_offset"       ,"%u",          	cfg->data_offset);
    jsonhandler->write_obj("random_data"       ,"\"%s\"",      	cfg->random_data ? "true" : "false");
    jsonhandler->write_obj("data_size_range"   ,"\"%u:%u\"",	cfg->data_size_range.min, cfg->data_size_range.max);
    jsonhandler->write_obj("data_size_list"    ,"\"%s\"",   	cfg->data_size_list.print(tmpbuf, sizeof(tmpbuf)-1));
    jsonhandler->write_obj("data_size_pattern" ,"\"%s\"", 		cfg->data_size_pattern);
    jsonhandler->write_obj("expiry_range"      ,"\"%u:%u\"",   	cfg->expiry_range.min, cfg->expiry_range.max);
    jsonhandler->write_obj("data_import"       ,"\"%s\"",       cfg->data_import);
    jsonhandler->write_obj("data_verify"       ,"\"%s\"",       cfg->data_verify ? "true" : "false");
    jsonhandler->write_obj("verify_only"       ,"\"%s\"",       cfg->verify_only ? "true" : "false");
    jsonhandler->write_obj("generate_keys"     ,"\"%s\"",     	cfg->generate_keys ? "true" : "false");
    jsonhandler->write_obj("key_prefix"        ,"\"%s\"",       cfg->key_prefix);
    jsonhandler->write_obj("key_minimum"       ,"%11u",        	cfg->key_minimum);
    jsonhandler->write_obj("key_maximum"       ,"%11u",        	cfg->key_maximum);
    jsonhandler->write_obj("key_pattern"       ,"\"%s\"",       cfg->key_pattern);
    jsonhandler->write_obj("key_stddev"        ,"%f",           cfg->key_stddev);
    jsonhandler->write_obj("key_median"        ,"%f",           cfg->key_median);
    jsonhandler->write_obj("reconnect_interval","%u",    		cfg->reconnect_interval);
    jsonhandler->write_obj("multi_key_get"     ,"%u",         	cfg->multi_key_get);
    jsonhandler->write_obj("authenticate"      ,"\"%s\"",      	cfg->authenticate ? cfg->authenticate : "");
    jsonhandler->write_obj("select-db"         ,"%d",           cfg->select_db);
    jsonhandler->write_obj("no-expiry"         ,"\"%s\"",       cfg->no_expiry ? "true" : "false");
    jsonhandler->write_obj("wait-ratio"        ,"\"%u:%u\"",    cfg->wait_ratio.a, cfg->wait_ratio.b);
    jsonhandler->write_obj("num-slaves"        ,"\"%u:%u\"",    cfg->num_slaves.min, cfg->num_slaves.max);
    jsonhandler->write_obj("wait-timeout"      ,"\"%u-%u\"",   	cfg->wait_timeout.min, cfg->wait_timeout.max);

    jsonhandler->close_nesting();
}

static void config_init_defaults(struct benchmark_config *cfg)
{
    if (!cfg->server && !cfg->unix_socket)
        cfg->server = "localhost";
    if (!cfg->port && !cfg->unix_socket)
        cfg->port = 6379;
    if (!cfg->protocol)
        cfg->protocol = "redis";
    if (!cfg->run_count)
        cfg->run_count = 1;
    if (!cfg->clients)
        cfg->clients = 50;
    if (!cfg->threads)
        cfg->threads = 4;
    if (!cfg->ratio.is_defined())
        cfg->ratio = config_ratio("1:10");
    if (!cfg->pipeline)
        cfg->pipeline = 1;
    if (!cfg->data_size && !cfg->data_size_list.is_defined() && !cfg->data_size_range.is_defined() && !cfg->data_import)
        cfg->data_size = 32;
    if (cfg->generate_keys || !cfg->data_import) {
        if (!cfg->key_prefix)
            cfg->key_prefix = "memtier-";
        if (!cfg->key_maximum)
            cfg->key_maximum = 10000000;
    }
    if (!cfg->key_pattern)
        cfg->key_pattern = "R:R";
    if (!cfg->data_size_pattern)
        cfg->data_size_pattern = "R";
    if (cfg->requests == (unsigned int)-1) {
        cfg->requests = cfg->key_maximum - cfg->key_minimum;
        if (strcmp(cfg->key_pattern, "P:P")==0)
            cfg->requests = cfg->requests / (cfg->clients * cfg->threads) + 1;
        printf("setting requests to %d\n", cfg->requests);
    }
    if (!cfg->requests && !cfg->test_time)
        cfg->requests = 10000;
}

static int generate_random_seed()
{
    int R;
    FILE* f = fopen("/dev/random", "r");
    if (f)
    {
        size_t ignore = fread(&R, sizeof(R), 1, f);
        fclose(f);
        ignore++;//ignore warning
    }
    
    return (int)time(NULL)^getpid()^R;
}

static bool verify_cluster_option(struct benchmark_config *cfg) {
    if (cfg->reconnect_interval) {
        fprintf(stderr, "error: cluster mode dose not support reconnect-interval option.\n");
        return false;
    } else if (cfg->multi_key_get) {
        fprintf(stderr, "error: cluster mode dose not support multi-key-get option.\n");
        return false;
    } else if (cfg->wait_ratio.is_defined()) {
        fprintf(stderr, "error: cluster mode dose not support wait-ratio option.\n");
        return false;
    } else if (cfg->protocol && strcmp(cfg->protocol, "redis")) {
        fprintf(stderr, "error: cluster mode supported only in redis protocol.\n");
        return false;
    }

    return true;
}

static int config_parse_args(int argc, char *argv[], struct benchmark_config *cfg)
{
    enum extended_options {
        o_test_time = 128,
        o_ratio,
        o_pipeline,
        o_data_size_range,
        o_data_size_list,
        o_data_size_pattern,
        o_data_offset,
        o_expiry_range,
        o_data_import,
        o_data_verify,
        o_verify_only,
        o_key_prefix,
        o_key_minimum,
        o_key_maximum,
        o_key_pattern,
        o_key_stddev,
        o_key_median,
        o_show_config,
        o_hide_histogram,
        o_distinct_client_seed,
        o_randomize,
        o_client_stats,
        o_reconnect_interval,
        o_generate_keys,
        o_multi_key_get,
        o_select_db,
        o_no_expiry,
        o_wait_ratio,
        o_num_slaves,
        o_wait_timeout, 
        o_json_out_file,
        o_cluster_mode,
        o_server_threads,
        o_config_file,
        o_ir_distribution,
        o_log_dir,
        o_log_qps_file,
        o_log_latency_file,
        o_videos
    };
    
    static struct option long_options[] = {
        { "server",                     1, 0, 's' },
        { "port",                       1, 0, 'p' },
        { "unix-socket",                1, 0, 'S' },
        { "protocol",                   1, 0, 'P' },
        { "out-file",                   1, 0, 'o' },
        { "client-stats",               1, 0, o_client_stats },
        { "run-count",                  1, 0, 'x' },
        { "debug",                      0, 0, 'D' },
        { "show-config",                0, 0, o_show_config },
        { "hide-histogram",             0, 0, o_hide_histogram },
        { "distinct-client-seed",       0, 0, o_distinct_client_seed },
        { "randomize",                  0, 0, o_randomize },
        { "requests",                   1, 0, 'n' },
        { "clients",                    1, 0, 'c' },
        { "threads",                    1, 0, 't' },        
        { "test-time",                  1, 0, o_test_time },
        { "ratio",                      1, 0, o_ratio },
        { "pipeline",                   1, 0, o_pipeline },
        { "data-size",                  1, 0, 'd' },
        { "data-offset",                1, 0, o_data_offset },
        { "random-data",                0, 0, 'R' },
        { "data-size-range",            1, 0, o_data_size_range },
        { "data-size-list",             1, 0, o_data_size_list },
        { "data-size-pattern",          1, 0, o_data_size_pattern },
        { "expiry-range",               1, 0, o_expiry_range },
        { "data-import",                1, 0, o_data_import },
        { "data-verify",                0, 0, o_data_verify },
        { "verify-only",                0, 0, o_verify_only },
        { "generate-keys",              0, 0, o_generate_keys },
        { "key-prefix",                 1, 0, o_key_prefix },
        { "key-minimum",                1, 0, o_key_minimum },
        { "key-maximum",                1, 0, o_key_maximum },
        { "key-pattern",                1, 0, o_key_pattern },
        { "key-stddev",                 1, 0, o_key_stddev },
        { "key-median",                 1, 0, o_key_median },
        { "reconnect-interval",         1, 0, o_reconnect_interval },
        { "multi-key-get",              1, 0, o_multi_key_get },
        { "authenticate",               1, 0, 'a' },
        { "select-db",                  1, 0, o_select_db },
        { "no-expiry",                  0, 0, o_no_expiry },
        { "wait-ratio",                 1, 0, o_wait_ratio },
        { "num-slaves",                 1, 0, o_num_slaves },
        { "wait-timeout",               1, 0, o_wait_timeout },
        { "json-out-file",              1, 0, o_json_out_file },
        { "cluster-mode",                0, 0, o_cluster_mode },
        { "help",                       0, 0, 'h' },
        { "version",                    0, 0, 'v' },
        { "blocking",                   0, 0, 'b' },
        { "skew-level",                 1, 0, 'k'},
        { "server-threads",             1, 0, o_server_threads },
        { "config-file",                1, 0, o_config_file},
        { "ir-dist",                    1, 0, o_ir_distribution},
        { "log-dir",                    1, 0, o_log_dir},
        { "log-qpsfile",                1, 0, o_log_qps_file},
        { "log-latencyfile",            1, 0, o_log_latency_file},
        { "videos",                     1, 0, o_videos},
        { NULL,                         0, 0, 0 }
    };

    int option_index;
    int c;
    char *endptr;
    while ((c = getopt_long(argc, argv, 
                "s:S:p:P:o:x:DRn:c:t:d:a:hbk:", long_options, &option_index)) != -1)
    {
        switch (c) {
                case 'h':
                    return -1;
                    break;
                case 'v':
                    puts(PACKAGE_STRING);
                // FIXME!!
                    puts("Copyright (C) 2011-2017 Redis Labs Ltd.");
                    puts("This is free software.  You may redistribute copies of it under the terms of");
                    puts("the GNU General Public License <http://www.gnu.org/licenses/gpl.html>.");
                    puts("There is NO WARRANTY, to the extent permitted by law.");
                    exit(0);
                case 'b':
                    cfg->blocking = true;
                    break;
                case 'k':
                    endptr = NULL;
                    cfg->skew_level = (int) strtoul(optarg, &endptr, 10);
                    if (cfg->skew_level < 1 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: skew-level must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_server_threads:
                    endptr = NULL;
                    cfg->server_threads = (int) strtoul(optarg, &endptr, 10);
                    if (cfg->server_threads < 1 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: server threads must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_config_file:
                    cfg->config_file = optarg;
                    break;
                case o_ir_distribution:
                    if (strcmp(optarg, "NONE") &&
                        strcmp(optarg, "POISSON") &&
                        strcmp(optarg, "UNIFORM")) {
                            fprintf(stderr, "Don't support this type: %s \n",
                                    optarg);
                            return -1;
                        }
                    cfg->ir_distribution = optarg;
                    break;
                case o_log_dir:
                    cfg->log_dir = optarg;
                    break;
                case o_videos:
                    cfg->num_videos = (int) strtoul(optarg, &endptr, 10);
                    if (!endptr || *endptr != '\0') {
                        fprintf(stderr, "error: num of videos must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_log_qps_file:
                    cfg->log_qps_file = optarg;
                    break;
                case o_log_latency_file:
                    cfg->log_latency_file = optarg;
                    break;
                case 's':
                    cfg->server = optarg;
                    break;
                case 'S':
                    cfg->unix_socket = optarg;
                    break;
                case 'p':
                    endptr = NULL;
                    cfg->port = (unsigned short) strtoul(optarg, &endptr, 10);
                    if (!cfg->port || cfg->port > 65535 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: port must be a number in the range [1-65535].\n");
                        return -1;
                    }
                    break;
                case 'P':
                    if (strcmp(optarg, "memcache_text") &&
                        strcmp(optarg, "memcache_binary") &&
                        strcmp(optarg, "redis")) {
                                fprintf(stderr, "error: supported protocols are 'memcache_text', 'memcache_binary' and 'redis'.\n");
                                return -1;
                    }
                    cfg->protocol = optarg;
                    break;
                case 'o':
                    cfg->out_file = optarg;
                    break;
                case o_client_stats:
                    cfg->client_stats = optarg;
                    break;
                case 'x':
                    endptr = NULL;
                    cfg->run_count = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!cfg->run_count || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: run count must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case 'D':
                    cfg->debug++;
                    break;
                case o_show_config:
                    cfg->show_config++;
                    break;
                case o_hide_histogram:
                    cfg->hide_histogram++;
                    break;
                case o_distinct_client_seed:
                    cfg->distinct_client_seed++;
                    break;
                case o_randomize:
                    srandom(generate_random_seed());
                    cfg->randomize = random();
                    break;
                case 'n':
                    endptr = NULL;
                    if (strcmp(optarg, "allkeys")==0)
                        cfg->requests = -1;
                    else {
                        cfg->requests = (unsigned int) strtoul(optarg, &endptr, 10);
                        if (!cfg->requests || !endptr || *endptr != '\0') {
                            fprintf(stderr, "error: requests must be greater than zero.\n");
                            return -1;
                        }
                        if (cfg->test_time) {
                            fprintf(stderr, "error: --test-time and --requests are mutually exclusive.\n");
                            return -1;
                        }
                    }
                    break;
                case 'c':
                    endptr = NULL;
                    cfg->clients = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!cfg->clients || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: clients must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case 't':
                    endptr = NULL;
                    cfg->threads = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!cfg->threads || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: threads must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_test_time:
                    endptr = NULL;
                    cfg->test_time = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!cfg->test_time || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: test time must be greater than zero.\n");
                        return -1;
                    }
                    if (cfg->requests) {
                        fprintf(stderr, "error: --test-time and --requests are mutually exclusive.\n");
                        return -1;
                    }
                    break;
                case o_ratio:
                    cfg->ratio = config_ratio(optarg);
                    if (!cfg->ratio.is_defined()) {
                        fprintf(stderr, "error: ratio must be expressed as [0-n]:[0-n].\n");
                        return -1;
                    }
                    break;
                case o_pipeline:
                    endptr = NULL;
                    cfg->pipeline = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!cfg->pipeline || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: pipeline must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case 'd':
                    endptr = NULL;
                    cfg->data_size = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!cfg->data_size || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: data-size must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case 'R':
                    cfg->random_data = true;
                    break;
                case o_data_offset:
                    endptr = NULL;
                    cfg->data_offset = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!endptr || *endptr != '\0') {
                        fprintf(stderr, "error: data-offset must be greater than or equal to zero.\n");
                        return -1;
                    }
                    break;
                case o_data_size_range:
                    cfg->data_size_range = config_range(optarg);
                    if (!cfg->data_size_range.is_defined() || cfg->data_size_range.min < 1) {
                        fprintf(stderr, "error: data-size-range must be expressed as [1-n]-[1-n].\n");
                        return -1;
                    }
                    break;
                case o_data_size_list:
                    cfg->data_size_list = config_weight_list(optarg);
                    if (!cfg->data_size_list.is_defined()) {
                        fprintf(stderr, "error: data-size-list must be expressed as [size1:weight1],...[sizeN:weightN].\n");
                        return -1;
                    }
                    break;
                case o_expiry_range:
                    cfg->expiry_range = config_range(optarg);
                    if (!cfg->expiry_range.is_defined()) {
                        fprintf(stderr, "error: data-size-range must be expressed as [0-n]-[1-n].\n");
                        return -1;
                    }
                    break;
                case o_data_size_pattern:
                    cfg->data_size_pattern = optarg;
                    if (strlen(cfg->data_size_pattern) != 1 ||
                        (cfg->data_size_pattern[0] != 'R' && cfg->data_size_pattern[0] != 'S')) {
                            fprintf(stderr, "error: data-size-pattern must be either R or S.\n");
                            return -1;
                    }
                    break;
                case o_data_import:
                    cfg->data_import = optarg;
                    break;
                case o_data_verify:
                    cfg->data_verify = 1;
                    break;
                case o_verify_only:
                    cfg->verify_only = 1;
                    cfg->data_verify = 1;   // Implied
                    break;
                case o_key_prefix:
                    cfg->key_prefix = optarg;
                    break;
                case o_key_minimum:
                    endptr = NULL;
                    cfg->key_minimum = strtoull(optarg, &endptr, 10);
                    if (cfg->key_minimum < 1 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: key-minimum must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_key_maximum:
                    endptr = NULL;
                    cfg->key_maximum = strtoull(optarg, &endptr, 10);
                    if (cfg->key_maximum< 1 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: key-maximum must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_key_stddev:
                    endptr = NULL;
                    cfg->key_stddev = (unsigned int) strtof(optarg, &endptr);
                    if (cfg->key_stddev<= 0 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: key-stddev must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_key_median:
                    endptr = NULL;
                    cfg->key_median = (unsigned int) strtof(optarg, &endptr);
                    if (cfg->key_median<= 0 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: key-median must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_key_pattern:
                    cfg->key_pattern = optarg;
                    if (strlen(cfg->key_pattern) != 3 || cfg->key_pattern[1] != ':' ||
                        (cfg->key_pattern[0] != 'R' && cfg->key_pattern[0] != 'S' && cfg->key_pattern[0] != 'G' && cfg->key_pattern[0] != 'P') ||
                        (cfg->key_pattern[2] != 'R' && cfg->key_pattern[2] != 'S' && cfg->key_pattern[2] != 'G' && cfg->key_pattern[2] != 'P')) {
                            fprintf(stderr, "error: key-pattern must be in the format of [S/R/G]:[S/R/G].\n");
                            return -1;
                    }
                    break;
                case o_reconnect_interval:
                    endptr = NULL;
                    cfg->reconnect_interval = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (!cfg->reconnect_interval || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: reconnect-interval must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case o_generate_keys:
                    cfg->generate_keys = 1;
                    break;
                case o_multi_key_get:
                    endptr = NULL;
                    cfg->multi_key_get = (unsigned int) strtoul(optarg, &endptr, 10);
                    if (cfg->multi_key_get <= 0 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: multi-key-get must be greater than zero.\n");
                        return -1;
                    }
                    break;
                case 'a':
                    cfg->authenticate = optarg;
                    break;
                case o_select_db:
                    cfg->select_db = (int) strtoul(optarg, &endptr, 10);
                    if (cfg->select_db < 0 || !endptr || *endptr != '\0') {
                        fprintf(stderr, "error: select-db must be greater or equal zero.\n");
                        return -1;
                    }
                    break;
                case o_no_expiry:
                    cfg->no_expiry = true;
                    break;
                case o_wait_ratio:
                    cfg->wait_ratio = config_ratio(optarg);
                    if (!cfg->wait_ratio.is_defined()) {
                        fprintf(stderr, "error: wait-ratio must be expressed as [0-n]:[0-n].\n");
                        return -1;
                    }
                    break;
                case o_num_slaves:
                    cfg->num_slaves = config_range(optarg);
                    if (!cfg->num_slaves.is_defined()) {
                        fprintf(stderr, "error: num-slaves must be expressed as [0-n]-[1-n].\n");
                        return -1;
                    }
                    break;
                case o_wait_timeout:
                    cfg->wait_timeout = config_range(optarg);
                    if (!cfg->wait_timeout.is_defined()) {
                        fprintf(stderr, "error: wait-timeout must be expressed as [0-n]-[1-n].\n");
                        return -1;
                    }
                    break;
                case o_json_out_file:
                    cfg->json_out_file = optarg;
                    break;
                case o_cluster_mode:
                    cfg->cluster_mode = true;
                    break;
            default:
                    return -1;
                    break;
        }
    }

    if (cfg->cluster_mode && !verify_cluster_option(cfg))
        return -1;
    if (cfg->blocking) {
        fprintf(stderr, "[CONFIG] In blocking libevent loop mode!\n");
    } else {
        fprintf(stderr, "[CONFIG] In non-blocking libevent loop mode!\n");
    }
    if (cfg->skew_level == 0) {
        cfg->skew_level = 1;
        cfg->server_threads = std::max(1, cfg->server_threads);
    } else {
        if (cfg->server_threads == 0) {
            fprintf(stderr, "ERROR: Must set server-threads when using skew level!\n");
            return -1;
        } else {
            fprintf(stderr, "[CONFIG] Skew level: %u, server threads: %u \n",
                    cfg->skew_level, cfg->server_threads);
        }
    }
    if (cfg->config_file == NULL) {
        fprintf(stderr, "No benchmark file! use default mode. \n");
        master_finished = true;
    }

    if (cfg->ir_distribution != NULL) {
        if (strcmp(cfg->ir_distribution, "NONE") == 0) {
            cfg->distType = NONE;
        } else if (strcmp(cfg->ir_distribution, "POISSON") == 0) {
            cfg->distType = POISSON;
        } else if (strcmp(cfg->ir_distribution, "UNIFORM") == 0) {
            cfg->distType = UNIFORM;
        } else {
            fprintf(stderr, "Do not support type: %s \n", cfg->ir_distribution);
            return -1;
        }
    } else {
        cfg->distType = NONE;
    }

    if (cfg->log_dir == NULL) {
        cfg->log_dir = "./latency_throughput_log";
    }

    if (cfg->log_qps_file == NULL) {
        cfg->log_qps_file = "throughput.log";
    }

    // By default don't record latency!
//    if (cfg->log_latency_file == NULL) {
//        cfg->log_latency_file = "latency.log";
//    }
//
    if (cfg->num_videos > 0) {
        fprintf(stderr, "Number of background videos: %d \n", cfg->num_videos);
    } else {
        fprintf(stderr, "No background videos \n");
    }
    return 0;
}

void usage() {
    fprintf(stdout, "Usage: memtier_benchmark [options]\n"
            "A memcache/redis NoSQL traffic generator and performance benchmarking tool.\n"
            "\n"
            "Connection and General Options:\n"
            "  -s, --server=ADDR              Server address (default: localhost)\n"
            "  -p, --port=PORT                Server port (default: 6379)\n"
            "  -S, --unix-socket=SOCKET       UNIX Domain socket name (default: none)\n"
            "  -P, --protocol=PROTOCOL        Protocol to use (default: redis).  Other\n"
            "                                 supported protocols are memcache_text,\n"
            "                                 memcache_binary.\n"
            "  -x, --run-count=NUMBER         Number of full-test iterations to perform\n"
            "  -D, --debug                    Print debug output\n"
            "      --client-stats=FILE        Produce per-client stats file\n"
            "      --out-file=FILE            Name of output file (default: stdout)\n"
            "      --json-out-file=FILE       Name of JSON output file, if not set, will not print to json\n"
            "      --show-config              Print detailed configuration before running\n"
            "      --hide-histogram           Don't print detailed latency histogram\n"
            "      --cluster-mode             Run client in cluster mode\n"
            "      --help                     Display this help\n"
            "      --version                  Display version information\n"
            "\n"
            "Test Options:\n"
            "  -n, --requests=NUMBER          Number of total requests per client (default: 10000)\n"
            "                                 use 'allkeys' to run on the entire key-range\n"
            "  -c, --clients=NUMBER           Number of clients per thread (default: 50)\n"
            "  -t, --threads=NUMBER           Number of threads (default: 4)\n"
            "      --test-time=SECS           Number of seconds to run the test\n"
            "      --ratio=RATIO              Set:Get ratio (default: 1:10)\n"
            "      --pipeline=NUMBER          Number of concurrent pipelined requests (default: 1)\n"
            "      --reconnect-interval=NUM   Number of requests after which re-connection is performed\n"
            "      --multi-key-get=NUM        Enable multi-key get commands, up to NUM keys (default: 0)\n"
            "  -a, --authenticate=CREDENTIALS Authenticate to redis using CREDENTIALS, which depending\n"
            "                                 on the protocol can be PASSWORD or USER:PASSWORD.\n"
            "      --select-db=DB             DB number to select, when testing a redis server\n"
            "      --distinct-client-seed     Use a different random seed for each client\n"
            "      --randomize                random seed based on timestamp (default is constant value)\n"
            "\n"
            "Object Options:\n"
            "  -d  --data-size=SIZE           Object data size (default: 32)\n"
            "      --data-offset=OFFSET       Actual size of value will be data-size + data-offset\n"
            "                                 Will use SETRANGE / GETRANGE (default: 0)\n"
            "  -R  --random-data              Indicate that data should be randomized\n"
            "      --data-size-range=RANGE    Use random-sized items in the specified range (min-max)\n"
            "      --data-size-list=LIST      Use sizes from weight list (size1:weight1,..sizeN:weightN)\n"
            "      --data-size-pattern=R|S    Use together with data-size-range\n"
            "                                 when set to R, a random size from the defined data sizes will be used,\n"
            "                                 when set to S, the defined data sizes will be evenly distributed across\n"
            "                                 the key range, see --key-maximum (default R)\n"
            "      --expiry-range=RANGE       Use random expiry values from the specified range\n"
            "\n"
            "Imported Data Options:\n"
            "      --data-import=FILE         Read object data from file\n"
            "      --data-verify              Enable data verification when test is complete\n"
            "      --verify-only              Only perform --data-verify, without any other test\n"
            "      --generate-keys            Generate keys for imported objects\n"
            "      --no-expiry                Ignore expiry information in imported data\n"
            "\n"
            "Key Options:\n"
            "      --key-prefix=PREFIX        Prefix for keys (default: \"memtier-\")\n"
            "      --key-minimum=NUMBER       Key ID minimum value (default: 0)\n"
            "      --key-maximum=NUMBER       Key ID maximum value (default: 10000000)\n"
            "      --key-pattern=PATTERN      Set:Get pattern (default: R:R)\n"
            "                                 G for Gaussian distribution.\n"
            "                                 R for uniform Random.\n"
            "                                 S for Sequential.\n"
            "                                 P for Parallel (Sequential were each client has a subset of the key-range).\n"
            "      --key-stddev               The standard deviation used in the Gaussian distribution\n"
            "                                 (default is key range / 6)\n"
            "      --key-median               The median point used in the Gaussian distribution\n"
            "                                 (default is the center of the key range)\n"
            "\n"
            "WAIT Options:\n"
            "      --wait-ratio=RATIO         Set:Wait ratio (default is no WAIT commands - 1:0)\n"
            "      --num-slaves=RANGE         WAIT for a random number of slaves in the specified range\n"
            "      --wait-timeout=RANGE       WAIT for a random number of milliseconds in the specified range (normal \n"
            "                                 distribution with the center in the middle of the range)"
            "\n"
            "BLOCKING Option:\n"
            "  -b  --blocking                 Run with libevent blocking loop \n"
            "\n"
            "SKEWED Option:\n"
            "  -k  --skew-level               How many clients pileup on memcached's 1st thread \n"
            "      --sever-threads            How many worker threads used in memcached server \n"
            "\n"
            "SYNTHETIC Option:\n"
            "      --config-file              Input synthetic benchmark config file \n"
            "      --ir-dist                  Inter request distribution type (NONE/POISSON/UNIFORM) \n"
            "LOGGING Option:\n"
            "      --log-dir                  Directory to store log files \n"
            "      --log-qpsfile              File name to store qps log \n"
            "      --log-latencyfile          File name to store latency log \n"
            "VIDEO BACKGROUND Option:\n"
            "      --videos=NUM               Number of background video processes to start\n"
            "\n"
            );
    
    exit(2);
}

static void* cg_thread_start(void *t);

struct cg_thread {
    unsigned int m_thread_id;
    benchmark_config* m_config;
    object_generator* m_obj_gen;
    client_group* m_cg;
    abstract_protocol* m_protocol;
    pthread_t m_thread;
    bool m_finished;
    
    cg_thread(unsigned int id, benchmark_config* config, object_generator* obj_gen) :
        m_thread_id(id), m_config(config), m_obj_gen(obj_gen), m_cg(NULL), m_protocol(NULL), m_finished(false)
    {
        m_protocol = protocol_factory(m_config->protocol);
        assert(m_protocol != NULL);
        
        m_cg = new client_group(m_config, m_protocol, m_obj_gen);
    }
        
    ~cg_thread()
    {
        if (m_cg != NULL) {
            delete m_cg;
        }
        if (m_protocol != NULL) {
            delete m_protocol;
        }
    }

    int prepare(void)
    {
        if (m_cg->create_clients(m_config->clients) < (int) m_config->clients)
            return -1;
        return m_cg->prepare();
    }
    
    int start(void)
    {
        return pthread_create(&m_thread, NULL, cg_thread_start, (void *)this);        
    }

    void join(void)
    {
        int* retval;
        int ret;

        ret = pthread_join(m_thread, (void **)&retval);
        assert(ret == 0);        
    }
    
};

static void* cg_thread_start(void *t)
{
    cg_thread* thread = (cg_thread*) t;
    thread->m_cg->run();
    thread->m_finished = true;
    
    return t;
}

void size_to_str(unsigned long int size, char *buf, int buf_len)
{
    if (size >= 1024*1024*1024) {
        snprintf(buf, buf_len, "%.2fGB", 
            (float) size / (1024*1024*1024));
    } else if (size >= 1024*1024) {
        snprintf(buf, buf_len, "%.2fMB",
            (float) size / (1024*1024));
    } else {
        snprintf(buf, buf_len, "%.2fKB",
            (float) size / 1024);
    }    
}

// Check a directory exists or not. If not exists, then create one.
static void check_dir(std::string &dir) {
    struct stat st;
    if (stat(dir.c_str(), &st) == 0) {
        if ((st.st_mode & S_IFDIR) != 0) {
            fprintf(stderr, "\nOutput dir: %s exists!\n", dir.c_str());
        }
    } else {
        fprintf(stderr, "Creating dir: %s ...\n", dir.c_str());
        const int dir_err = mkdir(dir.c_str(),  S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
        if (dir_err != 0) {
            printf("Error creating dir: %s !\n", dir.c_str());
            exit(-1);
        }
    }
}

static int parse_config_file(benchmark_config *cfg) {
    const char* config_file = cfg->config_file;
    int numServerThreads = cfg->server_threads;
    int numClients = cfg->threads * cfg->clients;

    if (config_file == NULL) {
        for (int i = 0; i < numServerThreads; ++i) {
            qpsPerClient.push_back(0);
        }
        return 0;
    }
    FILE* specFile = fopen(config_file, "r");
    if (!specFile) {
        fprintf(stderr, "Configuration file '%s' non existent! \n", config_file);
        return -1;
    }
    char buffer[1024];
    if (fgets(buffer, 1024, specFile) == NULL) {
        fprintf(stderr, "Error reading configuration file: %s\n", strerror(errno));
        return -1;
    }
    sscanf(buffer, "%zu %d", &numIntervals, &cfg->server_threads);
    intervals = new Interval[numIntervals];
    numServerThreads = cfg->server_threads;

    for (size_t i = 0; i < numIntervals; ++i) {
        if (fgets(buffer, 1024, specFile) == NULL) {
            fprintf(stderr, "Error reading configuration file: %s\n", strerror(errno));
            return -1;
        }
        sscanf(buffer, "%ld %lf %lf", &intervals[i].timeToRun,
               &intervals[i].requestsPerSecond, &intervals[i].skewFactor);
    }
    fclose(specFile);

    // Initialize per client qps
    currGoalQPS = intervals[0].requestsPerSecond;
    currentSkew = intervals[0].skewFactor;

    // Skew on the first thread
    double initialQPSskew = currGoalQPS *
        currentSkew * numServerThreads / (numClients * 1.0);

    qpsPerClient.push_back(initialQPSskew);

    // Evenly distributed among all other clients
    double initialQPS = currGoalQPS *
        (1.0 - currentSkew) * numServerThreads /
        (numClients * (numServerThreads - 1) * 1.0);

    // Fill in all other threads
    for (int i = 1; i < numServerThreads; ++i) {
        qpsPerClient.push_back(initialQPS);
    }
    return 0;
}

static void* start_master(void *arg) {
    fprintf(stderr, "Start the master!\n");
    benchmark_config *cfg = (benchmark_config*)arg;
    // Initialize per client qps
    int numServerThreads = cfg->server_threads;
    int numClients = cfg->threads * cfg->clients;
    double clientQPS = 0.0;
    double clientQPSskew = 0.0;

    fprintf(stderr, "Num of intervals: %zu, Num of server threads %d\n",
            numIntervals, numServerThreads);

    // Initialize interval index
    size_t currentInterval = 0;

    // Start the DCFT-style loop
    switch (cfg->distType) {
        case NONE:
            fprintf(stderr, "No inter-request time! \n");
            break;
        case POISSON:
            fprintf(stderr, "Poisson distribution! \n");
            break;
        case UNIFORM:
            fprintf(stderr, "Uniform distribution! \n");
    }

    // Start video processes
    if (cfg->num_videos > 0) {
        char videoName[100];
        char prefixstr[10];
        for (int i = 1; i <= cfg->num_videos; ++i) {
            sprintf(videoName, "sintel-1280-copy%d.y4m", i);
            char prefix = 'a' + i - 1;
            sprintf(prefixstr, "%c", prefix);
            start_video_decoding(videoName, prefixstr, cfg);
        }
    }

    // Our workload must start after this point
    PerfUtils::Util::serialize();

    uint64_t currentTime = Cycles::rdtsc();

    uint64_t nextIntervalTime =
        currentTime +
        Cycles::fromNanoseconds(intervals[currentInterval].timeToRun);

    int64_t shouldCount = 0;
    shouldCount += intervals[currentInterval].requestsPerSecond *
        (intervals[currentInterval].timeToRun / 1000000000);

    for (;; currentTime = Cycles::rdtsc()) {
        if (nextIntervalTime < currentTime) {
            // Advance the interval
            currentInterval++;
            if (currentInterval == numIntervals)
                break;
            shouldCount += intervals[currentInterval].requestsPerSecond *
                (intervals[currentInterval].timeToRun / 1000000000);

            // Skew on the first thread
            currentSkew = intervals[currentInterval].skewFactor;
            currGoalQPS = intervals[currentInterval].requestsPerSecond;
            clientQPSskew =
                currGoalQPS * currentSkew * numServerThreads / (numClients * 1.0);

            qpsPerClient[0] = clientQPSskew;

            // Evenly distributed among all other clients
            clientQPS =
                currGoalQPS * (1.0 - currentSkew) * numServerThreads /
                (numClients * (numServerThreads - 1) * 1.0);

            // Fill in all other threads
            for (int i = 1; i < numServerThreads; ++i) {
                qpsPerClient[i] = clientQPS;
            }

            nextIntervalTime =
                currentTime +
                Cycles::fromNanoseconds(intervals[currentInterval].timeToRun);
        }
        usleep(10000); // sleep 10ms avoid busy loop
    }

    master_finished = true;
    fprintf(stderr, "[STATS] should send out %ld reqs \n", shouldCount);

    // Stop video processes
    if (cfg->num_videos > 0) {
        stop_video_decoding(cfg);
    }

    return NULL;
}

static void join_master(pthread_t tid) {
    int* retval;
    int ret;

    ret = pthread_join(tid, (void **)&retval);
    assert(ret == 0);

    delete []intervals;
    fprintf(stderr, "Shutdown the master!\n");
    return;
}

static void process_results(std::string filePath) {
    FILE *latencyLog = fopen(filePath.c_str(), "w");
    if (latencyLog == NULL) {
        fprintf(stderr, "Fail to open log file: %s \n", filePath.c_str());
        return;
    }
    fprintf(stderr, "Storing latency log to %s \n", filePath.c_str());

    // Sanity check
    if ((getArrayIndex.load() > MAX_ENTRIES) ||
        (setArrayIndex.load() > MAX_ENTRIES)) {
        fprintf(stderr, "Benchmark wrote past the end of latency array."
                "Assuming memory corruption. Final index writtern = %u,"
                "Max entries: %lu",
                std::max(getArrayIndex.load(), setArrayIndex.load()),
                MAX_ENTRIES);
        abort();
    }
    fprintf(stderr, " %u entries for setLatencies and %u for getLatencies \n",
            setArrayIndex.load(), getArrayIndex.load());

    fprintf(latencyLog, "TimeInUSecSinceEpoch,DurationInUsec,"
            "50%% Latency GET,90%% GET,99%% GET,Min GET,Max GET,"
            "50%% SET,90%% SET,99%% SET,Min SET,Max SET,"
            "Throghput GET,Throughput SET\n");

    char outbuff[1024];
    for (size_t i = 1; i < getIndices.size(); ++i) {
        double durationOfInterval = timeStamps[i] - timeStamps[i-1];

        Statistics mathStatsGet;
        Statistics mathStatsSet;

        uint64_t getEntries = 1.0; // At least one entry
        uint64_t setEntries = 1.0;
        // Get Latency statistics
        // Note that this computation will modify data
        if (getArrayIndex.load() > 0) {
            getEntries = getIndices[i] - getIndices[i-1];
        }
        mathStatsGet =
            computeStatistics(getLatencies + getIndices[i-1], getEntries);

        if (setArrayIndex.load() > 0) {
            setEntries = setIndices[i] - setIndices[i-1];
        }
        mathStatsSet =
            computeStatistics(setLatencies + setIndices[i-1], setEntries);

        double getThroughput =
            (getIndices[i] - getIndices[i-1]) * 1000000.0 / durationOfInterval;
        double setThroughput =
            (setIndices[i] - setIndices[i-1]) * 1000000.0 / durationOfInterval;

        sprintf(outbuff,
                "%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.2f,%.2f\n",
                mathStatsGet.median, mathStatsGet.P90,
                mathStatsGet.P99, mathStatsGet.min, mathStatsGet.max,
                mathStatsSet.median, mathStatsSet.P90, mathStatsSet.P99,
                mathStatsSet.min, mathStatsSet.max,
                getThroughput, setThroughput);

        if (i == 1) {
            // Duplicate the first line! For figures
            fprintf(latencyLog, "%lu,%.6lf,%s", timeStamps[0], 0.0, outbuff);
        }

        fprintf(latencyLog, "%lu,%.6lf,%s",
                timeStamps[i], durationOfInterval, outbuff);
    }

    fclose(latencyLog);
}

run_stats run_benchmark(int run_id, benchmark_config* cfg, object_generator* obj_gen)
{
    fprintf(stderr, "[RUN #%u] Preparing benchmark client...\n", run_id);

    // Page in our data store
    MAX_ENTRIES = 1L << ARRAY_EXP;

    if (cfg->log_latency_file != NULL) {
        setLatencies = new uint64_t[MAX_ENTRIES];
        getLatencies = new uint64_t[MAX_ENTRIES];
        if ((setLatencies == NULL) || (getLatencies == NULL)) {
            fprintf(stderr, "Failed to allocate log entries! \n");
            exit(-1);
        }
        memset(setLatencies, 0, MAX_ENTRIES * sizeof(uint64_t));
        memset(getLatencies, 0, MAX_ENTRIES * sizeof(uint64_t));
    } else {
        setLatencies = NULL;
        getLatencies = NULL;
    }
    setArrayIndex.store(0);
    getArrayIndex.store(0);

    // Initial elements in the arrays
    setIndices.push_back(setArrayIndex);
    getIndices.push_back(getArrayIndex);

    // prepare threads data
    std::vector<cg_thread*> threads;
    for (unsigned int i = 0; i < cfg->threads; i++) {
        cg_thread* t = new cg_thread(i, cfg, obj_gen);
        assert(t != NULL);

        if (t->prepare() < 0) {
            benchmark_error_log("error: failed to prepare thread %u for test.\n", i);
            exit(1);
        }
        threads.push_back(t);
    }

    // launch threads
    fprintf(stderr, "[RUN #%u] Launching threads now...\n", run_id);
    for (std::vector<cg_thread*>::iterator i = threads.begin(); i != threads.end(); i++) {
        (*i)->start();
    }

    // launch the master thread that controls the rate of requests
    pthread_t master_tid;
    if (cfg->config_file) {
        pthread_create(&master_tid, NULL, start_master, cfg);
    }

    struct timeval curstartTime, prevstartTime, startTime, stopTime;
    gettimeofday(&prevstartTime, NULL);
    startTime = prevstartTime;

    timeStamps.push_back(timeval_to_ts(startTime));

    unsigned long int prev_ops = 0;
    unsigned long int prev_bytes = 0;
    unsigned long int prev_duration = 0;
    double prev_latency = 0, cur_latency = 0;
    unsigned long int cur_ops_sec = 0;
    unsigned long int cur_bytes_sec = 0;
    unsigned long int realTotalOps = 0;
    unsigned long int total_ops = 0;

    // To collect per-client stats
#ifdef PER_CLIENT
    int totalClients = cfg->threads * cfg->clients;
    unsigned long int *prevOpsPerClient = new unsigned long int[totalClients];
    unsigned long int *currOpsPerClient = new unsigned long int[totalClients];
    double *prevLatencyPerClient = new double[totalClients];
    double *currLatencyPerClient = new double[totalClients];

    unsigned long int *realTotalOpsPerClient = prevOpsPerClient;
    double *realTotalLatencyPerClient = prevLatencyPerClient;

    unsigned long int *totalOpsPerClient = new unsigned long int[totalClients];
    double *totalLatencyPerClient = new double[totalClients];

    memset(prevOpsPerClient, 0, totalClients * sizeof(unsigned long int));
    memset(currOpsPerClient, 0, totalClients * sizeof(unsigned long int));
    memset(prevLatencyPerClient, 0, totalClients * sizeof(double));
    memset(totalOpsPerClient, 0, totalClients * sizeof(unsigned long int));
    memset(totalLatencyPerClient, 0, totalClients * sizeof(double));
#endif

    // To collect per-server thread stats
#ifdef PER_SERVER_TID
    int serverThreads = cfg->server_threads;
    unsigned long int *prevOpsPerTid = new unsigned long int[serverThreads];
    unsigned long int *currOpsPerTid = new unsigned long int[serverThreads];
    double *prevLatencyPerTid = new double[serverThreads];
    double *currLatencyPerTid = new double[serverThreads];

    unsigned long int *realTotalOpsPerTid = prevOpsPerTid;
    double *realTotalLatencyPerTid = prevLatencyPerTid;

    unsigned long int *totalOpsPerTid = new unsigned long int[serverThreads];
    double *totalLatencyPerTid = new double[serverThreads];

    memset(prevOpsPerTid, 0, serverThreads * sizeof(unsigned long int));
    memset(currOpsPerTid, 0, serverThreads * sizeof(unsigned long int));
    memset(prevLatencyPerTid, 0, serverThreads * sizeof(double));

    std::string logDir = std::string(cfg->log_dir);
    check_dir(logDir);
    std::string filePath;
    filePath = logDir + "/" + cfg->log_qps_file;
    FILE *qpsLog = fopen(filePath.c_str(), "w");

    fprintf(stderr, "Storing QPS log to %s/%s \n", cfg->log_dir, cfg->log_qps_file);
    fprintf(qpsLog, "TimeInUSecSinceEpoch,DurationInSec,Skew,Goal");

    for (int tid = 0; tid < serverThreads; ++tid) {
        fprintf(qpsLog, ",tid%02d", tid);
    }

    fprintf(qpsLog, ",Total\n");

    fflush(qpsLog);
    double prevSkew = currentSkew;
    double prevcurrGoalQPS = currGoalQPS;
#endif

    // provide some feedback...
    unsigned int active_threads = 0;
    int iters = 0;
    do {
        active_threads = 0;
        sleep(1); // Sleep 1 sec

        total_ops = 0;
        unsigned long int total_bytes = 0;
        unsigned long int duration = 0;
        unsigned int thread_counter = 0; 
        unsigned long int total_latency = 0;

#ifdef PER_CLIENT
        int cid = 0; // client id
#endif

#ifdef PER_SERVER_TID
        memset(totalOpsPerTid, 0, serverThreads * sizeof(unsigned long int));
        memset(totalLatencyPerTid, 0, serverThreads * sizeof(double));
#endif

        gettimeofday(&curstartTime, NULL);
        double curDuration = (curstartTime.tv_sec - prevstartTime.tv_sec) +
                             ((curstartTime.tv_usec - prevstartTime.tv_usec) / 1000000.0);

        // Collect latency, throughput information from the past interval
        setIndices.push_back(setArrayIndex.load());
        getIndices.push_back(getArrayIndex.load());
        timeStamps.push_back(timeval_to_ts(curstartTime));
        for (std::vector<cg_thread*>::iterator i = threads.begin(); i != threads.end(); i++) {
            if (!(*i)->m_finished)
                active_threads++;

            total_ops += (*i)->m_cg->get_total_ops();
            total_bytes += (*i)->m_cg->get_total_bytes();
            total_latency += (*i)->m_cg->get_total_latency();
            thread_counter++;
            float factor = ((float)(thread_counter - 1) / thread_counter);
            duration =  factor * duration +  (float)(*i)->m_cg->get_duration_usec() / thread_counter ;

#ifdef PER_CLIENT
            // Collect per-client stats
            for (std::vector<client*>::iterator j = (*i)->m_cg->m_clients.begin();
                 j != (*i)->m_cg->m_clients.end(); ++j) {

                totalOpsPerClient[cid] = (*j)->get_stats()->get_total_ops();
                totalLatencyPerClient[cid] = (*j)->get_stats()->get_total_latency();

                currOpsPerClient[cid] = totalOpsPerClient[cid] - prevOpsPerClient[cid];
                currLatencyPerClient[cid] = (totalLatencyPerClient[cid] - prevLatencyPerClient[cid])
                                            / currOpsPerClient[cid];

                fprintf(stderr, "Cid: %d, currOps/sec: %.2lf, currLatency: %.4lf us\n",
                        cid, currOpsPerClient[cid] / curDuration, currLatencyPerClient[cid]);

                prevOpsPerClient[cid] = totalOpsPerClient[cid];
                prevLatencyPerClient[cid] = totalLatencyPerClient[cid];
                cid++;
            }
#endif

#ifdef PER_SERVER_TID
            // Collect per server thread id stats
            for (std::vector<client*>::iterator j = (*i)->m_cg->m_clients.begin();
                 j != (*i)->m_cg->m_clients.end(); ++j) {
                int tid = (*j)->serverTid;
                totalOpsPerTid[tid] += (*j)->get_stats()->get_total_ops();
                totalLatencyPerTid[tid] += (*j)->get_stats()->get_total_latency();
            }
#endif
        }

#ifdef PER_SERVER_TID
        char outputBuff[1024];
        unsigned long int curTimeStamp =
            curstartTime.tv_sec * 1000000 + curstartTime.tv_usec;
        sprintf(outputBuff, "%lu,%.6lf,%6lf,%.2lf", curTimeStamp, curDuration,
                prevSkew, prevcurrGoalQPS);
        double totalQPS = 0.0;
        for (int tid = 0; tid < serverThreads; ++tid) {
            currOpsPerTid[tid] = totalOpsPerTid[tid] - prevOpsPerTid[tid];
            currLatencyPerTid[tid] =
                (totalLatencyPerTid[tid] - prevLatencyPerTid[tid]) /
                currOpsPerTid[tid];
            double currOps = currOpsPerTid[tid] / curDuration;
            totalQPS += currOps;
            sprintf(outputBuff + strlen(outputBuff), ",%.2lf", currOps);
            prevOpsPerTid[tid] = totalOpsPerTid[tid];
            prevLatencyPerTid[tid] = totalLatencyPerTid[tid];
        }
        sprintf(outputBuff + strlen(outputBuff), ",%.2lf\n", totalQPS);
        if (iters == 0) {
            // Duplicate the first line! For figures
            char dupBuff[1024];
            unsigned long prevTimeStamp = curTimeStamp - curDuration * 1000000;
            sprintf(dupBuff, "%lu,%.6lf", prevTimeStamp, 0.0);
            strcpy(dupBuff + strlen(dupBuff), outputBuff + strlen(dupBuff));
            fprintf(qpsLog, "%s", dupBuff);
        }
        fprintf(qpsLog, "%s", outputBuff);
        fflush(qpsLog);

        prevSkew = currentSkew; // because we are logging for the last duration
        prevcurrGoalQPS = currGoalQPS;  // we must update later
#endif
        // In order to throw out the last loop
        stopTime = prevstartTime;
        realTotalOps = prev_ops;

        unsigned long int cur_ops = total_ops - prev_ops;
        unsigned long int cur_bytes = total_bytes - prev_bytes;
        unsigned long int cur_duration = duration - prev_duration;
        double cur_total_latency = total_latency - prev_latency;
        prev_ops = total_ops;
        prev_bytes = total_bytes;
        prev_latency = total_latency;
        prev_duration = duration;
        prevstartTime = curstartTime;
        
        unsigned long int ops_sec = 0;
        unsigned long int bytes_sec = 0;
        double avg_latency = 0;

        double curOpsSec = cur_ops / curDuration; // Calculate throughput with unified time interval
  
        if (duration > 1) {
            ops_sec = (long)( (double)total_ops / duration * 1000000);
            bytes_sec = (long)( (double)total_bytes / duration * 1000000);
            avg_latency = ((double) total_latency / 1000 / total_ops) ;
        }
        if (cur_duration > 1 && active_threads == cfg->threads) {
            cur_ops_sec = (long)( (double)cur_ops / cur_duration * 1000000);
            cur_bytes_sec = (long)( (double)cur_bytes / cur_duration * 1000000);
            cur_latency = ((double) cur_total_latency / 1000 / cur_ops) ;
        }

        char bytes_str[40], cur_bytes_str[40];
        size_to_str(bytes_sec, bytes_str, sizeof(bytes_str)-1);
        size_to_str(cur_bytes_sec, cur_bytes_str, sizeof(cur_bytes_str)-1);
        
        double progress = 0;
        if(cfg->requests)
            progress = 100.0 * total_ops / ((double)cfg->requests*cfg->clients*cfg->threads);
        else
            progress = 100.0 * (duration / 1000000.0)/cfg->test_time;
        
        fprintf(stderr, "[RUN #%u %.0f%%, %3u secs] %2u threads: %11lu ops, %7lu (avg: %7lu) ops/sec, %s/sec (avg: %s/sec), %5.2f (avg: %5.2f) msec latency, real throughput %.2lf ops/sec \n",
            run_id, progress, (unsigned int) (duration / 1000000), active_threads, total_ops, cur_ops_sec, ops_sec, cur_bytes_str, bytes_str, cur_latency, avg_latency, curOpsSec);

        iters++;
    } while (active_threads > 0);

    fprintf(stderr, "\n\n");
    double realDuration = (stopTime.tv_sec - startTime.tv_sec) + ((stopTime.tv_usec - startTime.tv_usec) / 1000000.0);
    double realThroughput = realTotalOps / realDuration;
    fprintf(stderr, "Real throughput (ops/sec) is: %.2lf \n"
            "Total responses: %ld \n", realThroughput, total_ops);

#ifdef PER_CLIENT
    // Print per-client stats summary
    for (int cid = 0; cid < totalClients; ++cid) {
        fprintf(stderr, "Cid: %d, Avg Ops/sec: %.2lf, Avg Latency: %.4lf us\n",
                cid, realTotalOpsPerClient[cid] / realDuration,
                realTotalLatencyPerClient[cid] / realTotalOpsPerClient[cid]);
    }
#endif

#ifdef PER_SERVER_TID
    // Print per server thread id summary
    for (int tid = 0; tid < serverThreads; ++tid) {
        fprintf(stderr, "ServerTid: %d, Avg Ops/sec: %.2lf,"
                "Avg Latency: %.4lf us \n",
                tid, realTotalOpsPerTid[tid] / realDuration,
                realTotalLatencyPerTid[tid] / realTotalOpsPerTid[tid]);
    }
#endif

    // join the master thread
    if (cfg->config_file) {
        join_master(master_tid);
    }

    // join all threads back and unify stats
    run_stats stats;
    for (std::vector<cg_thread*>::iterator i = threads.begin(); i != threads.end(); i++) {
        (*i)->join();
        (*i)->m_cg->merge_run_stats(&stats);
    }

    // Do we need to produce client stats?
    if (cfg->client_stats != NULL) {
        unsigned int cg_id = 0;
        fprintf(stderr, "[RUN %u] Writing client stats files...\n", run_id);
        for (std::vector<cg_thread*>::iterator i = threads.begin(); i != threads.end(); i++) {
            char prefix[PATH_MAX];

            snprintf(prefix, sizeof(prefix)-1, "%s-%u-%u", cfg->client_stats, run_id, cg_id++);
            (*i)->m_cg->write_client_stats(prefix);
        }
    }

    // clean up all client_groups.  the main value of this is to be able to
    // properly look for leaks...
    while (threads.size() > 0) {
        cg_thread* t = *threads.begin();
        threads.erase(threads.begin());
        delete t;
    }

    // Save to log file only if we provide the file name
    if (cfg->log_latency_file != NULL) {
        process_results(std::string(logDir) + "/" + cfg->log_latency_file);
    }

#ifdef PER_CLIENT
    delete []prevOpsPerClient;
    delete []currOpsPerClient;
    delete []prevLatencyPerClient;
    delete []currLatencyPerClient;
    delete []totalOpsPerClient;
    delete []totalLatencyPerClient;
#endif

#ifdef PER_SERVER_TID
    delete []prevOpsPerTid;
    delete []currOpsPerTid;
    delete []prevLatencyPerTid;
    delete []currLatencyPerTid;
    delete []totalOpsPerTid;
    delete []totalLatencyPerTid;

    fclose(qpsLog);
#endif

    // Release resources
    if (setLatencies != NULL)
        delete[] setLatencies;
    if (getLatencies != NULL)
        delete[] getLatencies;
    setIndices.clear();
    getIndices.clear();
    timeStamps.clear();

    return stats;
}


int main(int argc, char *argv[])
{
    struct benchmark_config cfg;

    master_finished = false;

    memset(&cfg, 0, sizeof(struct benchmark_config));
    if (config_parse_args(argc, argv, &cfg) < 0) {
        usage();
    }

    config_init_defaults(&cfg);
    log_level = cfg.debug;
    if (cfg.show_config) {
        fprintf(stderr, "============== Configuration values: ==============\n");
        config_print(stdout, &cfg);
        fprintf(stderr, "===================================================\n");
    }

    // Parse benchmark config file
    if (parse_config_file(&cfg) < 0) {
        fprintf(stderr, "ERROR: fail to read config file \n");
        exit(1);
    }

    // JSON file initiation
    json_handler *jsonhandler = NULL;
    if (cfg.json_out_file != NULL){
        jsonhandler = new json_handler((const char *)cfg.json_out_file);
        // We allways print the configuration to the JSON file      
        config_print_to_json(jsonhandler,&cfg);
    }

    struct rlimit rlim;
    if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
        benchmark_error_log("error: getrlimit failed: %s\n", strerror(errno));
        exit(1);
    }

    if (cfg.unix_socket != NULL &&
        (cfg.server != NULL || cfg.port > 0)) {
        benchmark_error_log("error: UNIX domain socket and TCP cannot be used together.\n");
        exit(1);
    }

    if (cfg.server != NULL && cfg.port > 0) {
        try {
            cfg.server_addr = new server_addr(cfg.server, cfg.port);
        } catch (std::runtime_error& e) {
            benchmark_error_log("%s:%u: error: %s\n",
                    cfg.server, cfg.port, e.what());
            exit(1);
        }
    }

    unsigned int fds_needed = (cfg.threads * cfg.clients) + (cfg.threads * 10) + 10;
    if (fds_needed > rlim.rlim_cur) {
        if (fds_needed > rlim.rlim_max && getuid() != 0) {
            benchmark_error_log("error: running the tool with this number of connections requires 'root' privilegs.\n");
            exit(1);
        }
        rlim.rlim_cur = rlim.rlim_max = fds_needed;

        if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
            benchmark_error_log("error: setrlimit failed: %s\n", strerror(errno));
            exit(1);
        }
    }

    // create and configure object generator
    object_generator* obj_gen = NULL;
    imported_keylist* keylist = NULL;
    if (!cfg.data_import) {
        if (cfg.data_verify) {
            fprintf(stderr, "error: use data-verify only with data-import\n");
            exit(1);
        }
        if (cfg.no_expiry) {
            fprintf(stderr, "error: use no-expiry only with data-import\n");
            exit(1);
        }
        
        obj_gen = new object_generator();
        assert(obj_gen != NULL);
    } else {
        // check paramters
        if (cfg.data_size ||
            cfg.data_size_list.is_defined() ||
            cfg.data_size_range.is_defined()) {
            fprintf(stderr, "error: data size cannot be specified when importing.\n");
            exit(1);
        }

        if (cfg.random_data) {
            fprintf(stderr, "error: random-data cannot be specified when importing.\n");
            exit(1);
        }

        if (!cfg.generate_keys &&
            (cfg.key_maximum || cfg.key_minimum || cfg.key_prefix)) {
                fprintf(stderr, "error: use key-minimum, key-maximum and key-prefix only with generate-keys.\n");
                exit(1);
        }

        if (!cfg.generate_keys) {        
            // read keys
            fprintf(stderr, "Reading keys from %s...", cfg.data_import);
            keylist = new imported_keylist(cfg.data_import);
            assert(keylist != NULL);
            
            if (!keylist->read_keys()) {
                fprintf(stderr, "\nerror: failed to read keys.\n");
                exit(1);
            } else {
                fprintf(stderr, " %u keys read.\n", keylist->size());
            }
        }

        obj_gen = new import_object_generator(cfg.data_import, keylist, cfg.no_expiry);
        assert(obj_gen != NULL);

        if (dynamic_cast<import_object_generator*>(obj_gen)->open_file() != true) {
            fprintf(stderr, "error: %s: failed to open.\n", cfg.data_import);
            exit(1);
        }
    }

    if (cfg.authenticate) {
        if (strcmp(cfg.protocol, "redis") != 0  &&
            strcmp(cfg.protocol, "memcache_binary") != 0) {
                fprintf(stderr, "error: authenticate can only be used with redis or memcache_binary.\n");
                usage();
        }
        if (strcmp(cfg.protocol, "memcache_binary") == 0 &&
            strchr(cfg.authenticate, ':') == NULL) {
                fprintf(stderr, "error: binary_memcache credentials must be in the form of USER:PASSWORD.\n");
                usage();
        }
    }
    if (!cfg.data_import) {
        obj_gen->set_random_data(cfg.random_data);
    }

    if (cfg.select_db > 0 && strcmp(cfg.protocol, "redis")) {
        fprintf(stderr, "error: select-db can only be used with redis protocol.\n");
        usage();
    }
    if (cfg.data_offset > 0) {
        if (cfg.data_offset > (1<<29)-1) {
            fprintf(stderr, "error: data-offset too long\n");
            usage();
        }
        if (cfg.expiry_range.min || cfg.expiry_range.max || strcmp(cfg.protocol, "redis")) {
            fprintf(stderr, "error: data-offset can only be used with redis protocol, and cannot be used with expiry\n");
            usage();
        }
    }
    if (cfg.data_size) {
        if (cfg.data_size_list.is_defined() || cfg.data_size_range.is_defined()) {
            fprintf(stderr, "error: data-size cannot be used with data-size-list or data-size-range.\n");
            usage();
        }
        obj_gen->set_data_size_fixed(cfg.data_size);
    } else if (cfg.data_size_list.is_defined()) {
        if (cfg.data_size_range.is_defined()) {
            fprintf(stderr, "error: data-size-list cannot be used with data-size-range.\n");
            usage();
        }
        obj_gen->set_data_size_list(&cfg.data_size_list);
    } else if (cfg.data_size_range.is_defined()) {
        obj_gen->set_data_size_range(cfg.data_size_range.min, cfg.data_size_range.max);
        obj_gen->set_data_size_pattern(cfg.data_size_pattern);
    } else if (!cfg.data_import) {
        fprintf(stderr, "error: data-size, data-size-list or data-size-range must be specified.\n");
        usage();
    }
    
    if (!cfg.data_import || cfg.generate_keys) {
        obj_gen->set_key_prefix(cfg.key_prefix);
        fprintf(stderr, "key prefix: %s \n", cfg.key_prefix);
        obj_gen->set_key_range(cfg.key_minimum, cfg.key_maximum);
    }
    if (cfg.key_stddev>0 || cfg.key_median>0) {
        if (cfg.key_pattern[0]!='G' && cfg.key_pattern[2]!='G') {
            fprintf(stderr, "error: key-stddev and key-median are only allowed together with key-pattern set to G.\n");
            usage();
        }   
        if (cfg.key_median!=0 && (cfg.key_median<cfg.key_minimum || cfg.key_median>cfg.key_maximum)) {
            fprintf(stderr, "error: key-median must be between key-minimum and key-maximum.\n");
            usage();
        }
        obj_gen->set_key_distribution(cfg.key_stddev, cfg.key_median);
    }
    obj_gen->set_expiry_range(cfg.expiry_range.min, cfg.expiry_range.max);

    // Prepare output file
    FILE *outfile;
    if (cfg.out_file != NULL) {
        fprintf(stderr, "Writing results to %s...\n", cfg.out_file);
        outfile = fopen(cfg.out_file, "w");
        if (!outfile) {
            perror(cfg.out_file);
        }
    } else {
        outfile = stdout;
    }

    if (!cfg.verify_only) {
        std::vector<run_stats> all_stats;
        for (unsigned int run_id = 1; run_id <= cfg.run_count; run_id++) {
            if (run_id > 1)
                sleep(1);   // let connections settle
            
            run_stats stats = run_benchmark(run_id, &cfg, obj_gen);
            all_stats.push_back(stats);
        }
        //
        // Print some run information        
        fprintf(outfile,
               "%-9u Threads\n"
               "%-9u Connections per thread\n"
               "%-9u %s\n",
               cfg.threads, cfg.clients, 
               cfg.requests > 0 ? cfg.requests : cfg.test_time,
               cfg.requests > 0 ? "Requests per thread"  : "Seconds");
        if (jsonhandler != NULL){
            jsonhandler->open_nesting("run information");
            jsonhandler->write_obj("Threads","%u",cfg.threads);
            jsonhandler->write_obj("Connections per thread","%u",cfg.clients);
            jsonhandler->write_obj(cfg.requests > 0 ? "Requests per thread"  : "Seconds","%u",
                                   cfg.requests > 0 ? cfg.requests : cfg.test_time);
            jsonhandler->close_nesting();
        }

        // If more than 1 run was used, compute best, worst and average
        if (cfg.run_count > 1) {
            unsigned int min_ops_sec = (unsigned int) -1;
            unsigned int max_ops_sec = 0;
            run_stats* worst = NULL;
            run_stats* best = NULL;        
            for (std::vector<run_stats>::iterator i = all_stats.begin(); i != all_stats.end(); i++) {
                unsigned long usecs = i->get_duration_usec();
                unsigned int ops_sec = (int)(((double)i->get_total_ops() / (usecs > 0 ? usecs : 1)) * 1000000);
                if (ops_sec < min_ops_sec || worst == NULL) {
                    min_ops_sec = ops_sec;                
                    worst = &(*i);
                }
                if (ops_sec > max_ops_sec || best == NULL) {
                    max_ops_sec = ops_sec;
                    best = &(*i);
                }
            }

            // Best results:
            best->print(outfile, !cfg.hide_histogram, "BEST RUN RESULTS", jsonhandler);
            // worst results:
            worst->print(outfile, !cfg.hide_histogram, "WORST RUN RESULTS", jsonhandler);
            // average results:
            run_stats average;
            average.aggregate_average(all_stats);
            char average_header[50];
            sprintf(average_header,"AGGREGATED AVERAGE RESULTS (%u runs)", cfg.run_count);
            average.print(outfile, !cfg.hide_histogram, average_header, jsonhandler);
        } else {
            all_stats.begin()->print(outfile, !cfg.hide_histogram, "ALL STATS", jsonhandler);
        }
    }

    // If needed, data verification is done now...
    if (cfg.data_verify) {
        struct event_base *verify_event_base = event_base_new();
        abstract_protocol *verify_protocol = protocol_factory(cfg.protocol);
        verify_client *client = new verify_client(verify_event_base, &cfg, verify_protocol, obj_gen);

        fprintf(outfile, "\n\nPerforming data verification...\n");

        // Run client in verification mode
        client->prepare();
        event_base_dispatch(verify_event_base);

        fprintf(outfile, "Data verification completed:\n"
                        "%-10llu keys verified successfuly.\n"
                        "%-10llu keys failed.\n",
                        client->get_verified_keys(),
                        client->get_errors());
        
        if (jsonhandler != NULL){
            jsonhandler->open_nesting("client verifications results");
            jsonhandler->write_obj("keys verified successfuly", "%-10llu",  client->get_verified_keys());
            jsonhandler->write_obj("keys failed", "%-10llu",  client->get_errors());
            jsonhandler->close_nesting();
        }

        // Clean up...
        delete client;
        delete verify_protocol;
        event_base_free(verify_event_base);
    }

    if (outfile != stdout) {
        fclose(outfile);
    }

    if (jsonhandler != NULL) {
        // closing the JSON
        delete jsonhandler;
    }

    delete obj_gen;
    if (keylist != NULL)
        delete keylist;
}
