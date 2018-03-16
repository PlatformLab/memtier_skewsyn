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

#ifndef MEMTIER_BENCHMARK_SHARD_CONNECTION_H
#define MEMTIER_BENCHMARK_SHARD_CONNECTION_H

#include <poll.h>
#include <queue>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <event2/event.h>
#include <event2/buffer.h>

#include "protocol.h"

// forward decleration
class connections_manager;
struct benchmark_config;
class abstract_protocol;
class object_generator;

enum authentication_state { auth_none, auth_sent, auth_done };
enum select_db_state { select_none, select_sent, select_done };
enum cluster_slots_state { slots_none, slots_sent, slots_done };

enum request_type { rt_unknown, rt_set, rt_get, rt_wait, rt_auth, rt_select_db, rt_cluster_slots };
struct request {
    request_type m_type;
    struct timeval m_sent_time;
    unsigned int m_size;
    unsigned int m_keys;

    request(request_type type, unsigned int size, struct timeval* sent_time, unsigned int keys);
    virtual ~request(void) {}
};

struct verify_request : public request {
    char *m_key;
    unsigned int m_key_len;
    char *m_value;
    unsigned int m_value_len;

    verify_request(request_type type,
                   unsigned int size,
                   struct timeval* sent_time,
                   unsigned int keys,
                   const char *key,
                   unsigned int key_len,
                   const char *value,
                   unsigned int value_len);
    virtual ~verify_request(void);
};

class shard_connection {
    friend void cluster_client_event_handler(evutil_socket_t sfd, short evtype, void *opaque);

public:
    shard_connection(unsigned int id, connections_manager* conn_man, benchmark_config* config,
                     struct event_base* event_base, abstract_protocol* abs_protocol);
    ~shard_connection();

    void set_address_port(const char* address, const char* port);
    int connect(struct connect_info* addr);
    void disconnect();

    void send_wait_command(struct timeval* sent_time,
                            unsigned int num_slaves, unsigned int timeout);
    void send_set_command(struct timeval* sent_time, const char *key, int key_len,
                          const char *value, int value_len, int expiry, unsigned int offset);
    void send_get_command(struct timeval* sent_time,
                          const char *key, int key_len, unsigned int offset);
    void send_mget_command(struct timeval* sent_time, const keylist* key_list);
    void send_verify_get_command(struct timeval* sent_time, const char *key, int key_len,
                                 const char *value, int value_len, int expiry, unsigned int offset);

    void set_authentication() {
        m_authentication = auth_none;
    }

    void set_select_db() {
        m_db_selection = select_none;
    }

    void set_cluster_slots() {
        m_cluster_slots = slots_none;
    }

    unsigned int get_id() {
        return m_id;
    }

    abstract_protocol* get_protocol() {
        return m_protocol;
    }

    const char* get_address() {
        return m_address;
    }

    const char* get_port() {
        return m_port;
    }

    int check_sockfd_writable();
    int check_sockfd_readable();
    void gurantee_sockfd_dispatch();

private:
    void setup_event();
    int setup_socket(struct connect_info* addr);

    bool is_conn_setup_done();
    void send_conn_setup_commands(struct timeval timestamp);

    request* pop_req();
    void push_req(request* req);

    void process_response(void);
    void process_first_request();
    void fill_pipeline(void);

    void handle_event(short evtype);

    unsigned int m_id;
    connections_manager* m_conns_manager;
    benchmark_config* m_config;

    int m_sockfd;
    char* m_address;
    char* m_port;
    struct sockaddr_un* m_unix_sockaddr;

    struct evbuffer* m_read_buf;
    struct evbuffer* m_write_buf;

    struct event_base* m_event_base;
    struct event* m_event;

    abstract_protocol* m_protocol;
    std::queue<request *>* m_pipeline;

    int m_pending_resp;
    bool m_connected;

    enum authentication_state m_authentication;
    enum select_db_state m_db_selection;
    enum cluster_slots_state m_cluster_slots;
};

#endif //MEMTIER_BENCHMARK_SHARD_CONNECTION_H
