#pragma once

#include <stddef.h>
#include <sys/types.h>
#include <time.h>

enum run_state {
    STOPPED, // no pidfile
    RUNNING, // pidfile points to a live process of ours
    STALE,   // pidfile is garbage or its process is gone
};

struct run_status {
    enum run_state state;
    pid_t pid;    // only set when RUNNING
    time_t since; // when up wrote the pidfile; only set when RUNNING
};

int state_dir(char *out, size_t size);
int state_ensure_dir(const char *dir);
int state_log_path(const char *dir, const char *name, char *out, size_t size);

int state_check(const char *dir, const char *name, struct run_status *status);
int state_write_pidfile(const char *dir, const char *name, pid_t pid);
int state_remove_pidfile(const char *dir, const char *name);
