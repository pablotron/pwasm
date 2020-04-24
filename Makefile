# LUA=$(HOME)/git/luajit-2.0/src/luajit
# LUAJIT_DIR=$(HOME)/git/luajit-2.0
# use -std=gnu11 for MAP_ANONYMOUS
CFLAGS=-W -Wall -Wextra -Werror -std=c11 -pedantic -g -pg -DPWASM_DEBUG # -I$(LUAJIT_DIR)
LIBS=-lm
APP=pwasm
OBJS=pwasm.o tests/main.o tests/mod-tests.o tests/func-tests.o

.PHONY=all clean

all: $(APP)

$(APP): $(OBJS)
	$(CC) -o $(APP) $(OBJS) $(LIBS)

%.o: %.c pwasm.h
	$(CC) -c -o $@ $(CFLAGS) $<

# bf.c: bf.c.dasm
# 	$(LUA) $(LUAJIT_DIR)/dynasm/dynasm.lua -D X64 -o bf.c bf.c.dasm

test: $(APP)
	./$(APP)

clean:
	$(RM) -f $(APP) $(OBJS)
