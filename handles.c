#include "handles.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct shared_user {
    char username[MAX_USERNAME_LEN + 1];
    bool connected;
    bool barrier_waiting;
    struct shared_user *next;
} shared_user_t;

struct canvas_record {
    uint64_t id;
    struct canvas *canvas;
    size_t width;
    size_t height;
    bool deleted;
    shared_user_t *users;
    pthread_cond_t barrier_cond;
    struct canvas_record *next;
};

struct sprite_record {
    uint64_t id;
    struct sprite *sprite;
    char owner[MAX_USERNAME_LEN + 1];
    bool deleted;
    struct sprite_record *next;
};

struct placement_record {
    uint64_t id;
    struct sprite_placement *placement;
    uint64_t canvas_id;
    uint64_t sprite_id;
    char owner[MAX_USERNAME_LEN + 1];
    bool deleted;
    struct placement_record *next;
};

struct handle_store {
    pthread_mutex_t mutex;
    uint64_t next_id;
    canvas_record_t *canvases;
    sprite_record_t *sprites;
    placement_record_t *placements;
};

static void copy_username(char dest[MAX_USERNAME_LEN + 1], const char *src) {
    memset(dest, 0, MAX_USERNAME_LEN + 1);
    if (src != NULL) {
        strncpy(dest, src, MAX_USERNAME_LEN);
    }
}

// Returns the shared user with the given username in the canvas, or NULL if not found.
static shared_user_t *find_shared_user(canvas_record_t *canvas,
                                       const char *username) {
    for (shared_user_t *user = canvas->users; user != NULL; user = user->next) {
        if (strcmp(user->username, username) == 0) {
            return user;
        }
    }
    return NULL;
}

// Returns true if the canvas has at least one connected user, false otherwise.
static bool canvas_has_connected_user(canvas_record_t *canvas) {
    for (shared_user_t *user = canvas->users; user != NULL; user = user->next) {
        if (user->connected) {
            return true;
        }
    }
    return false;
}

// Returns true if all connected users of the canvas are waiting at the barrier, false otherwise.
static bool canvas_barrier_complete(canvas_record_t *canvas) {
    for (shared_user_t *user = canvas->users; user != NULL; user = user->next) {
        if (user->connected && !user->barrier_waiting) {
            return false;
        }
    }
    return true;
}

static void maybe_destroy_canvas(canvas_record_t *canvas) {
    if (canvas->deleted && canvas->canvas != NULL && !canvas_has_connected_user(canvas)) {
        animate_destroy_canvas(canvas->canvas);
        canvas->canvas = NULL;
    }
}

// Creates a new handle store. Returns NULL on failure.
handle_store_t *handles_create(void) {
    handle_store_t *store = calloc(1, sizeof(*store));
    if (store == NULL) {
        return NULL;
    }
    if (pthread_mutex_init(&store->mutex, NULL) != 0) {
        free(store);
        return NULL;
    }
    store->next_id = 1;
    return store;
}

// Destroys the handle store and all associated resources. Safe to call with NULL.
void handles_destroy(handle_store_t *store) {
    canvas_record_t *canvas;
    sprite_record_t *sprite;
    placement_record_t *placement;

    if (store == NULL) {
        return;
    }

    placement = store->placements;
    while (placement != NULL) {
        placement_record_t *next = placement->next;
        if (placement->placement != NULL) {
            animate_destroy_placement(placement->placement);
        }
        free(placement);
        placement = next;
    }

    canvas = store->canvases;
    while (canvas != NULL) {
        canvas_record_t *next = canvas->next;
        shared_user_t *user = canvas->users;
        if (canvas->canvas != NULL) {
            animate_destroy_canvas(canvas->canvas);
        }
        while (user != NULL) {
            shared_user_t *next_user = user->next;
            free(user);
            user = next_user;
        }
        pthread_cond_destroy(&canvas->barrier_cond);
        free(canvas);
        canvas = next;
    }

    sprite = store->sprites;
    while (sprite != NULL) {
        sprite_record_t *next = sprite->next;
        if (sprite->sprite != NULL) {
            animate_destroy_sprite(sprite->sprite);
        }
        free(sprite);
        sprite = next;
    }

    pthread_mutex_destroy(&store->mutex);
    free(store);
}

uint64_t handles_add_canvas(handle_store_t *store, struct canvas *canvas,
                            size_t width, size_t height, const char *owner) {
    canvas_record_t *record;
    shared_user_t *user;

    if (store == NULL || canvas == NULL || owner == NULL) {
        return INVALID_HANDLE;
    }
    record = calloc(1, sizeof(*record));
    user = calloc(1, sizeof(*user));
    if (record == NULL || user == NULL) {
        free(record);
        free(user);
        return INVALID_HANDLE;
    }

    pthread_mutex_lock(&store->mutex);
    record->id = store->next_id++;
    record->canvas = canvas;
    record->width = width;
    record->height = height;
    copy_username(user->username, owner);
    user->connected = true;
    record->users = user;
    pthread_cond_init(&record->barrier_cond, NULL);
    record->next = store->canvases;
    store->canvases = record;
    pthread_mutex_unlock(&store->mutex);

    return record->id;
}

uint64_t handles_add_sprite(handle_store_t *store, struct sprite *sprite,
                            const char *owner) {
    sprite_record_t *record;

    if (store == NULL || sprite == NULL || owner == NULL) {
        return INVALID_HANDLE;
    }
    record = calloc(1, sizeof(*record));
    if (record == NULL) {
        return INVALID_HANDLE;
    }

    pthread_mutex_lock(&store->mutex);
    record->id = store->next_id++;
    record->sprite = sprite;
    copy_username(record->owner, owner);
    record->next = store->sprites;
    store->sprites = record;
    pthread_mutex_unlock(&store->mutex);

    return record->id;
}

uint64_t handles_add_placement(handle_store_t *store,
                               struct sprite_placement *placement,
                               uint64_t canvas_id, uint64_t sprite_id,
                               const char *owner) {
    placement_record_t *record;

    if (store == NULL || placement == NULL || owner == NULL) {
        return INVALID_HANDLE;
    }
    record = calloc(1, sizeof(*record));
    if (record == NULL) {
        return INVALID_HANDLE;
    }

    pthread_mutex_lock(&store->mutex);
    record->id = store->next_id++;
    record->placement = placement;
    record->canvas_id = canvas_id;
    record->sprite_id = sprite_id;
    copy_username(record->owner, owner);
    record->next = store->placements;
    store->placements = record;
    pthread_mutex_unlock(&store->mutex);

    return record->id;
}

canvas_record_t *handles_get_canvas(handle_store_t *store, uint64_t id,
                                    const char *username) {
    if (store == NULL || username == NULL || id == INVALID_HANDLE) {
        return NULL;
    }
    for (canvas_record_t *record = store->canvases; record != NULL;
         record = record->next) {
        if (record->id == id && record->canvas != NULL && !record->deleted) {
            shared_user_t *user = find_shared_user(record, username);
            if (user != NULL) {
                user->connected = true;
                return record;
            }
        }
    }
    return NULL;
}

sprite_record_t *handles_get_sprite(handle_store_t *store, uint64_t id,
                                    const char *username) {
    if (store == NULL || username == NULL || id == INVALID_HANDLE) {
        return NULL;
    }
    for (sprite_record_t *record = store->sprites; record != NULL;
         record = record->next) {
        if (record->id == id && record->sprite != NULL && !record->deleted &&
            strcmp(record->owner, username) == 0) {
            return record;
        }
    }
    return NULL;
}

placement_record_t *handles_get_placement(handle_store_t *store, uint64_t id,
                                          const char *username) {
    if (store == NULL || username == NULL || id == INVALID_HANDLE) {
        return NULL;
    }
    for (placement_record_t *record = store->placements; record != NULL;
         record = record->next) {
        if (record->id == id && record->placement != NULL && !record->deleted &&
            strcmp(record->owner, username) == 0) {
            return record;
        }
    }
    return NULL;
}

// The following functions return the internal pointers and properties of the records, as well as the mutex for synchronization.
struct canvas *canvas_record_ptr(canvas_record_t *record) { return record->canvas; }
size_t canvas_record_width(canvas_record_t *record) { return record->width; }
size_t canvas_record_height(canvas_record_t *record) { return record->height; }
uint64_t canvas_record_id(canvas_record_t *record) { return record->id; }
struct sprite *sprite_record_ptr(sprite_record_t *record) { return record->sprite; }
struct sprite_placement *placement_record_ptr(placement_record_t *record) { return record->placement; }
uint64_t placement_record_canvas_id(placement_record_t *record) { return record->canvas_id; }
pthread_mutex_t *handles_mutex(handle_store_t *store) { return &store->mutex; }

// The following functions implement the sharing and barrier synchronization for canvases, as well as destroying resources when users disconnect.
bool canvas_record_has_waiting_barrier(canvas_record_t *record,
                                       const char *username) {
    shared_user_t *user = find_shared_user(record, username);
    return user != NULL && user->barrier_waiting;
}

void canvas_record_wait_for_barrier(handle_store_t *store,
                                    canvas_record_t *record,
                                    const char *username) {
    shared_user_t *user = find_shared_user(record, username);
    while (store != NULL && user != NULL && user->barrier_waiting) {
        pthread_cond_wait(&record->barrier_cond, &store->mutex);
    }
}

int canvas_record_share(handle_store_t *store, canvas_record_t *record,
                        const char *username) {
    shared_user_t *user;

    if (store == NULL || record == NULL || username == NULL) {
        return -1;
    }
    if (find_shared_user(record, username) != NULL) {
        return 0;
    }
    user = calloc(1, sizeof(*user));
    if (user == NULL) {
        return -1;
    }
    copy_username(user->username, username);
    user->connected = false;
    user->next = record->users;
    record->users = user;
    return 0;
}

int canvas_record_barrier(handle_store_t *store, canvas_record_t *record,
                          const char *username) {
    shared_user_t *user;

    (void)store;
    if (record == NULL || username == NULL) {
        return -1;
    }
    user = find_shared_user(record, username);
    if (user == NULL) {
        return -1;
    }
    user->barrier_waiting = true;
    if (canvas_barrier_complete(record)) {
        for (shared_user_t *cur = record->users; cur != NULL; cur = cur->next) {
            cur->barrier_waiting = false;
        }
        pthread_cond_broadcast(&record->barrier_cond);
        return 0;
    }
    while (user->barrier_waiting) {
        pthread_cond_wait(&record->barrier_cond, &store->mutex);
    }
    return 0;
}

int handles_destroy_canvas_for_user(handle_store_t *store, uint64_t id,
                                    const char *username) {
    canvas_record_t *record = handles_get_canvas(store, id, username);
    shared_user_t *user;

    if (record == NULL) {
        return -1;
    }
    user = find_shared_user(record, username);
    if (user != NULL) {
        user->connected = false;
        user->barrier_waiting = false;
    }
    if (!canvas_has_connected_user(record)) {
        record->deleted = true;
    }
    maybe_destroy_canvas(record);
    pthread_cond_broadcast(&record->barrier_cond);
    return 0;
}

int handles_destroy_sprite(handle_store_t *store, uint64_t id,
                           const char *username) {
    sprite_record_t *record = handles_get_sprite(store, id, username);

    if (record == NULL) {
        return -1;
    }
    if (record->sprite != NULL) {
        bool failed = animate_destroy_sprite(record->sprite);
        if (failed) {
            return 1;
        }
    }
    record->sprite = NULL;
    record->deleted = true;
    return 0;
}

int handles_destroy_placement(handle_store_t *store, uint64_t id,
                              const char *username) {
    placement_record_t *record = handles_get_placement(store, id, username);

    if (record == NULL) {
        return -1;
    }
    if (record->placement != NULL) {
        animate_destroy_placement(record->placement);
    }
    record->placement = NULL;
    record->deleted = true;
    return 0;
}

void handles_disconnect_user(handle_store_t *store, const char *username) {
    if (store == NULL || username == NULL || username[0] == '\0') {
        return;
    }
    pthread_mutex_lock(&store->mutex);
    for (canvas_record_t *canvas = store->canvases; canvas != NULL;
         canvas = canvas->next) {
        shared_user_t *user = find_shared_user(canvas, username);
        if (user != NULL) {
            user->connected = false;
            user->barrier_waiting = false;
            maybe_destroy_canvas(canvas);
            pthread_cond_broadcast(&canvas->barrier_cond);
        }
    }
    pthread_mutex_unlock(&store->mutex);
}
