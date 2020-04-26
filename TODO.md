# pwasm TODO

## TODO

Each entry in this list is prefixed with category tags.

See the "Tag Definitions" section below for a brief description of each
tag.

* code, check: add control stack validation
* code, check: check `call_immediate` at invocation
* code, parse: cache control opcode targets at parse time
* code, ops: add extended opcode support (`0xFC ...`)
* code, ops: add vector op support (`0xFD ...`)
* code, jit: add jit (`dynasm`?)
* code, jit: add jit modes (lazy, aot, optimize, etc)
* code: add `uint64_t pwasm_platform_get_value()` (e.g. compile-time limits, flags, etc)
* cli: add command line tool
* cli, doc: add `help` command
* cli, test: add `test` command (e.g. `test foo-bar`, `test all`, etc)
* cli: add `dump` command (e.g. `objdump` to JSON)
* cli: add `disasm` command (dump to `wat`?)
* test: refactor tests (add `tests/`)
* test: add tests for all opcodes
* test: add tests for all module sections
* build: add meson support
* build, test: add test suite
* build, test: add clang static analysis support
* build: build dynamic library
* doc: add user guide (`docs/` via `mkdocs`?)
* doc: add full API documentation (`doxygen`?)
* ci, test: run full test suite on push
* ci, test: build on compilers on push
* ci, test: run static analysis passes on push
* ci, web: regenerate user guides for all tags and master on push (e.g. `pablotron.github.io/pwasm/$TAG/docs/guide/`, `docs.pwasm.org/$TAG/guide/`, etc)
* ci, web: regenerate api docs for all tags and master on push (e.g. `pablotron.github.io/pwasm/$TAG/docs/api/`, `docs.pwasm.org/$TAG/api/`, etc)
* ci, web: regenerate sites for all tags and master on push

## Done

Items in this section have been completed.

* refactor examples (added `examples/`)
* add interpreter (mostly done)
* add generic vec(u32) (added `u32s` to `pwasm_mod_t` and builder)
* add function code parsing (done)
* limit elements to const ops (and globals, in `mod_check_const_expr`)
* add generic vector handling (adde `pwasm_vec_t` from ecs)
* limit globals to const ops (added `parse_const_expr`)

## Tag Definitions

Rough definition of tags in the previous sections.

* `code`: code
* `check`: validation code (e.g. `pwasm_mod_check()`)
* `parse`: parser code (e.g. `pwasm_mod_init_unsafe()`)
* `ops`: opcode support code
* `jit`: JIT support
* `cli`: `pwasm` command-line tool
* `doc`: documentation
* `test`: testing
* `build`: build
* `ci`: continuous integration work
* `web`: web site(s)
