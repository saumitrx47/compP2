#ifndef RPC_H
#define RPC_H

#include "auth.h"
#include "handles.h"
#include "server_common.h"

typedef struct rpc_context {
    auth_db_t *auth;
    handle_store_t *handles;
} rpc_context_t;

char *rpc_handle_request(request_t *request, void *user_data);
void rpc_client_disconnected(rpc_context_t *ctx, client_state_t *client);

#endif
