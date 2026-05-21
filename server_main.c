/*
 * server_main.c
 *
 * Main entry point for animate_server.
 */

#include "server_common.h"
#include "connection.h"
#include "threadpool.h"
#include "rpc.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define CLIENT_READ_BUFFER 4096

typedef struct server_client {
    client_state_t state;
    char read_buffer[CLIENT_READ_BUFFER];
    size_t read_len;
    struct server_client *next;
} server_client_t;

static void usage(const char *program_name) {
    fprintf(stderr, "Usage: %s <threadpool_size>\n", program_name);
    fprintf(stderr, "  threadpool_size: Number of worker threads (minimum 1)\n");
    exit(1);
}

static int parse_arguments(int argc, char *argv[], int *threadpool_size) {
    if (argc != 2) {
        return -1;
    }

    char *endptr = NULL;
    errno = 0;
    long size = strtol(argv[1], &endptr, 10);
    if (errno != 0 || endptr == argv[1] || *endptr != '\0') {
        return -1;
    }
    if (size < 1 || size > INT_MAX) {
        return -1;
    }

    *threadpool_size = (int)size;
    return 0;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        return -1;
    }
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static server_client_t *find_client(server_client_t *clients, pid_t client_pid) {
    for (server_client_t *client = clients; client != NULL; client = client->next) {
        if (client->state.client_pid == client_pid) {
            return client;
        }
    }
    return NULL;
}

static void close_client(server_client_t *client, threadpool_t *pool, rpc_context_t *ctx) {
    if (client->state.disconnected) {
        return;
    }

    rpc_client_disconnected(ctx, &client->state);
    client->state.disconnected = true;
    threadpool_remove_client(pool, &client->state);

    if (client->state.fifo_c2s_fd != -1) {
        close(client->state.fifo_c2s_fd);
        client->state.fifo_c2s_fd = -1;
    }
    if (client->state.fifo_s2c_fd != -1) {
        close(client->state.fifo_s2c_fd);
        client->state.fifo_s2c_fd = -1;
    }
    unlink_fifos(client->state.client_pid);
}

static int accept_client(pid_t client_pid, server_client_t **clients,
                         threadpool_t *pool) {
    if (client_pid <= 0 || find_client(*clients, client_pid) != NULL) {
        return 0;
    }

    if (create_fifos(client_pid) == -1) {
        return -1;
    }
    if (signal_client_ready(client_pid) == -1) {
        unlink_fifos(client_pid);
        return -1;
    }

    char c2s_path[MAX_FIFO_NAME_LEN];
    char s2c_path[MAX_FIFO_NAME_LEN];
    if (get_fifo_paths(client_pid, c2s_path, s2c_path) == -1) {
        unlink_fifos(client_pid);
        return -1;
    }

    int c2s_fd = open(c2s_path, O_RDONLY | O_NONBLOCK);
    if (c2s_fd == -1) {
        perror("open server C2S FIFO");
        unlink_fifos(client_pid);
        return -1;
    }

    int s2c_fd = open(s2c_path, O_WRONLY);
    if (s2c_fd == -1) {
        perror("open server S2C FIFO");
        close(c2s_fd);
        unlink_fifos(client_pid);
        return -1;
    }
    if (set_nonblocking(s2c_fd) == -1) {
        perror("fcntl server S2C FIFO");
    }

    server_client_t *client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(c2s_fd);
        close(s2c_fd);
        unlink_fifos(client_pid);
        return -1;
    }

    client->state.client_pid = client_pid;
    client->state.fifo_c2s_fd = c2s_fd;
    client->state.fifo_s2c_fd = s2c_fd;
    client->state.last_activity = time(NULL);
    if (pthread_mutex_init(&client->state.output_mutex, NULL) != 0) {
        close(c2s_fd);
        close(s2c_fd);
        unlink_fifos(client_pid);
        free(client);
        return -1;
    }

    if (threadpool_register_client(pool, &client->state) == -1) {
        pthread_mutex_destroy(&client->state.output_mutex);
        close(c2s_fd);
        close(s2c_fd);
        unlink_fifos(client_pid);
        free(client);
        return -1;
    }

    client->next = *clients;
    *clients = client;
    fprintf(stderr, "[INFO] Connected client %d\n", client_pid);
    return 0;
}

static void submit_line(server_client_t *client, threadpool_t *pool,
                        const char *line, size_t len) {
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return;
    }

    memcpy(copy, line, len);
    copy[len] = '\0';
    if (threadpool_submit(pool, &client->state, copy) == -1) {
        free(copy);
    }
}

static void process_buffered_lines(server_client_t *client, threadpool_t *pool) {
    size_t start = 0;

    for (size_t i = 0; i < client->read_len; i++) {
        if (client->read_buffer[i] == '\n') {
            submit_line(client, pool, client->read_buffer + start, i - start + 1);
            start = i + 1;
        }
    }

    if (start > 0) {
        memmove(client->read_buffer, client->read_buffer + start,
                client->read_len - start);
        client->read_len -= start;
    }

    if (client->read_len == sizeof(client->read_buffer)) {
        submit_line(client, pool, client->read_buffer, client->read_len);
        client->read_len = 0;
    }
}


static void cleanup_inactive_clients(server_client_t *clients, threadpool_t *pool,
                                     rpc_context_t *ctx) {
    time_t now = time(NULL);

    for (server_client_t *client = clients; client != NULL; client = client->next) {
        if (client->state.disconnected) {
            continue;
        }
        if (client->state.disconnect_after != 0 && now >= client->state.disconnect_after) {
            close_client(client, pool, ctx);
            continue;
        }
        if (kill(client->state.client_pid, 0) == -1 && errno == ESRCH) {
            close_client(client, pool, ctx);
        }
    }
}

static void read_client_requests(server_client_t *client, threadpool_t *pool, rpc_context_t *ctx) {
    while (!client->state.disconnected) {
        ssize_t n = read(client->state.fifo_c2s_fd,
                         client->read_buffer + client->read_len,
                         sizeof(client->read_buffer) - client->read_len);
        if (n > 0) {
            client->read_len += (size_t)n;
            client->state.last_activity = time(NULL);
            process_buffered_lines(client, pool);
            continue;
        }
        if (n == 0) {
            close_client(client, pool, ctx);
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        perror("read client FIFO");
        close_client(client, pool, ctx);
        break;
    }
}

int main(int argc, char *argv[]) {
    int threadpool_size = 0;
    if (parse_arguments(argc, argv, &threadpool_size) == -1) {
        usage(argv[0]);
    }

    /* Block SIGUSR1/SIGUSR2 before creating threads so workers inherit blocked mask */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    sigaddset(&set, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &set, NULL);

    /* Initialize signal handlers BEFORE creating threadpool */
    if (server_init_signals() == -1) {
        fprintf(stderr, "Failed to initialize signal handlers\n");
        return 1;
    }

    /* Unblock signals on main thread only - workers will keep them blocked */
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);

    rpc_context_t rpc_ctx;
    rpc_ctx.auth = auth_load("users.txt");
    rpc_ctx.handles = handles_create();
    if (rpc_ctx.auth == NULL || rpc_ctx.handles == NULL) {
        fprintf(stderr, "Failed to initialize server state\n");
        auth_destroy(rpc_ctx.auth);
        handles_destroy(rpc_ctx.handles);
        return 1;
    }

    threadpool_t *pool = threadpool_create((size_t)threadpool_size,
                                           rpc_handle_request, &rpc_ctx);
    if (pool == NULL) {
        perror("threadpool_create");
        auth_destroy(rpc_ctx.auth);
        handles_destroy(rpc_ctx.handles);
        return 1;
    }

    printf("Server PID: %d\n", getpid());
    fflush(stdout);

    fprintf(stderr, "[INFO] Server running with %d worker threads\n", threadpool_size);

    server_client_t *clients = NULL;
    while (1) {
        pid_t pending_client = server_take_pending_client();
        if (pending_client > 0) {
            fprintf(stderr, "[DEBUG] Accepting client PID %d\n", (int)pending_client);
            if (accept_client(pending_client, &clients, pool) == -1) {
                fprintf(stderr, "[WARN] Failed to accept client %d\n", pending_client);
            }
        }

        cleanup_inactive_clients(clients, pool, &rpc_ctx);

        fd_set read_fds;
        FD_ZERO(&read_fds);
        int max_fd = -1;
        for (server_client_t *client = clients; client != NULL; client = client->next) {
            if (!client->state.disconnected && client->state.fifo_c2s_fd != -1) {
                FD_SET(client->state.fifo_c2s_fd, &read_fds);
                if (client->state.fifo_c2s_fd > max_fd) {
                    max_fd = client->state.fifo_c2s_fd;
                }
            }
        }

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int ready = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (ready == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("select");
            break;
        }
        if (ready == 0) {
            continue;
        }

        for (server_client_t *client = clients; client != NULL; client = client->next) {
            if (!client->state.disconnected && client->state.fifo_c2s_fd != -1 &&
                FD_ISSET(client->state.fifo_c2s_fd, &read_fds)) {
                read_client_requests(client, pool, &rpc_ctx);
            }
        }
    }

    for (server_client_t *client = clients; client != NULL; client = client->next) {
        close_client(client, pool, &rpc_ctx);
    }
    threadpool_destroy(pool);
    auth_destroy(rpc_ctx.auth);
    handles_destroy(rpc_ctx.handles);
    return 0;
}
