# you'll need to set this to build the second example
LUAJIT_DIR=$(HOME)/git/luajit-2.0

CFLAGS=-W -Wall -Wextra -Werror -fPIC -std=gnu11 -pedantic -O3 -I..
LIBS=-lm -ldl

INTERP_OBJS=pwasm.o
JIT_OBJS=pwasm.o pwasm-dynasm-jit.o

E00_APP=example-00-pythag
E00_OBJS=00-pythag.o

E01_APP=example-01-jit
E01_OBJS=01-jit.o

.PHONY=all clean

all: $(E00_APP) $(E01_APP)

$(E00_APP): $(E00_OBJS)
	$(CC) -o $(E00_APP) $(E00_OBJS) $(INTERP_OBJS) $(LIBS)

$(E01_APP): $(E01_OBJS)
	$(CC) -o $(E01_APP) $(E01_OBJS) $(JIT_OBJS) $(LIBS)

%.o: %.c
	$(CC) -c -o $@ $(CFLAGS) $<

pwasm.o: ../pwasm.c
	$(CC) -c -o $@ $(CFLAGS) $<

pwasm-dynasm-jit.o: ../pwasm-dynasm-jit.c
	$(CC) -c -o $@ $(CFLAGS) -I$(LUAJIT_DIR) $<

clean:
	$(RM) -f $(E00_APP) $(E00_OBJS) $(E01_APP) $(E01_OBJS)
