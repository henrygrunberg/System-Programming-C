#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "errors.h"

struct raw_value {
    char text[TUNNEL_TEXT_MAX];
    int line; // 0 means the key was not given
};

struct section {
    char name[TUNNEL_TEXT_MAX];
    int line;
    struct raw_value host;
    struct raw_value type;
    struct raw_value local;
    struct raw_value remote;
};

static const char *const type_names[] = {
    [LOCAL] = "local",
    [REMOTE] = "remote",
    [DYNAMIC] = "dynamic",
};

static int resolve_path(const char *path, char *resolved_path, size_t size) {
    int written;

    if (path != NULL) {
        written = snprintf(resolved_path, size, "%s", path);
    } else {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            print_error("HOME is not set, use -c <file> to give the config path");
            return -1;
        }
        written = snprintf(resolved_path, size, "%s/.tunnel/tunnels.conf", home);
    }

    if (written < 0 || (size_t)written >= size) {
        print_error("config path is too long");
        return -1;
    }
    return 0;
}

static char *trim(char *text) {
    while (isspace((unsigned char)*text)) {
        text++;
    }
    char *end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';
    return text;
}

static bool is_valid_name(const char *name) {
    if (name[0] == '\0') {
        return false;
    }
    for (const char *c = name; *c != '\0'; c++) {
        if (!isalnum((unsigned char)*c) && *c != '-' && *c != '_') {
            return false;
        }
    }
    return true;
}

static int parse_port(const char *text, int *port) {
    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || value < 1 || value > 65535) {
        return -1;
    }
    *port = (int)value;
    return 0;
}

static int parse_host_port(const char *text, char *host, size_t host_size, int *port) {
    const char *colon = strrchr(text, ':');
    if (colon == NULL || colon == text) {
        return -1;
    }

    size_t host_length = (size_t)(colon - text);
    if (host_length >= host_size) {
        return -1;
    }
    memcpy(host, text, host_length);
    host[host_length] = '\0';

    return parse_port(colon + 1, port);
}

static int parse_type(const char *text, enum forward_type *type) {
    size_t count = sizeof(type_names) / sizeof(type_names[0]);
    for (size_t i = 0; i < count; i++) {
        if (strcmp(text, type_names[i]) == 0) {
            *type = (enum forward_type)i;
            return 0;
        }
    }
    return -1;
}

static int add_tunnel(struct config *config, const struct tunnel *tunnel) {
    struct tunnel *grown = realloc(config->tunnels, (config->count + 1) * sizeof(*grown));
    if (grown == NULL) {
        print_error("out of memory");
        return -1;
    }
    config->tunnels = grown;
    config->tunnels[config->count] = *tunnel;
    config->count++;
    return 0;
}

static int finish_section(struct config *config, const struct section *section) {
    const char *path = config->path;
    struct tunnel tunnel = {0};
    snprintf(tunnel.name, sizeof(tunnel.name), "%s", section->name);

    if (section->host.line == 0) {
        print_error("%s:%d: tunnel '%s' has no 'host'", path, section->line, section->name);
        return -1;
    }
    // A host starting with '-' would be read by ssh as an option, e.g. -oProxyCommand=...
    if (section->host.text[0] == '-') {
        print_error("%s:%d: host must not start with '-'", path, section->host.line);
        return -1;
    }
    snprintf(tunnel.host, sizeof(tunnel.host), "%s", section->host.text);

    tunnel.type = LOCAL;
    if (section->type.line != 0 && parse_type(section->type.text, &tunnel.type) != 0) {
        print_error("%s:%d: unknown type '%s' (expected local, remote or dynamic)", path, section->type.line,
                    section->type.text);
        return -1;
    }

    bool is_remote = tunnel.type == REMOTE;
    const struct raw_value *listen = is_remote ? &section->remote : &section->local;
    const struct raw_value *target = is_remote ? &section->local : &section->remote;
    const char *listen_key = is_remote ? "remote" : "local";

    if (listen->line == 0) {
        print_error("%s:%d: tunnel '%s' needs '%s = <port>'", path, section->line, section->name, listen_key);
        return -1;
    }
    if (parse_port(listen->text, &tunnel.listen_port) != 0) {
        print_error("%s:%d: invalid port '%s' (expected 1-65535)", path, listen->line, listen->text);
        return -1;
    }

    if (tunnel.type != DYNAMIC) {
        if (target->line == 0) {
            print_error("%s:%d: tunnel '%s' needs '%s = <host>:<port>'", path, section->line, section->name,
                        is_remote ? "local" : "remote");
            return -1;
        }
        if (parse_host_port(target->text, tunnel.target_host, sizeof(tunnel.target_host), &tunnel.target_port) != 0) {
            print_error("%s:%d: invalid '%s' (expected <host>:<port>)", path, target->line, target->text);
            return -1;
        }
    }

    // REMOTE listens on the server, so only LOCAL and DYNAMIC can clash on this machine.
    if (!is_remote) {
        for (size_t i = 0; i < config->count; i++) {
            const struct tunnel *other = &config->tunnels[i];
            if (other->type != REMOTE && other->listen_port == tunnel.listen_port) {
                print_error("%s:%d: local port %d is already used by tunnel '%s'", path, listen->line,
                            tunnel.listen_port, other->name);
                return -1;
            }
        }
    }

    return add_tunnel(config, &tunnel);
}

static int start_section(const struct config *config, struct section *section, char *header, int line) {
    size_t length = strlen(header);
    if (header[length - 1] != ']') {
        print_error("%s:%d: missing ']' in section header", config->path, line);
        return -1;
    }
    header[length - 1] = '\0';
    char *name = trim(header + 1);

    if (!is_valid_name(name)) {
        print_error("%s:%d: invalid tunnel name '%s' (use letters, digits, '-' and '_')", config->path, line, name);
        return -1;
    }
    if (config_find(config, name) != NULL) {
        print_error("%s:%d: duplicate tunnel '%s'", config->path, line, name);
        return -1;
    }

    *section = (struct section){.line = line};
    snprintf(section->name, sizeof(section->name), "%s", name);
    return 0;
}

static int set_key(const struct config *config, struct section *section, char *assignment, int line) {
    char *equals = strchr(assignment, '=');
    if (equals == NULL) {
        print_error("%s:%d: expected 'key = value'", config->path, line);
        return -1;
    }
    *equals = '\0';
    char *key = trim(assignment);
    char *value = trim(equals + 1);

    struct raw_value *slot;
    if (strcmp(key, "host") == 0) {
        slot = &section->host;
    } else if (strcmp(key, "type") == 0) {
        slot = &section->type;
    } else if (strcmp(key, "local") == 0) {
        slot = &section->local;
    } else if (strcmp(key, "remote") == 0) {
        slot = &section->remote;
    } else {
        print_error("%s:%d: unknown key '%s' (expected host, type, local or remote)", config->path, line, key);
        return -1;
    }

    if (value[0] == '\0') {
        print_error("%s:%d: '%s' has no value", config->path, line, key);
        return -1;
    }

    snprintf(slot->text, sizeof(slot->text), "%s", value);
    slot->line = line;
    return 0;
}

static int parse_file(FILE *file, struct config *config) {
    // A line is no longer than the name and value buffers, so nothing read from it can overflow them.
    char line[TUNNEL_TEXT_MAX];
    int line_number = 0;
    struct section section;
    bool in_section = false;

    while (fgets(line, sizeof(line), file) != NULL) {
        line_number++;

        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\n') {
            line[length - 1] = '\0';
        } else if (!feof(file) && fgetc(file) != EOF) {
            print_error("%s:%d: line is too long (max %zu characters)", config->path, line_number, sizeof(line) - 2);
            return -1;
        }

        char *text = trim(line);
        if (text[0] == '\0' || text[0] == '#') {
            continue;
        }

        if (text[0] == '[') {
            if (in_section && finish_section(config, &section) != 0) {
                return -1;
            }
            if (start_section(config, &section, text, line_number) != 0) {
                return -1;
            }
            in_section = true;
        } else if (!in_section) {
            print_error("%s:%d: '%s' is outside of a [tunnel] section", config->path, line_number, text);
            return -1;
        } else if (set_key(config, &section, text, line_number) != 0) {
            return -1;
        }
    }

    if (ferror(file)) {
        print_error("cannot read '%s': %s", config->path, strerror(errno));
        return -1;
    }
    if (in_section) {
        return finish_section(config, &section);
    }
    return 0;
}

int config_load(const char *path, struct config *config) {
    *config = (struct config){0};

    if (resolve_path(path, config->path, sizeof(config->path)) != 0) {
        return -1;
    }

    FILE *file = fopen(config->path, "r");
    if (file == NULL) {
        print_error("cannot open config '%s': %s", config->path, strerror(errno));
        fprintf(stderr, "See tunnels.conf.example or the README for the format.\n");
        return -1;
    }

    int result = parse_file(file, config);
    fclose(file);

    if (result != 0) {
        config_free(config);
    }
    return result;
}

void config_free(struct config *config) {
    free(config->tunnels);
    *config = (struct config){0};
}

const struct tunnel *config_find(const struct config *config, const char *name) {
    for (size_t i = 0; i < config->count; i++) {
        if (strcmp(config->tunnels[i].name, name) == 0) {
            return &config->tunnels[i];
        }
    }
    return NULL;
}

const char *forward_type_name(enum forward_type type) {
    return type_names[type];
}
