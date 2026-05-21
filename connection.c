/*
 * connection.c
 *
 * Implementation of signal handling and FIFO management.
 * Handles client connection protocol via SIGUSR1/SIGUSR2 signals
 * and creates/manages named pipes for bidirectional communication.
 */

#include "connection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

/* Pending client PID captured from SIGUSR1. */
static volatile sig_atomic_t pending_client_pid = 0;

/*
 * Signal handler for SIGUSR1 (new client connection request).
 * Sets a flag indicating a connection request has been received.
 * The main event loop will check this flag and process the connection.
 *
 * Parameters:
 *   sig     - Signal number (SIGUSR1)
 *   info    - Signal info containing sender PID
 *   context - Unused
 *
 * Returns: (void signal handler)
 */
static void sigusr1_handler(int sig, siginfo_t *info, void *context) {
    (void)sig;
    (void)context;
    if (info != NULL) {
        pending_client_pid = info->si_pid;
        fprintf(stderr, "[DEBUG] SIGUSR1 received from PID %d\n", (int)info->si_pid);
    }
}

/*
 * Initialize signal handlers for server (SIGUSR1 for new connections).
 * Must be called early in server startup before entering main event loop.
 * This sets up the signal masks for the main thread.
 *
 * Returns: 0 on success, -1 on error
 */
int server_init_signals(void) {
    struct sigaction sa;

    /* Set up SIGUSR1 handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO;

    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        perror("sigaction");
        return -1;
    }

    return 0;
}

pid_t server_take_pending_client(void) {
    pid_t client_pid = (pid_t)pending_client_pid;
    pending_client_pid = 0;
    return client_pid;
}

/*
 * Generate FIFO path names for a given client PID.
 * Stores the generated paths in the provided buffers.
 *
 * Parameters:
 *   client_pid - PID of the client
 *   c2s_path   - Buffer to store C2S FIFO path (must be MAX_FIFO_NAME_LEN bytes)
 *   s2c_path   - Buffer to store S2C FIFO path (must be MAX_FIFO_NAME_LEN bytes)
 *
 * Returns: 0 on success, -1 if buffer space insufficient
 */
int get_fifo_paths(pid_t client_pid, char *c2s_path, char *s2c_path) {
    const char *fifo_dir = getenv("ANIMATE_FIFO_DIR");
    const char *prefix = (fifo_dir != NULL && fifo_dir[0] != '\0') ? fifo_dir : ".";
    int ret_c2s = snprintf(c2s_path, MAX_FIFO_NAME_LEN, "%s/FIFO_C2S_%d",
                           prefix, client_pid);
    int ret_s2c = snprintf(s2c_path, MAX_FIFO_NAME_LEN, "%s/FIFO_S2C_%d",
                           prefix, client_pid);

    if (ret_c2s < 0 || ret_c2s >= MAX_FIFO_NAME_LEN ||
        ret_s2c < 0 || ret_s2c >= MAX_FIFO_NAME_LEN) {
        return -1;
    }

    return 0;
}

/*
 * Create FIFOs for a new client connection.
 * On receiving SIGUSR1 from client_pid, creates:
 *   - FIFO_C2S_<pid> (client-to-server, read mode)
 *   - FIFO_S2C_<pid> (server-to-client, write mode)
 * If FIFOs already exist, they are unlinked and recreated.
 *
 * Parameters:
 *   client_pid - PID of the connecting client
 *
 * Returns: 0 on success, -1 on error (e.g., mkfifo failed)
 */
int create_fifos(pid_t client_pid) {
    char c2s_path[MAX_FIFO_NAME_LEN];
    char s2c_path[MAX_FIFO_NAME_LEN];

    if (get_fifo_paths(client_pid, c2s_path, s2c_path) == -1) {
        fprintf(stderr, "Failed to generate FIFO paths\n");
        return -1;
    }

    /* Unlink existing FIFOs (ignore errors if they don't exist) */
    unlink(c2s_path);
    unlink(s2c_path);

    /* Create C2S FIFO */
    if (mkfifo(c2s_path, 0666) == -1) {
        perror("mkfifo (C2S)");
        return -1;
    }

    /* Create S2C FIFO */
    if (mkfifo(s2c_path, 0666) == -1) {
        perror("mkfifo (S2C)");
        unlink(c2s_path); /* Clean up C2S on failure */
        return -1;
    }

    return 0;
}

/*
 * Send SIGUSR2 back to a client to signal FIFOs are ready.
 * Called after FIFOs are created and server is ready to open them.
 *
 * Parameters:
 *   client_pid - PID of the client to signal
 *
 * Returns: 0 on success, -1 if kill() failed
 */
int signal_client_ready(pid_t client_pid) {
    fprintf(stderr, "[DEBUG] Sending SIGUSR2 to client PID %d\n", (int)client_pid);
    if (kill(client_pid, SIGUSR2) == -1) {
        perror("kill");
        return -1;
    }

    return 0;
}

/*
 * Unlink (delete) FIFOs for a disconnecting client.
 * Called when a client disconnects to clean up named pipes.
 *
 * Parameters:
 *   client_pid - PID of the disconnecting client
 *
 * Returns: 0 on success, -1 on error
 */
int unlink_fifos(pid_t client_pid) {
    char c2s_path[MAX_FIFO_NAME_LEN];
    char s2c_path[MAX_FIFO_NAME_LEN];

    if (get_fifo_paths(client_pid, c2s_path, s2c_path) == -1) {
        return -1;
    }

    unlink(c2s_path);
    unlink(s2c_path);

    return 0;
}
