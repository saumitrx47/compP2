/*
 * client_connection.c
 *
 * Implementation of client-side connection handling.
 * Manages SIGUSR1/SIGUSR2 signal exchange and FIFO opening.
 */

#include "client_connection.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define MAX_FIFO_NAME_LEN 256

/* Global flag set by SIGUSR2 handler */
volatile sig_atomic_t sigusr2_received = 0;

/*
 * Signal handler for SIGUSR2 (server ready signal).
 * Sets a flag indicating the server has prepared FIFOs.
 *
 * Parameters:
 *   sig - Signal number (SIGUSR2)
 *
 * Returns: (void signal handler)
 */
static void sigusr2_handler(int sig) {
    (void)sig;  /* Avoid unused parameter warning */
    sigusr2_received = 1;
}

/*
 * Generate FIFO path names for this client.
 * Used by both server and client to refer to the same FIFO names.
 *
 * Parameters:
 *   client_pid - PID of this client
 *   c2s_path - Buffer to store C2S FIFO path
 *   s2c_path - Buffer to store S2C FIFO path
 *   path_len - Maximum length of path buffers
 *
 * Returns: 0 on success, -1 if paths too long
 */
int get_client_fifo_paths(pid_t client_pid, char *c2s_path, char *s2c_path,
                          size_t path_len) {
    const char *fifo_dir = getenv("ANIMATE_FIFO_DIR");
    const char *prefix = (fifo_dir != NULL && fifo_dir[0] != '\0') ? fifo_dir : ".";
    int ret_c2s = snprintf(c2s_path, path_len, "%s/FIFO_C2S_%d", prefix, client_pid);
    int ret_s2c = snprintf(s2c_path, path_len, "%s/FIFO_S2C_%d", prefix, client_pid);
    
    if (ret_c2s < 0 || (size_t)ret_c2s >= path_len ||
        ret_s2c < 0 || (size_t)ret_s2c >= path_len) {
        return -1;
    }
    
    return 0;
}

/*
 * Send SIGUSR1 to server and wait for SIGUSR2 response.
 * Blocks until SIGUSR2 is received or timeout (~1 second) occurs.
 * Must be called before calling open_client_fifos().
 *
 * Parameters:
 *   server_pid - PID of the server to contact
 *
 * Returns: 0 on success (SIGUSR2 received), -1 on timeout or error
 */
int send_connection_request(pid_t server_pid) {
    struct sigaction sa;
    
    /* Set up SIGUSR2 handler */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr2_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    
    if (sigaction(SIGUSR2, &sa, NULL) == -1) {
        perror("sigaction");
        return -1;
    }
    
    /* Send SIGUSR1 to server */
    if (kill(server_pid, SIGUSR1) == -1) {
        perror("kill");
        return -1;
    }
    
    /* Wait approximately 1 second for SIGUSR2 response */
    for (int i = 0; i < 10; i++) {
        if (sigusr2_received) {
            return 0;
        }
        usleep(100000);  /* 100ms sleep */
    }
    
    fprintf(stderr, "Error: Timeout waiting for SIGUSR2 from server\n");
    return -1;
}

/*
 * Open client FIFOs after SIGUSR2 has been received.
 * Opens FIFO_C2S_<client_pid> for writing and FIFO_S2C_<client_pid> for reading.
 * Must be called after send_connection_request() returns successfully.
 *
 * Parameters:
 *   client_pid - PID of this client process
 *   c2s_fd - Pointer to store file descriptor for C2S FIFO
 *   s2c_fd - Pointer to store file descriptor for S2C FIFO
 *
 * Returns: 0 on success, -1 on error (open failed)
 */
int open_client_fifos(pid_t client_pid, int *c2s_fd, int *s2c_fd) {
    char c2s_path[MAX_FIFO_NAME_LEN];
    char s2c_path[MAX_FIFO_NAME_LEN];
    
    if (get_client_fifo_paths(client_pid, c2s_path, s2c_path, MAX_FIFO_NAME_LEN) == -1) {
        fprintf(stderr, "Error: Failed to generate FIFO paths\n");
        return -1;
    }
    
    /* Open C2S FIFO for writing */
    *c2s_fd = open(c2s_path, O_WRONLY);
    if (*c2s_fd == -1) {
        perror("open (C2S FIFO)");
        return -1;
    }
    
    /* Open S2C FIFO for reading */
    *s2c_fd = open(s2c_path, O_RDONLY);
    if (*s2c_fd == -1) {
        perror("open (S2C FIFO)");
        close(*c2s_fd);
        return -1;
    }
    
    return 0;
}

/*
 * Close client FIFOs and clean up resources.
 * Called on disconnect or error recovery.
 *
 * Parameters:
 *   c2s_fd - File descriptor for C2S FIFO (may be -1 if not open)
 *   s2c_fd - File descriptor for S2C FIFO (may be -1 if not open)
 *
 * Returns: (void)
 */
void close_client_fifos(int c2s_fd, int s2c_fd) {
    if (c2s_fd != -1) {
        close(c2s_fd);
    }
    if (s2c_fd != -1) {
        close(s2c_fd);
    }
}
