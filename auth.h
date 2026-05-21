#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stdint.h>

#define AUTH_USERNAME_MAX 32

typedef struct auth_db auth_db_t;

auth_db_t *auth_load(const char *path);
void auth_destroy(auth_db_t *db);
bool auth_lookup(auth_db_t *db, const char *username, int32_t *balance);
bool auth_user_exists(auth_db_t *db, const char *username);
bool auth_valid_username(const char *username);

#endif
