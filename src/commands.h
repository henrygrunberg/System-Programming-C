#pragma once

#include "config.h"

enum exit_code {
    EXIT_OK = 0,
    EXIT_RUNTIME_ERROR = 1,
    EXIT_USAGE_ERROR = 2,
    EXIT_CONFIG_ERROR = 3,
};

int cmd_up(const struct tunnel *tunnel);
int cmd_down(const struct tunnel *tunnel);
int cmd_status(const struct tunnel *tunnel);
int cmd_ls(const struct config *config);
