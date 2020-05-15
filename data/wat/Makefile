WASMS=00-module.wasm 01-fib.wasm 02-vec.wasm 03-mem.wasm \
      04-global.wasm 06-imports.wasm 07-br_table.wasm \
      08-call_indirect.wasm

.PHONY=all clean

all: $(WASMS)

%.wasm: %.wat
	wat2wasm -o $@ $<

clean:
	$(RM) -f $(WASMS)
