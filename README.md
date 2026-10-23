# tunnel

A small command-line tool for SSH tunnels. You describe your tunnels once in a config file, then start, stop and check them by name.

```sh
tunnel up db        # start the tunnel called "db" in the background
tunnel ls           # list all tunnels and whether they are running
tunnel status db    # details and the last lines of ssh's output
tunnel down db      # stop it
```

`tunnel` does not implement SSH itself. It starts the normal `ssh` program in the background, remembers its process ID and stops it again when you ask.

## Building

You need a C compiler, `make` and `ssh`.

```sh
make                          # builds build/tunnel
cp build/tunnel ~/.local/bin/ # optional: any folder on your PATH works
```

`make clean` removes the build output.

## Usage

```
tunnel [-c FILE] up NAME        start a tunnel in the background
tunnel [-c FILE] down NAME      stop it
tunnel [-c FILE] status NAME    show details of one tunnel
tunnel [-c FILE] ls             list all tunnels
tunnel --help                   help (also: tunnel up --help)
```

`-c FILE` uses a different config file instead of the default one.

```
$ tunnel up db
db: up (pid 48211) localhost:5432 -> user@myserver -> localhost:5432

$ tunnel ls
NAME     TYPE     LISTEN  TARGET          HOST           STATUS
db       local    5432    localhost:5432  user@myserver  up (pid 48211)
grafana  local    3000    localhost:3000  user@monitor   down
```

`up` waits until the tunnel actually works, meaning ssh has logged in and opened the local port. If ssh fails instead (wrong host, refused login, port already taken, or no answer within 5 seconds), you get the reason and the last lines of ssh's output.

Exit codes: `0` success, `1` runtime error (for example ssh failed), `2` wrong usage or unknown tunnel name, `3` problem with the config file. Errors go to stderr.

## Configuration

The config lives in `~/.tunnel/tunnels.conf`, and `tunnels.conf.example` in this repository is an example to start from. Each tunnel is a section:

```ini
[db]
host   = user@myserver
local  = 5432
remote = localhost:5432
```

`host` is anything you could pass to `ssh`, including a `Host` alias from `~/.ssh/config`. The optional `type` is `local` (the default, `ssh -L`), `remote` (`ssh -R`) or `dynamic` (`ssh -D`). Mistakes in the file are reported with their line number.

SSH keys work as usual through `~/.ssh` and ssh-agent. Keys with a passphrase have to be loaded with `ssh-add` first, because the tunnel runs in the background and cannot ask for it.

## How it works

`tunnel up` starts `ssh` as a background process detached from the terminal and saves its process ID in `~/.tunnel/<name>.pid`, with ssh's output going to `~/.tunnel/<name>.log`. The other commands use that pidfile to find the process again, check that it is still a running `ssh`, and stop it. The check for "is this process ssh" is the only platform-specific code, with one version for macOS and one for Linux.

## Supported platforms

Tested on:

- macOS 26.6 (Apple Silicon), Apple clang 17
- Linux: Debian 13 "trixie" (x86_64), GCC 14

Windows is not supported.

## Credits

Command-line parsing uses `lib/cli.h`, a small library written by a fellow student, under the MIT license (see `lib/LICENSE`).

I used Claude Code as a helping tool when planning and writing this project.
