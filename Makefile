# LUA=$(HOME)/git/luajit-2.0/src/luajit
# LUAJIT_DIR=$(HOME)/git/luajit-2.0
# gnu11 for MAP_ANONYMOUS
CFLAGS=-W -Wall -O2 -std=gnu11 -DPT_WASM_DEBUG # -I$(LUAJIT_DIR)
APP=wasm
OBJS=pt-wasm.o main.o mod-tests.o func-tests.o

.PHONY=all clean

all: $(APP)

$(APP): $(OBJS)
	$(CC) -o $(APP) $(OBJS)

%.o: %.c pt-wasm.h
	$(CC) -c $(CFLAGS) $<

# bf.c: bf.c.dasm
# 	$(LUA) $(LUAJIT_DIR)/dynasm/dynasm.lua -D X64 -o bf.c bf.c.dasm

test: $(APP)
	./$(APP)

clean:
	$(RM) -f $(APP) $(OBJS)
