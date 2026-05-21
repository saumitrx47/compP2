#ifndef HANDLES_H
#define HANDLES_H

#include "server_common.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <animate/animate.h>

typedef struct handle_store handle_store_t;

typedef enum {
    HANDLE_CANVAS,
    HANDLE_SPRITE,
    HANDLE_PLACEMENT
} handle_type_t;

typedef struct canvas_record canvas_record_t;
typedef struct sprite_record sprite_record_t;
typedef struct placement_record placement_record_t;

handle_store_t *handles_create(void);
void handles_destroy(handle_store_t *store);

uint64_t handles_add_canvas(handle_store_t *store, struct canvas *canvas,
                            size_t width, size_t height, const char *owner);
uint64_t handles_add_sprite(handle_store_t *store, struct sprite *sprite,
                            const char *owner);
uint64_t handles_add_placement(handle_store_t *store,
                               struct sprite_placement *placement,
                               uint64_t canvas_id, uint64_t sprite_id,
                               const char *owner);

canvas_record_t *handles_get_canvas(handle_store_t *store, uint64_t id,
                                    const char *username);
sprite_record_t *handles_get_sprite(handle_store_t *store, uint64_t id,
                                    const char *username);
placement_record_t *handles_get_placement(handle_store_t *store, uint64_t id,
                                          const char *username);

struct canvas *canvas_record_ptr(canvas_record_t *record);
size_t canvas_record_width(canvas_record_t *record);
size_t canvas_record_height(canvas_record_t *record);
uint64_t canvas_record_id(canvas_record_t *record);
bool canvas_record_has_waiting_barrier(canvas_record_t *record,
                                       const char *username);
void canvas_record_wait_for_barrier(handle_store_t *store,
                                    canvas_record_t *record,
                                    const char *username);
int canvas_record_share(handle_store_t *store, canvas_record_t *record,
                        const char *username);
int canvas_record_barrier(handle_store_t *store, canvas_record_t *record,
                          const char *username);

struct sprite *sprite_record_ptr(sprite_record_t *record);
struct sprite_placement *placement_record_ptr(placement_record_t *record);
uint64_t placement_record_canvas_id(placement_record_t *record);

int handles_destroy_canvas_for_user(handle_store_t *store, uint64_t id,
                                    const char *username);
int handles_destroy_sprite(handle_store_t *store, uint64_t id,
                           const char *username);
int handles_destroy_placement(handle_store_t *store, uint64_t id,
                              const char *username);
void handles_disconnect_user(handle_store_t *store, const char *username);

pthread_mutex_t *handles_mutex(handle_store_t *store);

#endif
