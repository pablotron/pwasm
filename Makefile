# LUA=$(HOME)/git/luajit-2.0/src/luajit
# LUAJIT_DIR=$(HOME)/git/luajit-2.0
# use -std=gnu11 for MAP_ANONYMOUS
# CFLAGS=... -std=gnu11 -I$(LUAJIT_DIR)
CFLAGS=-W -Wall -Wextra -Werror -std=c11 -pedantic -g -pg -DPWASM_DEBUG
LIBS=-lm
APP=pwasm
# OBJS=pwasm.o tests/main.o tests/mod-tests.o tests/func-tests.o
OBJS=pwasm.o cli/main.o cli/cli-cmds.o cli/cli-tests.o \
     cli/cmd-help.o cli/cmd-test.o cli/test-init.o \
     cli/test-native.o cli/test-wasm.o cli/test-cli.o

.PHONY=all clean

all: $(APP)

$(APP): $(OBJS)
	$(CC) -o $(APP) $(OBJS) $(LIBS)

%.o: %.c pwasm.h
	$(CC) -c -o $@ $(CFLAGS) $<

test: $(APP)
	./$(APP)

clean:
	$(RM) -f $(APP) $(OBJS)
