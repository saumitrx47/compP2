#include "auth.h"
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct auth_user {
    char username[AUTH_USERNAME_MAX + 1];
    int32_t balance;
    struct auth_user *next;
} auth_user_t;

struct auth_db {
    auth_user_t *users;
};

bool auth_valid_username(const char *username) {
    size_t len;

    if (username == NULL) {
        return false;
    }
    len = strlen(username);
    if (len == 0 || len > AUTH_USERNAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)username[i])) {
            return false;
        }
    }
    return true;
}

static bool parse_balance(const char *s, int32_t *out) {
    char *end = NULL;
    long value;

    errno = 0;
    value = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0' ||
        value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    *out = (int32_t)value;
    return true;
}

auth_db_t *auth_load(const char *path) {
    FILE *file = fopen(path, "r");
    auth_db_t *db = calloc(1, sizeof(*db));
    char line[512];

    if (db == NULL) {
        return NULL;
    }
    if (file == NULL) {
        return db;
    }

    while (fgets(line, sizeof(line), file) != NULL) {
        char *save = NULL;
        char *username = strtok_r(line, " \t\r\n", &save);
        char *balance_s = strtok_r(NULL, " \t\r\n", &save);
        int32_t balance;
        auth_user_t *user;

        if (username == NULL || balance_s == NULL ||
            !auth_valid_username(username) || !parse_balance(balance_s, &balance)) {
            continue;
        }

        user = calloc(1, sizeof(*user));
        if (user == NULL) {
            fclose(file);
            auth_destroy(db);
            return NULL;
        }
        strncpy(user->username, username, AUTH_USERNAME_MAX);
        user->balance = balance;
        user->next = db->users;
        db->users = user;
    }

    fclose(file);
    return db;
}

void auth_destroy(auth_db_t *db) {
    auth_user_t *user;

    if (db == NULL) {
        return;
    }
    user = db->users;
    while (user != NULL) {
        auth_user_t *next = user->next;
        free(user);
        user = next;
    }
    free(db);
}

bool auth_lookup(auth_db_t *db, const char *username, int32_t *balance) {
    if (db == NULL || username == NULL) {
        return false;
    }
    for (auth_user_t *user = db->users; user != NULL; user = user->next) {
        if (strcmp(user->username, username) == 0) {
            if (balance != NULL) {
                *balance = user->balance;
            }
            return true;
        }
    }
    return false;
}

bool auth_user_exists(auth_db_t *db, const char *username) {
    return auth_lookup(db, username, NULL);
}
