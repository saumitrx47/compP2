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


int server_init_signals(void);
pid_t server_take_pending_client(void);

int create_fifos(pid_t client_pid);

int signal_client_ready(pid_t client_pid);

int get_fifo_paths(pid_t client_pid, char *c2s_path, char *s2c_path);

int unlink_fifos(pid_t client_pid);

#endif /* CONNECTION_H */
