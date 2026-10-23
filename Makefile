CC = cc
CFLAGS = -std=gnu17 -Wall -Wextra -Werror -g

# The PID check is the only platform-specific code, so pick the file that matches this system.
SYSTEM = $(shell uname -s)
ifeq ($(SYSTEM),Darwin)
PROCINFO = procinfo_macos
else ifeq ($(SYSTEM),Linux)
PROCINFO = procinfo_linux
else
$(error tunnel only supports Linux and macOS, not $(SYSTEM))
endif

OBJECTS = build/main.o build/commands.o build/config.o build/errors.o build/spawn.o build/state.o build/$(PROCINFO).o

build/tunnel: $(OBJECTS)
	$(CC) $(CFLAGS) -o build/tunnel $(OBJECTS)

build/main.o: src/main.c src/commands.h src/config.h src/errors.h lib/cli.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/main.c -o build/main.o

build/commands.o: src/commands.c src/commands.h src/config.h src/errors.h src/spawn.h src/state.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/commands.c -o build/commands.o

build/config.o: src/config.c src/config.h src/errors.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/config.c -o build/config.o

build/errors.o: src/errors.c src/errors.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/errors.c -o build/errors.o

build/spawn.o: src/spawn.c src/spawn.h src/config.h src/errors.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/spawn.c -o build/spawn.o

build/state.o: src/state.c src/state.h src/errors.h src/procinfo.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/state.c -o build/state.o

build/procinfo_macos.o: src/procinfo_macos.c src/procinfo.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/procinfo_macos.c -o build/procinfo_macos.o

build/procinfo_linux.o: src/procinfo_linux.c src/procinfo.h
	mkdir -p build
	$(CC) $(CFLAGS) -c src/procinfo_linux.c -o build/procinfo_linux.o

clean:
	rm -rf build

.PHONY: clean
