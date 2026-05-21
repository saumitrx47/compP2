/*
 * client_connection.h
 *
 * Client-side connection handling.
 * Manages SIGUSR1/SIGUSR2 signal exchange and FIFO opening
 * to establish bidirectional communication with the server.
 */

#ifndef CLIENT_CONNECTION_H
#define CLIENT_CONNECTION_H

#include <sys/types.h>

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
int send_connection_request(pid_t server_pid);

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
int open_client_fifos(pid_t client_pid, int *c2s_fd, int *s2c_fd);

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
void close_client_fifos(int c2s_fd, int s2c_fd);

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
                          size_t path_len);

#endif /* CLIENT_CONNECTION_H */
