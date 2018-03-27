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

#ifndef _CLIENT_H
#define _CLIENT_H

#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <vector>
#include <queue>
#include <map>
#include <iterator>
#include <event2/event.h>
#include <event2/buffer.h>

#include "protocol.h"
#include "JSON_handler.h"
#include "config_types.h"
#include "shard_connection.h"
#include "connections_manager.h"
#include "obj_gen.h"
#include "memtier_benchmark.h"

#define MAIN_CONNECTION m_connections[0]

class client;               // forward decl
class client_group;         // forward decl
struct benchmark_config;

class object_generator;
class data_object;

typedef std::map<float, int> latency_map;
typedef std::map<float, int>::iterator latency_map_itr;
typedef std::map<float, int>::const_iterator latency_map_itr_const;

class run_stats {
protected:
    struct one_second_stats {
        unsigned int m_second;        // from start of test
        unsigned long int m_bytes_get;
        unsigned long int m_bytes_set;
        unsigned long int m_ops_get;
        unsigned long int m_ops_set;
        unsigned long int m_ops_wait;

        unsigned int m_get_hits;
        unsigned int m_get_misses;

        unsigned long long int m_total_get_latency;
        unsigned long long int m_total_set_latency;
        unsigned long long int m_total_wait_latency;

        one_second_stats(unsigned int second);
        void reset(unsigned int second);
        void merge(const one_second_stats& other);
    };

    friend bool one_second_stats_predicate(const run_stats::one_second_stats& a, const run_stats::one_second_stats& b);    

    struct timeval m_start_time;
    struct timeval m_end_time;

    struct totals {
        double m_ops_sec_set;
        double m_ops_sec_get;
        double m_ops_sec_wait;
        double m_ops_sec;

        double m_hits_sec;
        double m_misses_sec;

        double m_bytes_sec_set;
        double m_bytes_sec_get;
        double m_bytes_sec;
        
        double m_latency_set;
        double m_latency_get;
        double m_latency_wait;
        double m_latency;

        unsigned long int m_bytes;
        unsigned long int m_ops_set;
        unsigned long int m_ops_get;
        unsigned long int m_ops_wait;
        unsigned long int m_ops;

        totals();
        void add(const totals& other);
    } m_totals;

    std::vector<one_second_stats> m_stats;
    one_second_stats m_cur_stats;

    latency_map m_get_latency_map;
    latency_map m_set_latency_map;
    latency_map m_wait_latency_map;
    void roll_cur_stats(struct timeval* ts);

public:
    run_stats();
    void set_start_time(struct timeval* start_time);
    void set_end_time(struct timeval* end_time);

    void update_get_op(struct timeval* ts, unsigned int bytes, unsigned int latency, unsigned int hits, unsigned int misses);
    void update_set_op(struct timeval* ts, unsigned int bytes, unsigned int latency);
    void update_wait_op(struct timeval* ts, unsigned int latency);

    void aggregate_average(const std::vector<run_stats>& all_stats);
    void summarize(totals& result) const;
    void merge(const run_stats& other, int iteration);
    bool save_csv(const char *filename);
    void debug_dump(void);
    void print(FILE *file, bool histogram, const char* header = NULL, json_handler* jsonhandler = NULL);
    
    unsigned int get_duration(void);
    unsigned long int get_duration_usec(void);
    unsigned long int get_total_bytes(void);
    unsigned long int get_total_ops(void);
    unsigned long int get_total_latency(void);
 };

class client : public connections_manager {
protected:

    std::vector<shard_connection*> m_connections;

    struct event_base* m_event_base;
    bool m_initialized;
    bool m_end_set;

    // test related
    benchmark_config* m_config;
    object_generator* m_obj_gen;
    run_stats m_stats;

    unsigned int m_reqs_processed;      // requests processed (responses received)
    unsigned int m_reqs_generated;      // requests generated (wait for responses)
    unsigned int m_set_ratio_count;     // number of sets counter (overlaps on ratio)
    unsigned int m_get_ratio_count;     // number of gets counter (overlaps on ratio)

    unsigned long m_tot_set_ops;        // Total number of SET ops
    unsigned long m_tot_wait_ops;       // Total number of WAIT ops

    keylist *m_keylist;                 // used to construct multi commands

    static pthread_mutex_t m_skew_mutex; // used to serialize skewed assignment to memcached server
    static int skew_count;               // used to count the number of clients
                                         // piled up on thread "0" for memcached server
    static int total_conns;              // used to count the total created connections
    static int real_conns;               // used to count the real number of connections to the server
public:
    client(client_group* group);
    client(struct event_base *event_base, benchmark_config *config, abstract_protocol *protocol, object_generator *obj_gen);
    virtual ~client();
    virtual bool setup_client(benchmark_config *config, abstract_protocol *protocol, object_generator *obj_gen);
    virtual int prepare(void);

    bool initialized(void);

    run_stats* get_stats(void) { return &m_stats; }

    // client manager api's
    unsigned int get_reqs_processed() {
        return m_reqs_processed;
    }

    void inc_reqs_processed() {
        m_reqs_processed++;
    }

    unsigned int get_reqs_generated() {
        return m_reqs_generated;
    }

    void inc_reqs_generated() {
        m_reqs_generated++;
    }

    virtual void handle_cluster_slots(protocol_response *r) {
        assert(false && "handle_cluster_slots not supported");
    }

    virtual void handle_response(struct timeval timestamp, request *request, protocol_response *response);
    virtual bool finished(void);
    virtual void set_start_time();
    virtual void set_end_time();
    virtual void create_request(struct timeval timestamp, unsigned int conn_id);
    virtual bool hold_pipeline(unsigned int conn_id);
    virtual int connect(void);
    virtual void disconnect(void);

    // Utility function to get the object iterator type based on the config
    inline int obj_iter_type(benchmark_config *cfg, unsigned char index)
    {
        if (cfg->key_pattern[index] == 'R')
            return OBJECT_GENERATOR_KEY_RANDOM;
        else if (cfg->key_pattern[index] == 'G')
            return OBJECT_GENERATOR_KEY_GAUSSIAN;
        return OBJECT_GENERATOR_KEY_SET_ITER;
    }

    // Corresponding server thread id
    int serverTid;
};

class verify_client : public client {
protected:
    bool m_finished;
    unsigned long long int m_verified_keys;
    unsigned long long int m_errors;

    virtual bool finished(void);
    virtual void create_request(struct timeval timestamp, unsigned int conn_id);
    virtual void handle_response(struct timeval timestamp, request *request, protocol_response *response);
public:
    verify_client(struct event_base *event_base, benchmark_config *config, abstract_protocol *protocol, object_generator *obj_gen);
    unsigned long long int get_verified_keys(void);
    unsigned long long int get_errors(void);
};

class client_group {
protected:
    struct event_base* m_base;
    benchmark_config *m_config;
    abstract_protocol* m_protocol;
    object_generator* m_obj_gen;
public:
    client_group(benchmark_config *cfg, abstract_protocol *protocol, object_generator* obj_gen);
    ~client_group();

    int create_clients(int count);
    int prepare(void);
    void run(void);

    void write_client_stats(const char *prefix);

    struct event_base *get_event_base(void) { return m_base; }
    benchmark_config *get_config(void) { return m_config; }
    abstract_protocol* get_protocol(void) { return m_protocol; }
    object_generator* get_obj_gen(void) { return m_obj_gen; }    

    unsigned long int get_total_bytes(void);
    unsigned long int get_total_ops(void);
    unsigned long int get_total_latency(void);
    unsigned long int get_duration_usec(void);

    void merge_run_stats(run_stats* target);
    std::vector<client*> m_clients;
};


#endif	/* _CLIENT_H */
