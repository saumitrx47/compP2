#include "rpc.h"
#include <animate/animate.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <bits/posix1_lim.h>

typedef enum {
    RPC_FAIL = -1,
    RPC_VALUE = -2,
    RPC_INTERNAL = -3
} rpc_error_t;

// These functions return a malloc'd string containing the response, or NULL on allocation failure. The caller is responsible for freeing the result.
static char *xstrdup(const char *s) {
    char *copy = malloc(strlen(s) + 1);
    if (copy != NULL) {
        strcpy(copy, s);
    }
    return copy;
}

// These functions also print the response to stdout as a side effect, since the main thread is reading responses from there.
static char *response_code(int code) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d\n", code);
    char *result = xstrdup(buf);
    if (result != NULL) {
        printf("%s", result);
        fflush(stdout);
    }
    return result;
}

/* For numeric responses, we prefix with "0 " to indicate success, followed by the value. 
This is just a convention to make it easier to parse responses in tests. */
static char *response_u64(uint64_t value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "0 %llu\n", (unsigned long long)value);
    char *result = xstrdup(buf);
    if (result != NULL) {
        printf("%s", result);
        fflush(stdout);
    }
    return result;
}

// For signed 32-bit integer responses, we prefix with "0 " to indicate success, followed by the value.
static char *response_i32(int32_t value) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d\n", value);
    char *result = xstrdup(buf);
    if (result != NULL) {
        printf("%s", result);
        fflush(stdout);
    }
    return result;
}

// For generating custom responses, we prefix with "0 " to indicate success, followed by the values.
static char *response_generate(int dat, int movie) {
    char buf[64];
    snprintf(buf, sizeof(buf), "0 %d %d\n", dat, movie);
    char *result = xstrdup(buf);
    if (result != NULL) {
        printf("%s", result);
        fflush(stdout);
    }
    return result;
}

static size_t tokenize(char *line, char **tokens, size_t max_tokens) {
    char *save = NULL;
    char *tok;
    size_t count = 0;

    for (tok = strtok_r(line, " \t\r\n", &save);
         tok != NULL && count < max_tokens;
         tok = strtok_r(NULL, " \t\r\n", &save)) {
        tokens[count++] = tok;
    }
    if (tok != NULL) {
        return max_tokens + 1;
    }
    return count;
}

// These parsing functions return true on success and false on failure. On success, they store the parsed value in *out.
static bool parse_u64(const char *s, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;

    if (s == NULL || s[0] == '-') {
        return false;
    }
    errno = 0;
    value = strtoull(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0') {
        return false;
    }
    *out = (uint64_t)value;
    return true;
}

static bool parse_size(const char *s, size_t *out) {
    uint64_t value;
    if (!parse_u64(s, &value) || value > SIZE_MAX) {
        return false;
    }
    *out = (size_t)value;
    return true;
}

static bool parse_ssize(const char *s, ssize_t *out) {
    char *end = NULL;
    long long value;

    errno = 0;
    value = strtoll(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' ||
        value < (-(long long)SSIZE_MAX - 1) || value > (long long)SSIZE_MAX) {
        return false;
    }
    *out = (ssize_t)value;
    return true;
}

static bool parse_color(const char *s, color_t *out) {
    uint64_t value;
    if (!parse_u64(s, &value) || value > UINT32_MAX) {
        return false;
    }
    *out = (color_t)value;
    return true;
}

static bool parse_bool_token(const char *s, bool *out) {
    uint64_t value;
    if (!parse_u64(s, &value) || value > 1) {
        return false;
    }
    *out = value != 0;
    return true;
}

static bool valid_filename_base(const char *s) {
    if (s == NULL || s[0] == '\0') {
        return false;
    }
    for (size_t i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '/' || c == '\\' || c == ';' || c == '&' || c == '|' ||
            c == '<' || c == '>' || c == '`' || c == '$') {
            return false;
        }
    }
    return true;
}

static char *with_suffix(const char *base, const char *suffix) {
    size_t a = strlen(base);
    size_t b = strlen(suffix);
    char *out = malloc(a + b + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, base, a);
    memcpy(out + a, suffix, b + 1);
    return out;
}

/* This function runs ffmpeg to convert a raw video file to mp4 format. It returns 0 on success and -1 on failure. 
The ffmpeg output is written to the specified log file. */
static int run_ffmpeg(const char *dat, const char *mp4, const char *log,
                      size_t width, size_t height, size_t frame_rate) {
    char size_arg[64];
    char rate_arg[32];
    int log_fd;
    pid_t pid;
    int status;

    snprintf(size_arg, sizeof(size_arg), "%zux%zu", width, height);
    snprintf(rate_arg, sizeof(rate_arg), "%zu", frame_rate);

    log_fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (log_fd == -1) {
        return -1;
    }

    pid = fork();
    if (pid == -1) {
        close(log_fd);
        return -1;
    }
    if (pid == 0) {
        dup2(log_fd, STDOUT_FILENO);
        dup2(log_fd, STDERR_FILENO);
        close(log_fd);
        execlp("ffmpeg", "ffmpeg", "-y", "-f", "rawvideo",
               "-pixel_format", "argb", "-video_size", size_arg,
               "-framerate", rate_arg, "-i", dat, mp4, (char *)NULL);
        _exit(127);
    }

    close(log_fd);
    do {
        if (waitpid(pid, &status, 0) == -1) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        break;
    } while (true);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return -1;
    }
    return 0;
}

/* This function handles the "generate" RPC command, which generates a movie from a range of frames of a canvas. 
It returns a response string indicating success or failure, and also prints the response to stdout. */
static char *handle_generate(rpc_context_t *ctx, client_state_t *client,
                             char **tokens, size_t ntokens) {
    uint64_t canvas_id;
    size_t start, end, frame_rate;
    canvas_record_t *canvas;
    char *dat = NULL;
    char *mp4 = NULL;
    char *log = NULL;
    FILE *file = NULL;
    void *buf = NULL;
    size_t frame_size;
    int movie_status;

    if (ntokens != 6) {
        return response_code(RPC_FAIL);
    }
    if (!parse_u64(tokens[1], &canvas_id) || !valid_filename_base(tokens[2]) ||
        !parse_size(tokens[3], &start) || !parse_size(tokens[4], &end) ||
        !parse_size(tokens[5], &frame_rate) || start > end || frame_rate == 0) {
        return response_code(RPC_VALUE);
    }

    pthread_mutex_lock(handles_mutex(ctx->handles));
    canvas = handles_get_canvas(ctx->handles, canvas_id, client->username);
    if (canvas == NULL) {
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        return response_code(RPC_VALUE);
    }
    canvas_record_wait_for_barrier(ctx->handles, canvas, client->username);
    frame_size = animate_frame_size_bytes(canvas_record_ptr(canvas));
    pthread_mutex_unlock(handles_mutex(ctx->handles));

    dat = with_suffix(tokens[2], ".dat");
    mp4 = with_suffix(tokens[2], ".mp4");
    log = with_suffix(tokens[2], ".log");
    buf = malloc(frame_size);
    if (dat == NULL || mp4 == NULL || log == NULL || buf == NULL) {
        free(dat); free(mp4); free(log); free(buf);
        return response_code(RPC_INTERNAL);
    }

    file = fopen(dat, "wb");
    if (file == NULL) {
        free(dat); free(mp4); free(log); free(buf);
        return response_generate(-1, 0);
    }

    for (size_t frame = start; frame <= end; frame++) {
        pthread_mutex_lock(handles_mutex(ctx->handles));
        canvas = handles_get_canvas(ctx->handles, canvas_id, client->username);
        if (canvas == NULL) {
            pthread_mutex_unlock(handles_mutex(ctx->handles));
            fclose(file);
            free(dat); free(mp4); free(log); free(buf);
            return response_code(RPC_VALUE);
        }
        animate_generate_frame(canvas_record_ptr(canvas), frame, frame_rate, buf);
        pthread_mutex_unlock(handles_mutex(ctx->handles));

        if (fwrite(buf, 1, frame_size, file) != frame_size) {
            fclose(file);
            free(dat); free(mp4); free(log); free(buf);
            return response_generate(-1, 0);
        }
        if (frame == SIZE_MAX) {
            break;
        }
    }
    if (fclose(file) != 0) {
        free(dat); free(mp4); free(log); free(buf);
        return response_generate(-1, 0);
    }

    movie_status = run_ffmpeg(dat, mp4, log, canvas_record_width(canvas),
                              canvas_record_height(canvas), frame_rate);
    free(dat); free(mp4); free(log); free(buf);
    if (movie_status == -1) {
        return response_generate(0, -1);
    }
    return response_generate(0, 0);
}

static char *require_login(client_state_t *client) {
    if (!client->logged_in) {
        return response_code(RPC_VALUE);
    }
    return NULL;
}

// If the canvas is currently at a barrier for this user, wait until they have passed the barrier and can safely interact with the canvas again.
static void wait_canvas_barrier_if_needed(rpc_context_t *ctx,
                                          canvas_record_t *canvas,
                                          client_state_t *client) {
    canvas_record_wait_for_barrier(ctx->handles, canvas, client->username);
}

// This function can be called when a client disconnects unexpectedly, or when they explicitly request disconnection. 
// It performs necessary cleanup of server-side state related to the client.
char *rpc_handle_request(request_t *request, void *user_data) {
    rpc_context_t *ctx = user_data;
    client_state_t *client = request->client;
    char *line;
    char *tokens[16];
    size_t ntokens;
    char *login_error;

    if (ctx == NULL || request == NULL || request->rpc_line == NULL) {
        return response_code(RPC_INTERNAL);
    }

    line = xstrdup(request->rpc_line);
    if (line == NULL) {
        return response_code(RPC_INTERNAL);
    }
    ntokens = tokenize(line, tokens, 16);
    if (ntokens == 0 || ntokens > 16) {
        free(line);
        return response_code(RPC_FAIL);
    }

    if (strcmp(tokens[0], "Login") == 0) {
        int32_t balance;
        if (ntokens != 2 || !auth_valid_username(tokens[1])) {
            free(line);
            return response_code(RPC_FAIL);
        }
        if (!auth_lookup(ctx->auth, tokens[1], &balance)) {
            client->disconnect_after = time(NULL) + 1;
            free(line);
            printf("Reject UNAUTHORISED\n");
            fflush(stdout);
            return xstrdup("Reject UNAUTHORISED\n");
        }
        if (balance <= 0) {
            client->disconnect_after = time(NULL) + 1;
            free(line);
            printf("Reject BALANCE\n");
            fflush(stdout);
            return xstrdup("Reject BALANCE\n");
        }
        strncpy(client->username, tokens[1], MAX_USERNAME_LEN);
        client->username[MAX_USERNAME_LEN] = '\0';
        client->balance = balance;
        client->logged_in = true;
        free(line);
        return response_i32(balance);
    }

    if (strcmp(tokens[0], "Disconnect") == 0) {
        rpc_client_disconnected(ctx, client);
        free(line);
        printf("0\n");
        fflush(stdout);
        return xstrdup("0\n");
    }

    login_error = require_login(client);
    if (login_error != NULL) {
        free(line);
        return login_error;
    }

    if (strcmp(tokens[0], "create_canvas") == 0) {
        size_t height, width;
        color_t color;
        struct canvas *canvas;
        uint64_t id;
        if (ntokens != 4 || !parse_size(tokens[1], &height) ||
            !parse_size(tokens[2], &width) || !parse_color(tokens[3], &color) ||
            height == 0 || width == 0) {
            free(line); return response_code(RPC_VALUE);
        }
        canvas = animate_create_canvas(height, width, color);
        fprintf(stderr, "Created canvas %p\n", (void*)canvas);
        if (canvas == NULL) { free(line); return response_code(RPC_INTERNAL); }
        id = handles_add_canvas(ctx->handles, canvas, width, height, client->username);
        if (id == INVALID_HANDLE) { animate_destroy_canvas(canvas); free(line); return response_code(RPC_INTERNAL); }
        free(line); return response_u64(id);
    }

    if (strcmp(tokens[0], "create_sprite") == 0) {
        struct sprite *sprite;
        uint64_t id;
        if (ntokens != 2) { free(line); return response_code(RPC_FAIL); }
        sprite = animate_create_sprite(tokens[1]);
        if (sprite == NULL) { free(line); return response_code(RPC_INTERNAL); }
        id = handles_add_sprite(ctx->handles, sprite, client->username);
        if (id == INVALID_HANDLE) { animate_destroy_sprite(sprite); free(line); return response_code(RPC_INTERNAL); }
        free(line); return response_u64(id);
    }

    if (strcmp(tokens[0], "create_rectangle") == 0) {
        size_t width, height;
        color_t color;
        bool filled;
        struct sprite *sprite;
        uint64_t id;
        if (ntokens != 5 || !parse_size(tokens[1], &width) ||
            !parse_size(tokens[2], &height) || !parse_color(tokens[3], &color) ||
            !parse_bool_token(tokens[4], &filled) || width == 0 || height == 0) {
            free(line); return response_code(RPC_VALUE);
        }
        sprite = animate_create_rectangle(width, height, color, filled);
        if (sprite == NULL) { free(line); return response_code(RPC_INTERNAL); }
        id = handles_add_sprite(ctx->handles, sprite, client->username);
        if (id == INVALID_HANDLE) { animate_destroy_sprite(sprite); free(line); return response_code(RPC_INTERNAL); }
        free(line); return response_u64(id);
    }

    if (strcmp(tokens[0], "create_circle") == 0) {
        size_t radius;
        color_t color;
        bool filled;
        struct sprite *sprite;
        uint64_t id;
        if (ntokens != 4 || !parse_size(tokens[1], &radius) ||
            !parse_color(tokens[2], &color) || !parse_bool_token(tokens[3], &filled) ||
            radius == 0) {
            free(line); return response_code(RPC_VALUE);
        }
        sprite = animate_create_circle(radius, color, filled);
        if (sprite == NULL) { free(line); return response_code(RPC_INTERNAL); }
        id = handles_add_sprite(ctx->handles, sprite, client->username);
        if (id == INVALID_HANDLE) { animate_destroy_sprite(sprite); free(line); return response_code(RPC_INTERNAL); }
        free(line); return response_u64(id);
    }

    if (strcmp(tokens[0], "destroy_canvas") == 0) {
        uint64_t id;
        int rc;
        if (ntokens != 2 || !parse_u64(tokens[1], &id)) { free(line); return response_code(RPC_VALUE); }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        rc = handles_destroy_canvas_for_user(ctx->handles, id, client->username);
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        free(line); return response_code(rc == 0 ? 0 : RPC_VALUE);
    }

    if (strcmp(tokens[0], "destroy_sprite") == 0) {
        uint64_t id;
        int rc;
        if (ntokens != 2 || !parse_u64(tokens[1], &id)) { free(line); return response_code(RPC_VALUE); }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        rc = handles_destroy_sprite(ctx->handles, id, client->username);
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        free(line);
        if (rc == -1) { return response_code(RPC_VALUE); }
        return response_u64((uint64_t)rc);
    }

    if (strcmp(tokens[0], "place_sprite") == 0) {
        uint64_t canvas_id, sprite_id;
        ssize_t x, y;
        canvas_record_t *canvas;
        sprite_record_t *sprite;
        struct sprite_placement *placement;
        uint64_t id;
        if (ntokens != 5 || !parse_u64(tokens[1], &canvas_id) ||
            !parse_u64(tokens[2], &sprite_id) || !parse_ssize(tokens[3], &x) ||
            !parse_ssize(tokens[4], &y)) { free(line); return response_code(RPC_VALUE); }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        canvas = handles_get_canvas(ctx->handles, canvas_id, client->username);
        sprite = handles_get_sprite(ctx->handles, sprite_id, client->username);
        if (canvas == NULL || sprite == NULL) { pthread_mutex_unlock(handles_mutex(ctx->handles)); free(line); return response_code(RPC_VALUE); }
        wait_canvas_barrier_if_needed(ctx, canvas, client);
        placement = animate_place_sprite(canvas_record_ptr(canvas), sprite_record_ptr(sprite), x, y);
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        if (placement == NULL) { free(line); return response_code(RPC_INTERNAL); }
        id = handles_add_placement(ctx->handles, placement, canvas_id, sprite_id, client->username);
        if (id == INVALID_HANDLE) { animate_destroy_placement(placement); free(line); return response_code(RPC_INTERNAL); }
        free(line); return response_u64(id);
    }

    if (strcmp(tokens[0], "placement_up") == 0 || strcmp(tokens[0], "placement_down") == 0 ||
        strcmp(tokens[0], "placement_top") == 0 || strcmp(tokens[0], "placement_bottom") == 0) {
        uint64_t id;
        placement_record_t *placement;
        canvas_record_t *canvas;
        if (ntokens != 2 || !parse_u64(tokens[1], &id)) { free(line); return response_code(RPC_VALUE); }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        placement = handles_get_placement(ctx->handles, id, client->username);
        if (placement == NULL) { pthread_mutex_unlock(handles_mutex(ctx->handles)); free(line); return response_code(RPC_VALUE); }
        canvas = handles_get_canvas(ctx->handles, placement_record_canvas_id(placement), client->username);
        if (canvas != NULL) { wait_canvas_barrier_if_needed(ctx, canvas, client); }
        if (strcmp(tokens[0], "placement_up") == 0) animate_placement_up(placement_record_ptr(placement));
        else if (strcmp(tokens[0], "placement_down") == 0) animate_placement_down(placement_record_ptr(placement));
        else if (strcmp(tokens[0], "placement_top") == 0) animate_placement_top(placement_record_ptr(placement));
        else animate_placement_bottom(placement_record_ptr(placement));
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        free(line); return response_code(0);
    }

    if (strcmp(tokens[0], "destroy_placement") == 0) {
        uint64_t id;
        int rc;
        if (ntokens != 2 || !parse_u64(tokens[1], &id)) { free(line); return response_code(RPC_VALUE); }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        rc = handles_destroy_placement(ctx->handles, id, client->username);
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        free(line); return response_code(rc == 0 ? 0 : RPC_VALUE);
    }

    if (strcmp(tokens[0], "set_animation_params") == 0) {
        uint64_t id;
        ssize_t vx, vy, ax, ay;
        placement_record_t *placement;
        canvas_record_t *canvas;
        if (ntokens != 6 || !parse_u64(tokens[1], &id) ||
            !parse_ssize(tokens[2], &vx) || !parse_ssize(tokens[3], &vy) ||
            !parse_ssize(tokens[4], &ax) || !parse_ssize(tokens[5], &ay)) {
            free(line); return response_code(RPC_VALUE);
        }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        placement = handles_get_placement(ctx->handles, id, client->username);
        if (placement == NULL) { pthread_mutex_unlock(handles_mutex(ctx->handles)); free(line); return response_code(RPC_VALUE); }
        canvas = handles_get_canvas(ctx->handles, placement_record_canvas_id(placement), client->username);
        if (canvas != NULL) { wait_canvas_barrier_if_needed(ctx, canvas, client); }
        animate_set_animation_params(placement_record_ptr(placement), vx, vy, ax, ay);
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        free(line); return response_code(0);
    }

    if (strcmp(tokens[0], "share_canvas") == 0) {
        uint64_t canvas_id;
        canvas_record_t *canvas;
        int rc;
        if (ntokens != 3 || !parse_u64(tokens[1], &canvas_id) ||
            !auth_valid_username(tokens[2]) || !auth_user_exists(ctx->auth, tokens[2])) {
            free(line); return response_code(RPC_VALUE);
        }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        canvas = handles_get_canvas(ctx->handles, canvas_id, client->username);
        if (canvas == NULL) { pthread_mutex_unlock(handles_mutex(ctx->handles)); free(line); return response_code(RPC_VALUE); }
        rc = canvas_record_share(ctx->handles, canvas, tokens[2]);
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        free(line); return response_code(rc == 0 ? 0 : RPC_INTERNAL);
    }

    if (strcmp(tokens[0], "barrier") == 0) {
        uint64_t canvas_id;
        canvas_record_t *canvas;
        int rc;
        if (ntokens != 2 || !parse_u64(tokens[1], &canvas_id)) { free(line); return response_code(RPC_VALUE); }
        pthread_mutex_lock(handles_mutex(ctx->handles));
        canvas = handles_get_canvas(ctx->handles, canvas_id, client->username);
        if (canvas == NULL) { pthread_mutex_unlock(handles_mutex(ctx->handles)); free(line); return response_code(RPC_VALUE); }
        rc = canvas_record_barrier(ctx->handles, canvas, client->username);
        pthread_mutex_unlock(handles_mutex(ctx->handles));
        free(line); return response_code(rc == 0 ? 0 : RPC_INTERNAL);
    }

    if (strcmp(tokens[0], "generate") == 0) {
        char *result = handle_generate(ctx, client, tokens, ntokens);
        free(line);
        return result;
    }

    free(line);
    return response_code(RPC_FAIL);
}

/* This function can be called when a client disconnects unexpectedly, or when they explicitly request disconnection. 
It performs necessary cleanup of server-side state related to the client. */
void rpc_client_disconnected(rpc_context_t *ctx, client_state_t *client) {
    if (ctx == NULL || client == NULL || !client->logged_in) {
        return;
    }
    handles_disconnect_user(ctx->handles, client->username);

    /* Mark client for server-side cleanup shortly. This lets the main
     * thread detect the disconnect and close FIFOs/unlink resources. */
    client->disconnect_after = time(NULL) + 1;
    client->logged_in = false;
    client->username[0] = '\0';
}
