/*
 * server_common.h
 *
 * Shared data structures, types, and constants for the animate server.
 * This header defines the core structures used across all server modules
 * for managing client state, handles, requests, and responses.
 */

#ifndef SERVER_COMMON_H
#define SERVER_COMMON_H

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <pthread.h>
#include <time.h>

/* Maximum username length (alphanumeric, max 32 chars per spec) */
#define MAX_USERNAME_LEN 32

/* Maximum FIFO name length */
#define MAX_FIFO_NAME_LEN 256

/* Reserved invalid handle ID */
#define INVALID_HANDLE 0

/* Maximum pending responses per client */
#define MAX_PENDING_RESPONSES 1024

/* Barrier timeout in seconds (for detecting dead clients) */
#define BARRIER_TIMEOUT_SEC 30

/*
 * Pending response structure.
 * Tracks a response that is complete but not yet sent (waiting for
 * previous responses to be sent first, to maintain per-client ordering).
 */
typedef struct {
    uint64_t seq_num;
    char *response_str;
    bool ready;
} response_t;

/*
 * Per-client state structure.
 * Tracks the state of a connected client including authentication,
 * resources, and communication endpoints.
 */
typedef struct {
    pid_t client_pid;
    char username[MAX_USERNAME_LEN + 1];
    int32_t balance;
    bool logged_in;
    
    /* File descriptors for FIFOs */
    int fifo_c2s_fd;  /* Client-to-Server (read) */
    int fifo_s2c_fd;  /* Server-to-Client (write) */
    
    /* Request/response sequencing */
    uint64_t requests_received;   /* Total requests from this client */
    uint64_t responses_sent;      /* Total responses sent to this client */
    
    /* Synchronization for response ordering */
    pthread_mutex_t output_mutex;
    response_t pending_responses[MAX_PENDING_RESPONSES];
    
    /* Client status */
    bool disconnected;
    time_t last_activity;
    time_t disconnect_after;
    
    /* TODO: Handle tables and shared canvas tracking */
} client_state_t;

/*
 * Request work item.
 * Represents a single RPC request queued for processing by a worker thread.
 */
typedef struct {
    client_state_t *client;
    uint64_t request_seq_num;
    char *rpc_line;  /* Full RPC command string */
} request_t;

typedef char *(*request_handler_fn)(request_t *request, void *user_data);

#endif /* SERVER_COMMON_H */
