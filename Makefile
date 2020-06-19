# note: using -std=gnu11 for MAP_ANONYMOUS

# used to generate pwasm-compile.c
DYNASM=dynasm
LUAJIT_DIR=$(HOME)/git/luajit-2.0

# release
# CFLAGS=-W -Wall -Wextra -Werror -std=gnu11 -pedantic -O3
# LIBS=-lm

# debug
CFLAGS=-W -Wall -Wextra -Werror -fPIC -std=gnu11 -pedantic -g -pg -DPWASM_DEBUG
LIBS=-lm

# asan
# https://clang.llvm.org/docs/AddressSanitizer.html
# run with: LD_PRELOAD=/lib/x86_64-linux-gnu/libasan.so.5 ./pwasm test
# CC=clang
# CFLAGS=-W -Wall -Wextra -Werror -std=gnu11 -pedantic -g -pg -O1 -fsanitize=address -DPWASM_DEBUG
# LIBS=-lm -lasan

# ubsan
# https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html
# CC=clang
# CFLAGS=-W -Wall -Wextra -Werror -std=gnu11 -pedantic -g -pg -fsanitize=undefined -DPWASM_DEBUG
# LIBS=-lm -lubsan

APP=pwasm
OBJS=pwasm.o pwasm-compile.o pwasm-dump.o \
     cli/main.o cli/cmds.o cli/tests.o cli/utils.o \
     cli/cmds/help.o cli/cmds/test.o cli/cmds/wat.o \
     cli/cmds/customs.o cli/cmds/cat.o cli/cmds/func.o \
     cli/cmds/imports.o cli/cmds/exports.o \
     cli/tests/init.o cli/tests/native.o cli/tests/wasm.o \
     cli/tests/compile.o cli/tests/cli.o cli/result-type.o

.PHONY=all clean

all: $(APP)

$(APP): $(OBJS)
	$(CC) -o $(APP) $(OBJS) $(LIBS)

%.o: %.c pwasm.h
	$(CC) -c -o $@ $(CFLAGS) $<

pwasm-compile.c: pwasm-compile.dasc
	$(DYNASM) -o pwasm-compile.c pwasm-compile.dasc

pwasm-compile.o: pwasm-compile.c pwasm.h
	$(CC) -c -o $@ $(CFLAGS) -I$(LUAJIT_DIR) $<

test: $(APP)
	./$(APP)

docs:
	mkdocs build

clean:
	$(RM) -f $(APP) $(OBJS)
