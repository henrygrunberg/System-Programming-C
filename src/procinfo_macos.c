#include <libproc.h>
#include <string.h>

#include "procinfo.h"

bool proc_is_ssh(pid_t pid) {
    char path[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(pid, path, sizeof(path)) <= 0) {
        return false;
    }

    const char *slash = strrchr(path, '/');
    const char *name = slash != NULL ? slash + 1 : path;
    return strcmp(name, "ssh") == 0;
}
