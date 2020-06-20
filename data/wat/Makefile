WASMS=00-module.wasm 01-fib.wasm 02-vec.wasm 03-mem.wasm \
      04-global.wasm 06-imports.wasm 07-br_table.wasm \
      08-call_indirect.wasm 09-life.wasm 10-start.wasm \
      12-v128-const.wasm 13-ops.wasm 14-i64-const.wasm \
      15-multi.wasm 16-mem-init.wasm 17-aot.wasm

.PHONY=all clean

all: $(WASMS)

%.wasm: %.wat
	wat2wasm --enable-all -o $@ $<

clean:
	$(RM) -f $(WASMS)
