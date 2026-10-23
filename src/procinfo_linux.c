#include <stdio.h>
#include <string.h>

#include "procinfo.h"

// /proc/<pid>/comm holds the executable's name (up to 15 characters) and, unlike /proc/<pid>/exe,
// stays correct when the ssh binary is replaced by a package update while the tunnel runs.
bool proc_is_ssh(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    char name[32];
    bool has_name = fgets(name, sizeof(name), file) != NULL;
    fclose(file);
    if (!has_name) {
        return false;
    }

    name[strcspn(name, "\n")] = '\0';
    return strcmp(name, "ssh") == 0;
}
