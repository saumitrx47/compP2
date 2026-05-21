/*
 * client_main.c
 *
 * Client for the PointerPro Animate service.
 */

#include "client_connection.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_INPUT_LEN 4096
#define MAX_RESPONSE_LEN 4096
#define MAX_USERNAME_LEN 32

static void usage(const char *program_name) {
    fprintf(stderr, "Usage: %s <server_pid>\n", program_name);
    exit(1);
}

static int parse_arguments(int argc, char *argv[], pid_t *server_pid) {
    char *endptr = NULL;
    long pid;

    if (argc != 2) {
        return -1;
    }
    errno = 0;
    pid = strtol(argv[1], &endptr, 10);
    if (errno != 0 || endptr == argv[1] || *endptr != '\0' || pid < 1) {
        return -1;
    }
    *server_pid = (pid_t)pid;
    return 0;
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

static int read_response_line(int fd, char *buffer, size_t len) {
    size_t used = 0;

    while (used + 1 < len) {
        char ch;
        ssize_t n = read(fd, &ch, 1);
        if (n == 1) {
            buffer[used++] = ch;
            if (ch == '\n') {
                buffer[used] = '\0';
                return 0;
            }
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }
    buffer[used] = '\0';
    return used > 0 ? 0 : -1;
}

static void strip_newline(char *s) {
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
        s[--len] = '\0';
    }
}

static char *trim_whitespace(char *s) {
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) {
        *--end = '\0';
    }
    return s;
}

static bool first_token(const char *line, char *token, size_t token_len) {
    size_t i = 0;
    size_t j = 0;

    while (line[i] == ' ' || line[i] == '\t') {
        i++;
    }
    while (line[i] != '\0' && line[i] != '\n' && line[i] != '\r' &&
           line[i] != ' ' && line[i] != '\t') {
        if (j + 1 >= token_len) {
            return false;
        }
        token[j++] = line[i++];
    }
    token[j] = '\0';
    return j > 0;
}

static bool login_username(const char *line, char *username, size_t username_len) {
    char copy[MAX_INPUT_LEN];
    char *save = NULL;
    char *tok;
    char *name;
    char *extra;

    strncpy(copy, line, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    tok = strtok_r(copy, " \t\r\n", &save);
    name = strtok_r(NULL, " \t\r\n", &save);
    extra = strtok_r(NULL, " \t\r\n", &save);
    if (tok == NULL || strcmp(tok, "Login") != 0 || name == NULL || extra != NULL) {
        return false;
    }
    if (strlen(name) >= username_len) {
        return false;
    }
    strcpy(username, name);
    return true;
}

static bool parse_integer_reply(const char *response, long *value) {
    char copy[MAX_RESPONSE_LEN];
    char *end = NULL;

    strncpy(copy, response, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    strip_newline(copy);

    errno = 0;
    *value = strtol(copy, &end, 10);
    while (*end == ' ' || *end == '\t') {
        end++;
    }
    return errno == 0 && end != copy && *end == '\0';
}

static void display_rpc_response(const char *response) {
    char copy[MAX_RESPONSE_LEN];
    char *save = NULL;
    char *token = NULL;

    strncpy(copy, response, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = '\0';
    strip_newline(copy);
    token = trim_whitespace(copy);

    if (*token == '\0') {
        printf("%s\n", response);
        fflush(stdout);
        return;
    }

    /* Map negative error codes to human-readable strings */
    if (strcmp(token, "-1") == 0) { puts("RPC Failed");     fflush(stdout); return; }
    if (strcmp(token, "-2") == 0) { puts("Value error");    fflush(stdout); return; }
    if (strcmp(token, "-3") == 0) { puts("Internal error"); fflush(stdout); return; }

    /* Success path: first token must be "0" */
    token = strtok_r(token, " \t", &save);
    if (token == NULL || strcmp(token, "0") != 0) {
        printf("%s\n", copy);
        fflush(stdout);
        return;
    }

    char *t1 = strtok_r(NULL, " \t", &save);
    if (t1 == NULL) { puts("Success"); fflush(stdout); return; }

    if (strcmp(t1, "-1") == 0) { puts("Data write failed"); fflush(stdout); return; }

    if (strcmp(t1, "0") == 0) {
        char *t2 = strtok_r(NULL, " \t", &save);
        puts(t2 == NULL || strcmp(t2, "0") == 0 ? "Success" : "Movie write failed");
        fflush(stdout);
        return;
    }

    printf("Success %s\n", t1);
    fflush(stdout);
}

int main(int argc, char *argv[]) {
    pid_t server_pid = 0;
    pid_t client_pid = getpid();
    int c2s_fd = -1;
    int s2c_fd = -1;
    bool logged_in = false;
    bool sent_disconnect = false;
    char current_username[MAX_USERNAME_LEN + 1] = {0};
    char input_buffer[MAX_INPUT_LEN];
    char response_buffer[MAX_RESPONSE_LEN];

    if (parse_arguments(argc, argv, &server_pid) == -1) {
        usage(argv[0]);
    }

    if (send_connection_request(server_pid) == -1) {
        fprintf(stderr, "Error: Failed to connect to server\n");
        return 1;
    }
    if (open_client_fifos(client_pid, &c2s_fd, &s2c_fd) == -1) {
        fprintf(stderr, "Error: Failed to open FIFOs\n");
        return 1;
    }

    while (fgets(input_buffer, sizeof(input_buffer), stdin) != NULL) {
        char command[64];
        char login_name[MAX_USERNAME_LEN + 1];
        size_t len = strlen(input_buffer);

        fprintf(stderr, "[DEBUG] Read from stdin: %s", input_buffer);

        if (len == 0) {
            continue;
        }
        if (input_buffer[len - 1] != '\n') {
            if (len + 1 >= sizeof(input_buffer)) {
                puts("Value error");
                int ch;
                while ((ch = getchar()) != '\n' && ch != EOF) {
                }
                continue;
            }
            input_buffer[len++] = '\n';
            input_buffer[len] = '\0';
        }

        if (!first_token(input_buffer, command, sizeof(command))) {
            continue;
        }

        if (!logged_in && strcmp(command, "Login") != 0 &&
            strcmp(command, "Disconnect") != 0) {
            puts("Not logged in");
            continue;
        }

        login_name[0] = '\0';
        if (strcmp(command, "Login") == 0) {
            login_username(input_buffer, login_name, sizeof(login_name));
        }

        if (write_all(c2s_fd, input_buffer, strlen(input_buffer)) == -1) {
            break;
        }
        if (strcmp(command, "Disconnect") == 0) {
            sent_disconnect = true;
            break;
        }
        fprintf(stderr, "[DEBUG] About to read response...\n");
        if (read_response_line(s2c_fd, response_buffer, sizeof(response_buffer)) == -1) {
            fprintf(stderr, "[DEBUG] read_response_line failed!\n");
            break;
        }
        fprintf(stderr, "[DEBUG] Read response: '%s'\n", response_buffer);

        if (strcmp(command, "Login") == 0) {
            char display[MAX_RESPONSE_LEN];
            long balance;

            strncpy(display, response_buffer, sizeof(display) - 1);
            display[sizeof(display) - 1] = '\0';
            strip_newline(display);

            if (strncmp(display, "Reject", 6) == 0) {
                puts(display);
                fflush(stdout);
                /* Rejected logins should exit without a cleanup Disconnect round-trip. */
                sent_disconnect = true;
                break;
            }
            if (parse_integer_reply(response_buffer, &balance)) {
                /* Successful Login replies are balances; use the original username. */
                logged_in = true;
                strncpy(current_username, login_name, MAX_USERNAME_LEN);
                current_username[MAX_USERNAME_LEN] = '\0';
                printf("Welcome %s. Your balance is %ld\n", current_username, balance);
                fflush(stdout);
            } else {
                puts(display);
                fflush(stdout);
            }
        } else {
            fprintf(stderr, "[DEBUG] command='%s', response_buffer='%s'\n", command, response_buffer);
            display_rpc_response(response_buffer);
        }
    }

    if (!sent_disconnect && c2s_fd != -1 && s2c_fd != -1) {
        const char disconnect_msg[] = "Disconnect\n";
        if (write_all(c2s_fd, disconnect_msg, sizeof(disconnect_msg) - 1) == 0) {
            (void)read_response_line(s2c_fd, response_buffer, sizeof(response_buffer));
        }
    }

    close_client_fifos(c2s_fd, s2c_fd);
    return 0;
}
