#pragma once

#include <stdbool.h>
#include <sys/types.h>

// True only if pid is a running process whose executable is named "ssh".
bool proc_is_ssh(pid_t pid);
