/*
 * threadpool.h
 *
 * Fair fixed-size worker pool for per-client RPC requests.
 */

#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "server_common.h"
#include <stddef.h>

typedef struct threadpool threadpool_t;

threadpool_t *threadpool_create(size_t worker_count,
                                request_handler_fn handler,
                                void *user_data);
void threadpool_destroy(threadpool_t *pool);

int threadpool_register_client(threadpool_t *pool, client_state_t *client);
void threadpool_remove_client(threadpool_t *pool, client_state_t *client);

int threadpool_submit(threadpool_t *pool, client_state_t *client, char *rpc_line);
void request_destroy(request_t *request);

#endif /* THREADPOOL_H */
