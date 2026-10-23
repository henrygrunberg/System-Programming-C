#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "commands.h"
#include "errors.h"
#include "spawn.h"
#include "state.h"

// ssh gives up on an unreachable host after 5 s (ConnectTimeout), so this leaves time for the login on top.
#define STARTUP_TIMEOUT_MS 10000
// Remote tunnels listen on the server, so there is no local port to wait for. Waiting just longer than
// ssh's ConnectTimeout (5 s) means an unreachable host is reported as a failure, not as up.
#define REMOTE_STARTUP_WAIT_MS 6000
#define LOG_TAIL_LINES 5

static int describe_run_state(const char *state_dir, const char *name, struct run_status *status, char *out,
                              size_t size) {
    if (state_check(state_dir, name, status) != 0) {
        snprintf(out, size, "unknown");
        return -1;
    }

    switch (status->state) {
        case RUNNING: snprintf(out, size, "up (pid %d)", (int)status->pid); break;
        case STOPPED: snprintf(out, size, "down"); break;
        case STALE:
            if (state_remove_pidfile(state_dir, name) != 0) {
                snprintf(out, size, "stale");
                return -1;
            }
            snprintf(out, size, "stale (cleaned up)");
            break;
    }
    return 0;
}

static void format_target(const struct tunnel *tunnel, char *out, size_t size) {
    if (tunnel->type == DYNAMIC) {
        snprintf(out, size, "-");
    } else {
        snprintf(out, size, "%s:%d", tunnel->target_host, tunnel->target_port);
    }
}

static void describe_forward(const struct tunnel *tunnel, char *out, size_t size) {
    switch (tunnel->type) {
        case LOCAL:
            snprintf(out, size, "localhost:%d -> %s -> %s:%d", tunnel->listen_port, tunnel->host, tunnel->target_host,
                     tunnel->target_port);
            break;
        case REMOTE:
            snprintf(out, size, "%s:%d -> %s:%d", tunnel->host, tunnel->listen_port, tunnel->target_host,
                     tunnel->target_port);
            break;
        case DYNAMIC:
            snprintf(out, size, "SOCKS proxy on localhost:%d via %s", tunnel->listen_port, tunnel->host);
            break;
    }
}

static void describe_exit(int wait_status, char *out, size_t size) {
    if (WIFEXITED(wait_status)) {
        snprintf(out, size, "exit code %d", WEXITSTATUS(wait_status));
    } else if (WIFSIGNALED(wait_status)) {
        snprintf(out, size, "killed by signal %d", WTERMSIG(wait_status));
    } else {
        snprintf(out, size, "status %d", wait_status);
    }
}

static void print_log_tail(const char *log_path, FILE *out) {
    FILE *file = fopen(log_path, "r");
    if (file == NULL) {
        return;
    }

    char lines[LOG_TAIL_LINES][256];
    size_t count = 0;
    while (fgets(lines[count % LOG_TAIL_LINES], sizeof(lines[0]), file) != NULL) {
        count++;
    }
    fclose(file);

    size_t first = count > LOG_TAIL_LINES ? count - LOG_TAIL_LINES : 0;
    for (size_t i = first; i < count; i++) {
        const char *line = lines[i % LOG_TAIL_LINES];
        size_t length = strlen(line);
        fprintf(out, "  | %s", line);
        if (length == 0 || line[length - 1] != '\n') {
            fputc('\n', out);
        }
    }
}

static int start_tunnel(const struct tunnel *tunnel, const char *state_dir) {
    struct run_status status;
    if (state_check(state_dir, tunnel->name, &status) != 0) {
        return -1;
    }
    if (status.state == RUNNING) {
        printf("%s: already up (pid %d)\n", tunnel->name, (int)status.pid);
        return 0;
    }
    if (status.state == STALE && state_remove_pidfile(state_dir, tunnel->name) != 0) {
        return -1;
    }

    bool has_local_port = tunnel->type != REMOTE;
    if (has_local_port && port_accepts_connections(tunnel->listen_port)) {
        print_error("%s: local port %d is already in use by another program", tunnel->name, tunnel->listen_port);
        return -1;
    }

    char log_path[PATH_MAX];
    if (state_log_path(state_dir, tunnel->name, log_path, sizeof(log_path)) != 0) {
        return -1;
    }

    pid_t pid;
    if (spawn_tunnel(tunnel, log_path, &pid) != 0) {
        return -1;
    }
    if (state_write_pidfile(state_dir, tunnel->name, pid) != 0) {
        kill(pid, SIGTERM);
        return -1;
    }

    int wait_status;
    int ready_port = has_local_port ? tunnel->listen_port : 0;
    int timeout_ms = has_local_port ? STARTUP_TIMEOUT_MS : REMOTE_STARTUP_WAIT_MS;
    int exited = wait_for_startup(pid, ready_port, timeout_ms, &wait_status);
    if (exited == 0) {
        char forward[TUNNEL_TEXT_MAX * 2 + 64];
        describe_forward(tunnel, forward, sizeof(forward));
        printf("%s: up (pid %d) %s\n", tunnel->name, (int)pid, forward);
        return 0;
    }

    state_remove_pidfile(state_dir, tunnel->name);
    if (exited < 0) {
        return -1;
    }
    char reason[64];
    describe_exit(wait_status, reason, sizeof(reason));
    print_error("%s: ssh failed (%s), last lines of %s:", tunnel->name, reason, log_path);
    print_log_tail(log_path, stderr);
    return -1;
}

static int stop_tunnel(const struct tunnel *tunnel, const char *state_dir) {
    struct run_status status;
    if (state_check(state_dir, tunnel->name, &status) != 0) {
        return -1;
    }

    switch (status.state) {
        case STOPPED: printf("%s: already down\n", tunnel->name); return 0;
        case STALE:
            printf("%s: already down (removed stale pidfile)\n", tunnel->name);
            return state_remove_pidfile(state_dir, tunnel->name);
        case RUNNING: break;
    }

    if (stop_process(status.pid) != 0) {
        return -1;
    }
    if (state_remove_pidfile(state_dir, tunnel->name) != 0) {
        return -1;
    }
    printf("%s: stopped\n", tunnel->name);
    return 0;
}

static int open_state_dir(char *out, size_t size) {
    return state_dir(out, size) == 0 && state_ensure_dir(out) == 0 ? 0 : -1;
}

int cmd_up(const struct tunnel *tunnel) {
    char state_dir_path[PATH_MAX];
    if (open_state_dir(state_dir_path, sizeof(state_dir_path)) != 0) {
        return EXIT_RUNTIME_ERROR;
    }
    return start_tunnel(tunnel, state_dir_path) == 0 ? EXIT_OK : EXIT_RUNTIME_ERROR;
}

int cmd_down(const struct tunnel *tunnel) {
    char state_dir_path[PATH_MAX];
    if (open_state_dir(state_dir_path, sizeof(state_dir_path)) != 0) {
        return EXIT_RUNTIME_ERROR;
    }
    return stop_tunnel(tunnel, state_dir_path) == 0 ? EXIT_OK : EXIT_RUNTIME_ERROR;
}

static void format_duration(time_t seconds, char *out, size_t size) {
    long minutes = (long)(seconds / 60);
    long hours = minutes / 60;
    long days = hours / 24;

    if (days > 0) {
        snprintf(out, size, "%ldd %ldh", days, hours % 24);
    } else if (hours > 0) {
        snprintf(out, size, "%ldh %ldm", hours, minutes % 60);
    } else if (minutes > 0) {
        snprintf(out, size, "%ldm %lds", minutes, (long)(seconds % 60));
    } else {
        snprintf(out, size, "%lds", (long)seconds);
    }
}

static void print_since(time_t since) {
    struct tm local;
    char date[32];
    if (since == 0 || localtime_r(&since, &local) == NULL ||
        strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", &local) == 0) {
        return;
    }
    char duration[32];
    format_duration(time(NULL) - since, duration, sizeof(duration));
    printf("  since:    %s (%s ago)\n", date, duration);
}

int cmd_status(const struct tunnel *tunnel) {

    char state_dir_path[PATH_MAX];
    char log_path[PATH_MAX];
    if (state_dir(state_dir_path, sizeof(state_dir_path)) != 0 ||
        state_log_path(state_dir_path, tunnel->name, log_path, sizeof(log_path)) != 0) {
        return EXIT_RUNTIME_ERROR;
    }

    struct run_status status;
    char run_state[64];
    int exit_code = describe_run_state(state_dir_path, tunnel->name, &status, run_state, sizeof(run_state)) == 0
                        ? EXIT_OK
                        : EXIT_RUNTIME_ERROR;

    char forward[TUNNEL_TEXT_MAX * 2 + 64];
    describe_forward(tunnel, forward, sizeof(forward));

    printf("%s: %s\n", tunnel->name, run_state);
    if (status.state == RUNNING) {
        print_since(status.since);
    }
    printf("  forward:  %s\n", forward);
    printf("  type:     %s\n", forward_type_name(tunnel->type));
    if (access(log_path, R_OK) != 0) {
        printf("  log:      none yet\n");
        return exit_code;
    }
    printf("  log:      %s\n", log_path);
    printf("  last log lines:\n");
    print_log_tail(log_path, stdout);
    return exit_code;
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

int cmd_ls(const struct config *config) {
    char state_dir_path[PATH_MAX];
    if (state_dir(state_dir_path, sizeof(state_dir_path)) != 0) {
        return EXIT_RUNTIME_ERROR;
    }

    // Each column is as wide as its longest entry, so long names or hosts never push into the next column.
    int name_width = (int)strlen("NAME");
    int target_width = (int)strlen("TARGET");
    int host_width = (int)strlen("HOST");
    for (size_t i = 0; i < config->count; i++) {
        char target[TUNNEL_TEXT_MAX + 8];
        format_target(&config->tunnels[i], target, sizeof(target));
        name_width = max_int(name_width, (int)strlen(config->tunnels[i].name));
        target_width = max_int(target_width, (int)strlen(target));
        host_width = max_int(host_width, (int)strlen(config->tunnels[i].host));
    }

    int exit_code = EXIT_OK;
    printf("%-*s  %-7s  %-6s  %-*s  %-*s  %s\n", name_width, "NAME", "TYPE", "LISTEN", target_width, "TARGET",
           host_width, "HOST", "STATUS");

    for (size_t i = 0; i < config->count; i++) {
        const struct tunnel *tunnel = &config->tunnels[i];
        char target[TUNNEL_TEXT_MAX + 8];
        format_target(tunnel, target, sizeof(target));

        struct run_status status;
        char run_state[64];
        if (describe_run_state(state_dir_path, tunnel->name, &status, run_state, sizeof(run_state)) != 0) {
            exit_code = EXIT_RUNTIME_ERROR;
        }

        printf("%-*s  %-7s  %-6d  %-*s  %-*s  %s\n", name_width, tunnel->name, forward_type_name(tunnel->type),
               tunnel->listen_port, target_width, target, host_width, tunnel->host, run_state);
    }
    return exit_code;
}
