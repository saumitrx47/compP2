/*
 * threadpool.c
 *
 * Implements a fixed-size worker pool with fair per-client scheduling.
 * Workers dequeue at most one request from a client before rotating to the
 * next active client, so one busy client cannot starve the others.
 */

#include "threadpool.h"
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct request_node {
    request_t *request;
    struct request_node *next;
} request_node_t;

typedef struct client_queue {
    client_state_t *client;
    request_node_t *head;
    request_node_t *tail;
    size_t queued_count;
    bool active;
    struct client_queue *prev;
    struct client_queue *next;
} client_queue_t;

struct threadpool {
    pthread_t *workers;
    size_t worker_count;
    request_handler_fn handler;
    void *user_data;

    pthread_mutex_t mutex;
    pthread_cond_t work_available;
    bool shutting_down;
    size_t pending_requests;

    client_queue_t *clients_head;
    client_queue_t *clients_tail;
    client_queue_t *next_client;
};

static void free_response(response_t *response) {
    free(response->response_str);
    response->response_str = NULL;
    response->ready = false;
    response->seq_num = 0;
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t total = 0;

    while (total < len) {
        ssize_t written = write(fd, buf + total, len - total);
        if (written == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            return -1;
        }
        total += (size_t)written;
    }

    return 0;
}

static void send_ordered_response(request_t *request, char *response_str) {
    client_state_t *client = request->client;
    uint64_t seq_num = request->request_seq_num;
    uint64_t slot = seq_num % MAX_PENDING_RESPONSES;

    pthread_mutex_lock(&client->output_mutex);

    if (client->disconnected || client->fifo_s2c_fd == -1) {
        free(response_str);
        pthread_mutex_unlock(&client->output_mutex);
        return;
    }

    if (seq_num < client->responses_sent ||
        seq_num >= client->responses_sent + MAX_PENDING_RESPONSES ||
        client->pending_responses[slot].ready) {
        free(response_str);
        pthread_mutex_unlock(&client->output_mutex);
        return;
    }

    client->pending_responses[slot].seq_num = seq_num;
    client->pending_responses[slot].response_str = response_str;
    client->pending_responses[slot].ready = true;

    while (true) {
        uint64_t next_slot = client->responses_sent % MAX_PENDING_RESPONSES;
        response_t *pending = &client->pending_responses[next_slot];

        if (!pending->ready || pending->seq_num != client->responses_sent) {
            break;
        }

        if (write_all(client->fifo_s2c_fd, pending->response_str,
                      strlen(pending->response_str)) == -1) {
            client->disconnected = true;
            free_response(pending);
            break;
        }

        free_response(pending);
        client->responses_sent++;
    }

    pthread_mutex_unlock(&client->output_mutex);
}

static client_queue_t *find_client_queue(threadpool_t *pool,
                                         client_state_t *client) {
    for (client_queue_t *queue = pool->clients_head; queue != NULL;
         queue = queue->next) {
        if (queue->client == client) {
            return queue;
        }
    }
    return NULL;
}

static void unlink_client_queue(threadpool_t *pool, client_queue_t *queue) {
    if (queue->prev != NULL) {
        queue->prev->next = queue->next;
    } else {
        pool->clients_head = queue->next;
    }

    if (queue->next != NULL) {
        queue->next->prev = queue->prev;
    } else {
        pool->clients_tail = queue->prev;
    }

    if (pool->next_client == queue) {
        pool->next_client = queue->next != NULL ? queue->next : pool->clients_head;
    }
}

static request_t *dequeue_fair_request(threadpool_t *pool) {
    if (pool->pending_requests == 0 || pool->clients_head == NULL) {
        return NULL;
    }

    client_queue_t *start = pool->next_client != NULL ? pool->next_client
                                                      : pool->clients_head;
    client_queue_t *queue = start;

    do {
        if (queue->active && queue->head != NULL) {
            request_node_t *node = queue->head;
            request_t *request = node->request;

            queue->head = node->next;
            if (queue->head == NULL) {
                queue->tail = NULL;
            }
            queue->queued_count--;
            pool->pending_requests--;
            pool->next_client = queue->next != NULL ? queue->next
                                                    : pool->clients_head;
            free(node);
            return request;
        }

        queue = queue->next != NULL ? queue->next : pool->clients_head;
    } while (queue != start);

    return NULL;
}

static void *worker_main(void *arg) {
    threadpool_t *pool = arg;

    while (true) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->shutting_down && pool->pending_requests == 0) {
            pthread_cond_wait(&pool->work_available, &pool->mutex);
        }

        if (pool->shutting_down && pool->pending_requests == 0) {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        request_t *request = dequeue_fair_request(pool);
        pthread_mutex_unlock(&pool->mutex);

        if (request == NULL) {
            continue;
        }

        char *response = pool->handler(request, pool->user_data);
        if (response == NULL) {
            response = strdup("-1\n");
        }
        if (response != NULL) {
            send_ordered_response(request, response);
        }
        request_destroy(request);
    }

    return NULL;
}

threadpool_t *threadpool_create(size_t worker_count,
                                request_handler_fn handler,
                                void *user_data) {
    if (worker_count == 0 || handler == NULL) {
        errno = EINVAL;
        return NULL;
    }

    threadpool_t *pool = calloc(1, sizeof(*pool));
    if (pool == NULL) {
        return NULL;
    }

    pool->workers = calloc(worker_count, sizeof(*pool->workers));
    if (pool->workers == NULL) {
        free(pool);
        return NULL;
    }

    pool->worker_count = worker_count;
    pool->handler = handler;
    pool->user_data = user_data;

    if (pthread_mutex_init(&pool->mutex, NULL) != 0) {
        free(pool->workers);
        free(pool);
        return NULL;
    }
    if (pthread_cond_init(&pool->work_available, NULL) != 0) {
        pthread_mutex_destroy(&pool->mutex);
        free(pool->workers);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < worker_count; i++) {
        if (pthread_create(&pool->workers[i], NULL, worker_main, pool) != 0) {
            pthread_mutex_lock(&pool->mutex);
            pool->shutting_down = true;
            pthread_cond_broadcast(&pool->work_available);
            pthread_mutex_unlock(&pool->mutex);

            for (size_t j = 0; j < i; j++) {
                pthread_join(pool->workers[j], NULL);
            }
            pthread_cond_destroy(&pool->work_available);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->workers);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

void request_destroy(request_t *request) {
    if (request == NULL) {
        return;
    }
    free(request->rpc_line);
    free(request);
}

int threadpool_register_client(threadpool_t *pool, client_state_t *client) {
    if (pool == NULL || client == NULL) {
        errno = EINVAL;
        return -1;
    }

    client_queue_t *queue = calloc(1, sizeof(*queue));
    if (queue == NULL) {
        return -1;
    }
    queue->client = client;
    queue->active = true;

    pthread_mutex_lock(&pool->mutex);
    if (find_client_queue(pool, client) != NULL) {
        pthread_mutex_unlock(&pool->mutex);
        free(queue);
        errno = EEXIST;
        return -1;
    }

    queue->prev = pool->clients_tail;
    if (pool->clients_tail != NULL) {
        pool->clients_tail->next = queue;
    } else {
        pool->clients_head = queue;
    }
    pool->clients_tail = queue;
    if (pool->next_client == NULL) {
        pool->next_client = queue;
    }
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

void threadpool_remove_client(threadpool_t *pool, client_state_t *client) {
    if (pool == NULL || client == NULL) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);
    client_queue_t *queue = find_client_queue(pool, client);
    if (queue == NULL) {
        pthread_mutex_unlock(&pool->mutex);
        return;
    }

    unlink_client_queue(pool, queue);

    request_node_t *node = queue->head;
    while (node != NULL) {
        request_node_t *next = node->next;
        request_destroy(node->request);
        free(node);
        pool->pending_requests--;
        node = next;
    }
    pthread_mutex_unlock(&pool->mutex);

    free(queue);
}

int threadpool_submit(threadpool_t *pool, client_state_t *client, char *rpc_line) {
    if (pool == NULL || client == NULL || rpc_line == NULL) {
        errno = EINVAL;
        return -1;
    }

    request_t *request = calloc(1, sizeof(*request));
    if (request == NULL) {
        return -1;
    }
    request->client = client;
    request->rpc_line = rpc_line;

    request_node_t *node = calloc(1, sizeof(*node));
    if (node == NULL) {
        free(request);
        return -1;
    }
    node->request = request;

    pthread_mutex_lock(&pool->mutex);
    if (pool->shutting_down) {
        pthread_mutex_unlock(&pool->mutex);
        free(node);
        free(request);
        errno = ESHUTDOWN;
        return -1;
    }

    client_queue_t *queue = find_client_queue(pool, client);
    if (queue == NULL || !queue->active || client->disconnected) {
        pthread_mutex_unlock(&pool->mutex);
        free(node);
        free(request);
        errno = ENOTCONN;
        return -1;
    }

    request->request_seq_num = client->requests_received++;
    if (queue->tail != NULL) {
        queue->tail->next = node;
    } else {
        queue->head = node;
    }
    queue->tail = node;
    queue->queued_count++;
    pool->pending_requests++;

    pthread_cond_signal(&pool->work_available);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

void threadpool_destroy(threadpool_t *pool) {
    if (pool == NULL) {
        return;
    }

    pthread_mutex_lock(&pool->mutex);
    pool->shutting_down = true;
    pthread_cond_broadcast(&pool->work_available);
    pthread_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    pthread_mutex_lock(&pool->mutex);
    client_queue_t *queue = pool->clients_head;
    while (queue != NULL) {
        client_queue_t *next_queue = queue->next;
        request_node_t *node = queue->head;
        while (node != NULL) {
            request_node_t *next_node = node->next;
            request_destroy(node->request);
            free(node);
            node = next_node;
        }
        free(queue);
        queue = next_queue;
    }
    pthread_mutex_unlock(&pool->mutex);

    pthread_cond_destroy(&pool->work_available);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->workers);
    free(pool);
}
