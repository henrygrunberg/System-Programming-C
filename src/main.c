#include <stdio.h>

#include "commands.h"
#include "config.h"
#include "errors.h"

// The command line is described with the macros of lib/cli.h
#define TUNNEL_NAME_ARGUMENT                                                                                           \
    CLI_ARG_POSITIONAL(char *, name, cli_parse_text, .label = "NAME", .required = true,                                \
                       .description = "Tunnel name from the config")

#define CLI_ARGUMENTS                                                                                                  \
    CLI_ARG_TEXT(config_path, .short_name = 'c', .long_name = "config", .label = "FILE",                               \
                 .description = "Config file (default: ~/.tunnel/tunnels.conf)")

#define CLI_COMMANDS                                                                                                   \
    CLI_COMMAND(up, "up", "Start a tunnel in the background", TUNNEL_NAME_ARGUMENT)                                    \
    CLI_COMMAND(down, "down", "Stop a tunnel", TUNNEL_NAME_ARGUMENT)                                                   \
    CLI_COMMAND(status, "status", "Show details of a tunnel", TUNNEL_NAME_ARGUMENT)                                    \
    CLI_COMMAND(ls, "ls", "List configured tunnels and their state", )

#define CLI_IMPLEMENTATION
#include "../lib/cli.h"

static const char *tunnel_name(const Cli_ParsedArgs *args) {
    switch (args->command) {
        case cli_command_up: return args->commands.up.name;
        case cli_command_down: return args->commands.down.name;
        case cli_command_status: return args->commands.status.name;
        case cli_command_ls:
        case cli_command_none: return NULL;
    }
    return NULL;
}

static int run_command(const Cli_ParsedArgs *args, const struct config *config, const struct tunnel *tunnel) {
    switch (args->command) {
        case cli_command_up: return cmd_up(tunnel);
        case cli_command_down: return cmd_down(tunnel);
        case cli_command_status: return cmd_status(tunnel);
        case cli_command_ls: return cmd_ls(config);
        case cli_command_none: break; // cli_parse only succeeds with a command
    }
    return EXIT_OK;
}

int main(int argc, char *argv[]) {
    Cli_ParsedArgs args;
    Cli_ParseError error;
    switch (cli_parse(argc, argv, &args, NULL, &error)) {
        case CLI_OK: break;

        case CLI_HELP:
            cli_print_usage(stdout, "tunnel", &args);
            cli_print_help(stdout, &args);
            return EXIT_OK;

        case CLI_ERROR:
            if (error.input != NULL) {
                print_error("%s '%s'", error.message, error.input);
            } else {
                print_error("%s", error.message);
            }
            fprintf(stderr, "Try 'tunnel --help' for more information.\n");
            return EXIT_USAGE_ERROR;
    }

    struct config config;
    if (config_load(args.config_path, &config) != 0) {
        return EXIT_CONFIG_ERROR;
    }

    int exit_code;
    const char *name = tunnel_name(&args);
    const struct tunnel *tunnel = name != NULL ? config_find(&config, name) : NULL;
    if (name != NULL && tunnel == NULL) {
        print_error("unknown tunnel '%s' (not in %s)", name, config.path);
        exit_code = EXIT_USAGE_ERROR;
    } else {
        exit_code = run_command(&args, &config, tunnel);
    }

    config_free(&config);
    return exit_code;
}
