// Copyright (C) 2016, 2017 Alexey Khrabrov, Bogdan Simion
//
// Distributed under the terms of the GNU General Public License.
//
// This file is part of Assignment 3, CSC469, Fall 2017.
//
// This is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This file is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this file.  If not, see <http://www.gnu.org/licenses/>.

// The key-value server implementation

#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/socket.h>
#include <sys/types.h>

#include "defs.h"
#include "hash.h"
#include "util.h"

// Program arguments

// Host name and port number of the metadata server
static char mserver_host_name[HOST_NAME_MAX] = "";
static uint16_t mserver_port = 0;

// Ports for listening to incoming connections from clients, servers and mserver
static uint16_t clients_port = 0;
static uint16_t servers_port = 0;
static uint16_t mservers_port = 0;

// Current server id and total number of servers
static int server_id = -1;
static int num_servers = 0;

// Log file name
static char log_file_name[PATH_MAX] = "";

static void usage(char **argv) {
    printf(
        "usage: %s -h <mserver host> -m <mserver port> -c <clients port> -s "
        "<servers port> "
        "-M <mservers port> -S <server id> -n <num servers> [-l <log file>]\n",
        argv[0]);
    printf("If the log file (-l) is not specified, log output is written to "
           "stdout\n");
}

// Returns false if the arguments are invalid
static bool parse_args(int argc, char **argv) {
    char option;
    while ((option = getopt(argc, argv, "h:m:c:s:M:S:n:l:")) != -1) {
        switch (option) {
        case 'h':
            strncpy(mserver_host_name, optarg, HOST_NAME_MAX);
            break;
        case 'm':
            mserver_port = atoi(optarg);
            break;
        case 'c':
            clients_port = atoi(optarg);
            break;
        case 's':
            servers_port = atoi(optarg);
            break;
        case 'M':
            mservers_port = atoi(optarg);
            break;
        case 'S':
            server_id = atoi(optarg);
            break;
        case 'n':
            num_servers = atoi(optarg);
            break;
        case 'l':
            strncpy(log_file_name, optarg, PATH_MAX);
            break;
        default:
            fprintf(stderr, "Invalid option: -%c\n", option);
            return false;
        }
    }

    return (mserver_host_name[0] != '\0') && (mserver_port != 0) &&
           (clients_port != 0) && (servers_port != 0) && (mservers_port != 0) &&
           (num_servers >= 3) && (server_id >= 0) && (server_id < num_servers);
}

// Socket for sending requests to the metadata server
static int mserver_fd_out = -1;
// Socket for receiving requests from the metadata server
static int mserver_fd_in = -1;

// Sockets for listening for incoming connections from clients, servers and
// mserver
static int my_clients_fd = -1;
static int my_servers_fd = -1;
static int my_mservers_fd = -1;

// Store fds for all connected clients, up to MAX_CLIENT_SESSIONS
#define MAX_CLIENT_SESSIONS 1000
static int client_fd_table[MAX_CLIENT_SESSIONS];

// Store fds for connected servers
#define MAX_SERVER_SESSIONS 2
static int server_fd_table[MAX_SERVER_SESSIONS] = {-1, -1};

// Storage for primary key set
hash_table primary_hash = {0};

// Storage for secondary key set
hash_table secondary_hash = {0};

// Primary server (the one that stores the primary copy for this server's
// secondary key set)
static int primary_sid = -1;
static int primary_fd = -1;

// Secondary server (the one that stores the secondary copy for this server's
// primary key set)
static int secondary_sid = -1;
static int secondary_fd = -1;

static void cleanup();

static const int hash_size = 65536;

static const int heartbeat_freq = 100000;

void *heartbeat() {
    for (;;) {
        mserver_ctrl_request request = {0};
        request.hdr.type = MSG_MSERVER_CTRL_REQ;
        request.type = HEARTBEAT;
        request.server_id = server_id;

        if (!send_msg(mserver_fd_out, &request, sizeof(request))) {
            log_error("sid %d: Failed to send heartbeat\n", server_id);
        }

        usleep(heartbeat_freq);
    }

    pthread_exit(NULL);
}

static bool update_primary = false;

void sync_kv(const char key[KEY_SIZE], void *value, size_t value_sz,
             void *args) {
    (void)args;

    int fd = update_primary ? primary_fd : secondary_fd;

    char replicate_request_buffer[MAX_MSG_LEN] = {0};
    operation_request *replicate_request =
        (operation_request *)replicate_request_buffer;
    replicate_request->hdr.type = MSG_OPERATION_REQ;
    memcpy(replicate_request->key, key, KEY_SIZE);
    replicate_request->type = OP_PUT;
    replicate_request->sid = server_id;
    memcpy(replicate_request->value, value, value_sz);

    char recv_buffer[MAX_MSG_LEN] = {0};
    if (!send_msg(fd, replicate_request,
                  sizeof(*replicate_request) + value_sz) ||
        !recv_msg(fd, recv_buffer, sizeof(recv_buffer), MSG_OPERATION_RESP)) {
        log_error("%d sid: asynchronous update failed\n", server_id);
    }
}

void *sync_hashtable(void *args) {
    (void)args;

    hash_table *table = update_primary ? &secondary_hash : &primary_hash;
    hash_iterate(table, sync_kv, NULL);

    mserver_ctrl_request request = {0};
    request.hdr.type = MSG_MSERVER_CTRL_REQ;
    request.type = update_primary ? UPDATED_PRIMARY : UPDATED_SECONDARY;
    request.server_id = server_id;

    if (!send_msg(mserver_fd_out, &request, sizeof(request))) {
        log_error(
            "sid %d: Failed to notify mserver that the async update finished\n",
            server_id);
    }

    pthread_exit(NULL);
}

// Initialize and start the server
static bool init_server() {
    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
        client_fd_table[i] = -1;
    }

    // Get the host name that server is running on
    char my_host_name[HOST_NAME_MAX] = "";
    if (get_local_host_name(my_host_name, sizeof(my_host_name)) < 0) {
        return false;
    }
    log_write("%s Server starts on host: %s\n", current_time_str(),
              my_host_name);

    // Create sockets for incoming connections from clients and other servers
    if (((my_clients_fd =
              create_server(clients_port, MAX_CLIENT_SESSIONS, NULL)) < 0) ||
        ((my_servers_fd =
              create_server(servers_port, MAX_SERVER_SESSIONS, NULL)) < 0) ||
        ((my_mservers_fd = create_server(mservers_port, 1, NULL)) < 0)) {
        goto cleanup;
    }

    // Connect to mserver to "register" that we are live
    if ((mserver_fd_out = connect_to_server(mserver_host_name, mserver_port)) <
        0) {
        goto cleanup;
    }

    // Determine the ids of replica servers
    primary_sid = primary_server_id(server_id, num_servers);
    secondary_sid = secondary_server_id(server_id, num_servers);

    // Initialize key-value storage
    if (!hash_init(&primary_hash, hash_size) ||
        !hash_init(&secondary_hash, hash_size)) {
        goto cleanup;
    }

    pthread_t heartbeat_thread;
    if (pthread_create(&heartbeat_thread, NULL, heartbeat, NULL)) {
        log_error("sid %d: Failed to create heartbeat thread\n", server_id);
        goto cleanup;
    }

    log_write("Server initialized\n");
    return true;

cleanup:
    cleanup();
    return false;
}

// Hash iterator for freeing memory used by values; called during storage
// cleanup
static void clean_iterator_f(const char key[KEY_SIZE], void *value,
                             size_t value_sz, void *arg) {
    (void)key;
    (void)value_sz;
    (void)arg;

    assert(value != NULL);
    free(value);
}

// Cleanup and release all the resources
static void cleanup() {
    log_write("Cleaning up and exiting ...\n");

    close_safe(&mserver_fd_out);
    close_safe(&mserver_fd_in);
    close_safe(&my_clients_fd);
    close_safe(&my_servers_fd);
    close_safe(&my_mservers_fd);
    close_safe(&secondary_fd);
    close_safe(&primary_fd);

    for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
        close_safe(&(client_fd_table[i]));
    }
    for (int i = 0; i < MAX_SERVER_SESSIONS; i++) {
        close_safe(&(server_fd_table[i]));
    }

    hash_iterate(&primary_hash, clean_iterator_f, NULL);
    hash_cleanup(&primary_hash);

    hash_iterate(&secondary_hash, clean_iterator_f, NULL);
    hash_cleanup(&secondary_hash);
}

// Connection will be closed after calling this function regardless of result
static void process_client_message(int fd) {
    log_write("%s Receiving a client message\n", current_time_str());

    // Read and parse the message
    char req_buffer[MAX_MSG_LEN] = {0};
    if (!recv_msg(fd, req_buffer, MAX_MSG_LEN, MSG_OPERATION_REQ)) {
        return;
    }
    operation_request *request = (operation_request *)req_buffer;

    // Initialize the response
    char resp_buffer[MAX_MSG_LEN] = {0};
    operation_response *response = (operation_response *)resp_buffer;
    response->hdr.type = MSG_OPERATION_RESP;
    uint16_t value_sz = 0;

    // Check that requested key is valid
    hash_table *table = NULL;
    int key_srv_id = key_server_id(request->key, num_servers);
    if (key_srv_id == server_id) {
        table = &primary_hash;
    } else if (key_srv_id == primary_sid) {
        table = &secondary_hash;
    } else {
        log_error("sid %d: Invalid client key %s sid %d\n", server_id,
                  key_to_str(request->key), key_srv_id);
        // This sould be considered a server failure (e.g. the metadata server
        // directed a client to the wrong server)
        response->status = SERVER_FAILURE;
        send_msg(fd, response, sizeof(*response) + value_sz);
        return;
    }

    // Process the request based on its type
    switch (request->type) {
    case OP_NOOP:
        response->status = SUCCESS;
        break;

    case OP_GET: {
        void *data = NULL;
        size_t size = 0;

        // Get the value for requested key from the hash table
        if (!hash_get(table, request->key, &data, &size)) {
            log_write("Key %s not found\n", key_to_str(request->key));
            response->status = KEY_NOT_FOUND;
            break;
        }

        // Copy the stored value into the response buffer
        memcpy(response->value, data, size);
        value_sz = size;

        response->status = SUCCESS;
        break;
    }

    case OP_PUT: {
        // Need to copy the value to dynamically allocated memory
        size_t value_size = request->hdr.length - sizeof(*request);
        void *value_copy = malloc(value_size);
        if (value_copy == NULL) {
            log_perror("malloc");
            log_error("sid %d: Out of memory\n", server_id);
            response->status = OUT_OF_SPACE;
            break;
        }
        memcpy(value_copy, request->value, value_size);

        void *old_value = NULL;
        size_t old_value_sz = 0;

        // Put the <key, value> pair into the hash table
        hash_lock(table, request->key);
        if (!hash_put(table, request->key, value_copy, value_size, &old_value,
                      &old_value_sz)) {
            log_error("sid %d: Out of memory\n", server_id);
            free(value_copy);
            response->status = OUT_OF_SPACE;
            hash_unlock(table, request->key);
            break;
        }

        char replicate_request_buffer[MAX_MSG_LEN] = {0};
        operation_request *replicate_request =
            (operation_request *)replicate_request_buffer;
        replicate_request->hdr.type = MSG_OPERATION_REQ;
        memcpy(replicate_request->key, request->key, KEY_SIZE);
        replicate_request->type = OP_PUT;
        replicate_request->sid = server_id;
        memcpy(replicate_request->value, value_copy, value_size);

        int replica_fd = (table == &primary_hash) ? secondary_fd : primary_fd;

        if (!send_msg(replica_fd, replicate_request,
                      sizeof(*replicate_request) + value_size)) {
            log_error("sid %d: Replication to %d failed\n", server_id,
                      replica_fd);
            hash_unlock(table, request->key);
            return;
        }

        // Need to free the old value (if there was any)
        if (old_value != NULL) {
            free(old_value);
        }

        hash_unlock(table, request->key);

        response->status = SUCCESS;
        break;
    }

    default:
        log_error("sid %d: Invalid client operation type\n", server_id);
        return;
    }

    // Send reply to the client
    send_msg(fd, response, sizeof(*response) + value_sz);
}

// Returns false if either the message was invalid or if this was the last
// message (in both cases the connection will be closed)
static bool process_server_message(int fd) {
    log_write("%s Receiving a server message\n", current_time_str());

    // Read and parse the message
    char req_buffer[MAX_MSG_LEN] = {0};
    if (!recv_msg(fd, req_buffer, MAX_MSG_LEN, MSG_OPERATION_REQ)) {
        return false;
    }
    operation_request *request = (operation_request *)req_buffer;

    // NOOP operation request is used to indicate the last message in an UPDATE
    // sequence
    if (request->type == OP_NOOP) {
        log_write("Received the last server message, closing connection\n");
        return false;
    }

    // Initialize the response
    char resp_buffer[MAX_MSG_LEN] = {0};
    operation_response *response = (operation_response *)resp_buffer;
    response->hdr.type = MSG_OPERATION_RESP;
    uint16_t value_sz = 0;

    switch (request->type) {
    case OP_GET: {
        log_error("process_server_message() OP_GET not implemented");
        return false;
    }

    case OP_PUT: {
        // Need to copy the value to dynamically allocated memory
        size_t value_size = request->hdr.length - sizeof(*request);
        void *value_copy = malloc(value_size);
        if (value_copy == NULL) {
            log_perror("malloc");
            log_error("sid %d: Out of memory\n", server_id);
            response->status = OUT_OF_SPACE;
            break;
        }
        memcpy(value_copy, request->value, value_size);

        void *old_value = NULL;
        size_t old_value_sz = 0;

        hash_table *table =
            (request->sid == secondary_sid) ? &primary_hash : &secondary_hash;

        // Put the <key, value> pair into the hash table
        hash_lock(table, request->key);
        if (!hash_put(table, request->key, value_copy, value_size, &old_value,
                      &old_value_sz)) {
            log_error("sid %d: Out of memory\n", server_id);
            free(value_copy);
            response->status = OUT_OF_SPACE;
            hash_unlock(table, request->key);
            break;
        }
        hash_unlock(table, request->key);

        // Need to free the old value (if there was any)
        if (old_value != NULL) {
            free(old_value);
        }

        response->status = SUCCESS;
        break;
    }

    default:
        log_error("sid %d: Invalid server operation type\n", server_id);
        return false;
    }

    send_msg(fd, response, sizeof(*response) + value_sz);

    return true;
}

// Returns false if the message was invalid (so the connection will be closed)
// Sets *shutdown_requested to true if received a SHUTDOWN message (so the
// server will terminate)
static bool process_mserver_message(int fd, bool *shutdown_requested) {
    assert(shutdown_requested != NULL);
    *shutdown_requested = false;

    log_write("%s Receiving a metadata server message\n", current_time_str());

    // Read and parse the message
    char req_buffer[MAX_MSG_LEN] = {0};
    if (!recv_msg(fd, req_buffer, MAX_MSG_LEN, MSG_SERVER_CTRL_REQ)) {
        return false;
    }
    server_ctrl_request *request = (server_ctrl_request *)req_buffer;

    // Initialize the response
    server_ctrl_response response = {0};
    response.hdr.type = MSG_SERVER_CTRL_RESP;

    // Process the request based on its type
    switch (request->type) {
    case SET_SECONDARY:
        close_safe(&secondary_fd);
        response.status = ((secondary_fd = connect_to_server(
                                request->host_name, request->port)) < 0)
                              ? CTRLREQ_FAILURE
                              : CTRLREQ_SUCCESS;
        break;

    case SHUTDOWN:
        *shutdown_requested = true;
        return true;

    case UPDATE_PRIMARY:
        close_safe(&primary_fd);
        response.status = ((primary_fd = connect_to_server(request->host_name,
                                                           request->port)) < 0)
                              ? CTRLREQ_FAILURE
                              : CTRLREQ_SUCCESS;

        if (response.status != CTRLREQ_FAILURE) {
            update_primary = true;
            pthread_t sync_primary_thread;
            pthread_create(&sync_primary_thread, NULL, sync_hashtable, NULL);
        }

        break;

    case UPDATE_SECONDARY:
        close_safe(&secondary_fd);
        response.status = ((secondary_fd = connect_to_server(
                                request->host_name, request->port)) < 0)
                              ? CTRLREQ_FAILURE
                              : CTRLREQ_SUCCESS;

        if (response.status != CTRLREQ_FAILURE) {
            update_primary = false;
            pthread_t sync_secondary_thread;
            pthread_create(&sync_secondary_thread, NULL, sync_hashtable, NULL);
        }

        break;

    default: // impossible
        assert(false);
        break;
    }

    send_msg(fd, &response, sizeof(response));
    return true;
}

// Returns false if stopped due to errors, true if shutdown was requested
static bool run_server_loop() {
    // Usual preparation stuff for select()
    fd_set rset, allset;
    FD_ZERO(&allset);
    FD_SET(my_clients_fd, &allset);
    FD_SET(my_mservers_fd, &allset);
    FD_SET(my_servers_fd, &allset);

    int maxfd = max(my_clients_fd, my_mservers_fd);

    // Server sits in an infinite loop waiting for incoming connections from
    // mserver/clients and for incoming messages from already connected
    // mserver/clients

    for (;;) {
        rset = allset;

        int num_ready_fds = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (num_ready_fds < 0) {
            log_perror("select");
            return false;
        }

        if (num_ready_fds <= 0) {
            continue;
        }

        // Incoming connection from the metadata server
        if (FD_ISSET(my_mservers_fd, &rset)) {
            int fd_idx = accept_connection(my_mservers_fd, &mserver_fd_in, 1);
            if (fd_idx >= 0) {
                FD_SET(mserver_fd_in, &allset);
                maxfd = max(maxfd, mserver_fd_in);
            }
            assert(fd_idx == 0);

            if (--num_ready_fds <= 0) {
                continue;
            }
        }

        // Check for any messages from the metadata server
        if ((mserver_fd_in != -1) && FD_ISSET(mserver_fd_in, &rset)) {
            bool shutdown_requested = false;
            if (!process_mserver_message(mserver_fd_in, &shutdown_requested)) {
                // Received an invalid message, close the connection
                log_error("sid %d: Closing mserver connection\n", server_id);
                FD_CLR(mserver_fd_in, &allset);
                close_safe(&(mserver_fd_in));
            } else if (shutdown_requested) {
                return true;
            }

            if (--num_ready_fds <= 0) {
                continue;
            }
        }

        // Incoming connection from a client
        if (FD_ISSET(my_clients_fd, &rset)) {
            int fd_idx = accept_connection(my_clients_fd, client_fd_table,
                                           MAX_CLIENT_SESSIONS);
            if (fd_idx >= 0) {
                FD_SET(client_fd_table[fd_idx], &allset);
                maxfd = max(maxfd, client_fd_table[fd_idx]);
            }

            if (--num_ready_fds <= 0) {
                continue;
            }
        }

        // Check for any messages from connected clients
        for (int i = 0; i < MAX_CLIENT_SESSIONS; i++) {
            if ((client_fd_table[i] != -1) &&
                FD_ISSET(client_fd_table[i], &rset)) {
                process_client_message(client_fd_table[i]);
                // Close connection after processing (semantics are "one
                // connection per request")
                FD_CLR(client_fd_table[i], &allset);
                close_safe(&(client_fd_table[i]));

                if (--num_ready_fds <= 0) {
                    break;
                }
            }
        }

        // Incoming connection from a server
        if (FD_ISSET(my_servers_fd, &rset)) {
            int fd_idx = accept_connection(my_servers_fd, server_fd_table,
                                           MAX_SERVER_SESSIONS);
            if (fd_idx >= 0) {
                FD_SET(server_fd_table[fd_idx], &allset);
                maxfd = max(maxfd, server_fd_table[fd_idx]);
            }

            if (--num_ready_fds <= 0) {
                continue;
            }
        }

        // Check for any messages from connected servers
        for (int i = 0; i < MAX_SERVER_SESSIONS; i++) {
            if ((server_fd_table[i] != -1) &&
                FD_ISSET(server_fd_table[i], &rset)) {
                if (!process_server_message(server_fd_table[i])) {
                    // Received an invalid message, close the connection
                    log_error("sid %d: Closing server connection\n", server_id);
                    FD_CLR(server_fd_table[i], &allset);
                    close_safe(&(server_fd_table[i]));
                }

                if (--num_ready_fds <= 0) {
                    break;
                }
            }
        }
    }

    // Should not get here
    return false;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    if (!parse_args(argc, argv)) {
        usage(argv);
        return 1;
    }

    open_log(log_file_name);

    if (!init_server()) {
        return 1;
    }

    bool result = run_server_loop();

    cleanup();
    return result ? 0 : 1;
}
