#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "errors.h"
#include "spawn.h"

#define POLL_INTERVAL_MS 100
#define TERM_TIMEOUT_MS 3000
#define KILL_TIMEOUT_MS 1000

static void sleep_ms(int milliseconds) {
    struct timespec delay = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    nanosleep(&delay, NULL);
}

static void format_forward(const struct tunnel *tunnel, char *out, size_t size) {
    if (tunnel->type == DYNAMIC) {
        snprintf(out, size, "%d", tunnel->listen_port);
    } else {
        snprintf(out, size, "%d:%s:%d", tunnel->listen_port, tunnel->target_host, tunnel->target_port);
    }
}

// Runs in the forked child and never returns. It uses _exit, not exit, so the
// parent's unflushed stdio buffers are not written a second time.
_Noreturn static void run_child(int log_fd, char *const argv[]) {
    // A new session has no controlling terminal, so Ctrl-C or closing the terminal does not reach ssh.
    if (setsid() < 0) {
        dprintf(log_fd, "tunnel: setsid failed: %s\n", strerror(errno));
        _exit(127);
    }

    int null_fd = open("/dev/null", O_RDONLY);
    if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 || dup2(log_fd, STDOUT_FILENO) < 0 ||
        dup2(log_fd, STDERR_FILENO) < 0) {
        dprintf(log_fd, "tunnel: cannot redirect ssh's input and output: %s\n", strerror(errno));
        _exit(127);
    }
    if (null_fd > STDERR_FILENO) {
        close(null_fd);
    }
    if (log_fd > STDERR_FILENO) {
        close(log_fd);
    }

    execvp(argv[0], argv);
    fprintf(stderr, "tunnel: cannot run %s: %s\n", argv[0], strerror(errno));
    _exit(127);
}

int spawn_tunnel(const struct tunnel *tunnel, const char *log_path, pid_t *pid) {
    char forward[TUNNEL_TEXT_MAX + 16];
    format_forward(tunnel, forward, sizeof(forward));

    static const char *const forward_flags[] = {
        [LOCAL] = "-L",
        [REMOTE] = "-R",
        [DYNAMIC] = "-D",
    };

    // execvp takes char *const[] for historical reasons; it never modifies the strings, so the casts are safe.
    char *const argv[] = {
        "ssh",
        "-N",
        (char *)forward_flags[tunnel->type],
        forward,
        "-o",
        "ExitOnForwardFailure=yes",
        "-o",
        "BatchMode=yes",
        "-o",
        "ConnectTimeout=5",
        (char *)tunnel->host,
        NULL,
    };

    int log_fd = open(log_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (log_fd < 0) {
        print_error("cannot open log '%s': %s", log_path, strerror(errno));
        return -1;
    }

    pid_t child = fork();
    if (child < 0) {
        print_error("cannot start ssh: fork failed: %s", strerror(errno));
        close(log_fd);
        return -1;
    }
    if (child == 0) {
        run_child(log_fd, argv);
    }

    close(log_fd);
    *pid = child;
    return 0;
}

bool port_accepts_connections(int port) {
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) {
        return false;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    bool accepted = connect(socket_fd, (struct sockaddr *)&address, sizeof(address)) == 0;
    close(socket_fd);
    return accepted;
}

int wait_for_startup(pid_t pid, int ready_port, int timeout_ms, int *wait_status) {
    for (int waited = 0;; waited += POLL_INTERVAL_MS) {
        pid_t result = waitpid(pid, wait_status, WNOHANG);
        if (result == pid) {
            return 1;
        }
        if (result < 0) {
            print_error("cannot wait for pid %d: %s", (int)pid, strerror(errno));
            return -1;
        }
        // ssh only opens the local port after the login worked, so an open port means the tunnel is up.
        if (waited >= timeout_ms || (ready_port != 0 && port_accepts_connections(ready_port))) {
            return 0;
        }
        sleep_ms(POLL_INTERVAL_MS);
    }
}

static bool is_gone(pid_t pid) {
    return kill(pid, 0) != 0 && errno == ESRCH;
}

// The process is not our child (an earlier `tunnel up` started it), so waitpid cannot be used; poll instead.
static bool wait_until_gone(pid_t pid, int timeout_ms) {
    for (int waited = 0; waited < timeout_ms; waited += POLL_INTERVAL_MS) {
        if (is_gone(pid)) {
            return true;
        }
        sleep_ms(POLL_INTERVAL_MS);
    }
    return is_gone(pid);
}

int stop_process(pid_t pid) {
    if (kill(pid, SIGTERM) != 0) {
        if (errno == ESRCH) {
            return 0;
        }
        print_error("cannot stop pid %d: %s", (int)pid, strerror(errno));
        return -1;
    }
    if (wait_until_gone(pid, TERM_TIMEOUT_MS)) {
        return 0;
    }

    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        print_error("cannot kill pid %d: %s", (int)pid, strerror(errno));
        return -1;
    }
    if (wait_until_gone(pid, KILL_TIMEOUT_MS)) {
        return 0;
    }

    print_error("pid %d is still running after SIGKILL", (int)pid);
    return -1;
}
