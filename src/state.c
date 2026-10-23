#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "errors.h"
#include "procinfo.h"
#include "state.h"

static int state_file_path(const char *dir, const char *name, const char *extension, char *out, size_t size) {
    int written = snprintf(out, size, "%s/%s.%s", dir, name, extension);
    if (written < 0 || (size_t)written >= size) {
        print_error("state path for '%s' is too long", name);
        return -1;
    }
    return 0;
}

static int pidfile_path(const char *dir, const char *name, char *out, size_t size) {
    return state_file_path(dir, name, "pid", out, size);
}

// pid 0 and negative pids mean process groups to kill(), and 1 is init, so none of them can be our ssh.
static int parse_pid(char *text, pid_t *pid) {
    text[strcspn(text, "\n")] = '\0';

    char *end;
    errno = 0;
    long value = strtol(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0' || value <= 1 || value > INT_MAX) {
        return -1;
    }
    *pid = (pid_t)value;
    return 0;
}

int state_dir(char *out, size_t size) {
    const char *home = getenv("HOME");
    if (home == NULL || home[0] == '\0') {
        print_error("HOME is not set");
        return -1;
    }

    int written = snprintf(out, size, "%s/.tunnel", home);
    if (written < 0 || (size_t)written >= size) {
        print_error("HOME is too long");
        return -1;
    }
    return 0;
}

int state_ensure_dir(const char *dir) {
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        print_error("cannot create '%s': %s", dir, strerror(errno));
        return -1;
    }
    return 0;
}

int state_log_path(const char *dir, const char *name, char *out, size_t size) {
    return state_file_path(dir, name, "log", out, size);
}

// Written to a temporary file and renamed, so a concurrent ls never reads a half-written pidfile.
int state_write_pidfile(const char *dir, const char *name, pid_t pid) {
    char path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (pidfile_path(dir, name, path, sizeof(path)) != 0 ||
        state_file_path(dir, name, "pid.tmp", temp_path, sizeof(temp_path)) != 0) {
        return -1;
    }

    FILE *file = fopen(temp_path, "w");
    if (file == NULL) {
        print_error("cannot create '%s': %s", temp_path, strerror(errno));
        return -1;
    }
    bool written = fprintf(file, "%d\n", (int)pid) > 0;
    if (fclose(file) != 0) {
        written = false;
    }

    if (!written || rename(temp_path, path) != 0) {
        print_error("cannot write '%s': %s", path, strerror(errno));
        unlink(temp_path);
        return -1;
    }
    return 0;
}

int state_check(const char *dir, const char *name, struct run_status *status) {
    *status = (struct run_status){.state = STOPPED};

    char path[PATH_MAX];
    if (pidfile_path(dir, name, path, sizeof(path)) != 0) {
        return -1;
    }

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        if (errno == ENOENT) {
            return 0;
        }
        print_error("cannot open '%s': %s", path, strerror(errno));
        return -1;
    }

    struct stat info;
    time_t since = fstat(fileno(file), &info) == 0 ? info.st_mtime : 0;
    char text[32];
    bool has_text = fgets(text, sizeof(text), file) != NULL;
    fclose(file);

    pid_t pid;
    if (!has_text || parse_pid(text, &pid) != 0) {
        status->state = STALE;
        return 0;
    }

    if (kill(pid, 0) == 0) {
        // PIDs get reused: a live process with our pid may be some other program of ours.
        if (!proc_is_ssh(pid)) {
            status->state = STALE;
            return 0;
        }
        status->state = RUNNING;
        status->pid = pid;
        status->since = since;
        return 0;
    }
    // EPERM: the process exists but belongs to another user, so the pid was reused and it is not our ssh.
    if (errno == ESRCH || errno == EPERM) {
        status->state = STALE;
        return 0;
    }

    print_error("cannot check pid %d: %s", (int)pid, strerror(errno));
    return -1;
}

int state_remove_pidfile(const char *dir, const char *name) {
    char path[PATH_MAX];
    if (pidfile_path(dir, name, path, sizeof(path)) != 0) {
        return -1;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        print_error("cannot remove '%s': %s", path, strerror(errno));
        return -1;
    }
    return 0;
}
