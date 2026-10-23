#pragma once

#include <limits.h>
#include <stddef.h>

#define TUNNEL_TEXT_MAX 256

enum forward_type {
    LOCAL,   // ssh -L
    REMOTE,  // ssh -R
    DYNAMIC, // ssh -D
};

struct tunnel {
    char name[TUNNEL_TEXT_MAX];
    char host[TUNNEL_TEXT_MAX];
    enum forward_type type;
    int listen_port;                   // on your machine for LOCAL and DYNAMIC, on the server for REMOTE
    char target_host[TUNNEL_TEXT_MAX]; // unused for DYNAMIC
    int target_port;                   // unused for DYNAMIC
};

struct config {
    char path[PATH_MAX];
    struct tunnel *tunnels;
    size_t count;
};

int config_load(const char *path, struct config *config);

// Releases everything config_load allocated. Call it once after every successful config_load.
void config_free(struct config *config);

const struct tunnel *config_find(const struct config *config, const char *name);
const char *forward_type_name(enum forward_type type);
