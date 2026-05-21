/*
 * connection.h
 *
 * Signal handling and FIFO management for client-server communication.
 * Provides functions to handle SIGUSR1/SIGUSR2 signals, create FIFOs,
 * and manage the connection lifecycle.
 */

#ifndef CONNECTION_H
#define CONNECTION_H

#include "server_common.h"
#include <signal.h>
#include <sys/types.h>

/*
 * Initialize signal handlers for server (SIGUSR1 for new connections).
 * Must be called early in server startup before entering main event loop.
 * This sets up the signal masks for the main thread.
 *
 * Returns: 0 on success, -1 on error
 */
int server_init_signals(void);
pid_t server_take_pending_client(void);

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
int create_fifos(pid_t client_pid);

/*
 * Send SIGUSR2 back to a client to signal FIFOs are ready.
 * Called after FIFOs are created and server is ready to open them.
 *
 * Parameters:
 *   client_pid - PID of the client to signal
 *
 * Returns: 0 on success, -1 if kill() failed
 */
int signal_client_ready(pid_t client_pid);

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
int get_fifo_paths(pid_t client_pid, char *c2s_path, char *s2c_path);

/*
 * Unlink (delete) FIFOs for a disconnecting client.
 * Called when a client disconnects to clean up named pipes.
 *
 * Parameters:
 *   client_pid - PID of the disconnecting client
 *
 * Returns: 0 on success, -1 on error
 */
int unlink_fifos(pid_t client_pid);

#endif /* CONNECTION_H */
