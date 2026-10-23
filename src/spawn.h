#pragma once

#include <stdbool.h>
#include <sys/types.h>

#include "config.h"

int spawn_tunnel(const struct tunnel *tunnel, const char *log_path, pid_t *pid);

// True if something on this machine listens on the port (checked by connecting to 127.0.0.1).
bool port_accepts_connections(int port);

// For our own child only. Waits until ready_port accepts connections (0: no port to check) or timeout_ms passed,
// then returns 0. Returns 1 and fills wait_status if the child exited before that.
int wait_for_startup(pid_t pid, int ready_port, int timeout_ms, int *wait_status);

// SIGTERM, then SIGKILL if the process is still alive a few seconds later.
int stop_process(pid_t pid);
