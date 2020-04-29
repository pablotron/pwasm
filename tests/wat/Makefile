WASMS=00-module.wasm 01-fib.wasm 02-vec.wasm 03-mem.wasm

.PHONY=all clean

all: $(WASMS)

%.wasm: %.wat
	wat2wasm -o $@ $<

clean:
	$(RM) -f $(WASMS)
