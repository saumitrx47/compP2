#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stdint.h>

#define AUTH_USERNAME_MAX 32

/* Opaque data type for the authentication database. The content of this struct
 is not exposed to users of the API. */
typedef struct auth_db auth_db_t;

/* Opaque data type for a user in the authentication database. The content of
 this struct is not exposed to users of the API. */
auth_db_t *auth_load(const char *path);

/* Validates that the username is non-empty, does not exceed the maximum length,
 and consists only of alphanumeric characters. */
void auth_destroy(auth_db_t *db);

/* Looks up a user in the authentication database and retrieves their balance. */
bool auth_lookup(auth_db_t *db, const char *username, int32_t *balance);

/* Checks if a user exists in the authentication database. */
bool auth_user_exists(auth_db_t *db, const char *username);

/* Validates that the username is non-empty, does not exceed the maximum length,
 and consists only of alphanumeric characters. */
bool auth_valid_username(const char *username);

#endif
