# pwasm TODO

## TODO

Each entry in this list is prefixed with category tags.

See the "Tag Definitions" section below for a brief description of each
tag.

* [ ] code, cli: `wat`: fix alignment
* [ ] code, test: add table tests
* [ ] code, test: add import test
* [ ] code, test: add wat2wasm round-trip tests
* [ ] code, check: add control stack validation (re-add)
* [ ] code, check: add global, elem, and segment expr validation
* [ ] code, check: check `call_immediate` at invocation
* [ ] code, parse: cache control target offsets at parse time
* [ ] code, ops: add extended opcode support (`0xFC ...`)
* [ ] code, ops: add vector op support (`0xFD ...`)
* [ ] code, jit: add jit (`dynasm`?)
* [ ] code, jit: add jit modes (lazy, aot, optimize, etc)
* [ ] code: add `uint64_t pwasm_platform_get_value()` (e.g. compile-time limits, flags, etc)
* [ ] code, cleanup: remove old interpreter
* [ ] code, cleanup: consider calloc() for memory init (zero mem, bounds check)
* [ ] code, test: check uses of realloc() for overflow (security)
* [ ] cli: add `dump` command (e.g. `objdump` to JSON)
* [ ] cli: add `imports` command (list module imports as CSV)
* [ ] test: add tests for all opcodes
* [ ] test: add tests for all module sections
* [ ] build: add meson support
* [ ] build, test: add clang static analysis support
* [ ] build: build dynamic library
* [ ] doc: add full API documentation (`doxygen`?)
* [ ] ci, test: run full test suite on push
* [ ] ci, test: build on compilers on push
* [ ] ci, test: run static analysis passes on push
* [ ] ci, web: regenerate user guides for all tags and master on push (e.g. `pablotron.github.io/pwasm/$TAG/docs/guide/`, `docs.pwasm.org/$TAG/guide/`, etc)
* [ ] ci, web: regenerate api docs for all tags and master on push (e.g. `pablotron.github.io/pwasm/$TAG/docs/api/`, `docs.pwasm.org/$TAG/api/`, etc)
* [ ] ci, web: regenerate sites for all tags and master on push

## In Progress
* [ ] code: add global, table, and mem init, call start func (added,
      but untested)

## Done

Items in this section have been completed.

* [x] refactor examples (added `examples/`)
* [x] add interpreter
* [x] add generic vec(u32) (added `u32s` to `pwasm_mod_t` and builder)
* [x] add function code parsing (done)
* [x] limit elements to const ops (and globals, in `mod_check_const_expr`)
* [x] add generic vector handling (adde `pwasm_vec_t` from ecs)
* [x] limit globals to const ops (added `parse_const_expr`)
* [x] code, interpreter: refactor interpreter to add top-level vectors
  for the following: `u32s`, `mods`, `funcs`, `globals`, `tables`,
  `mems`, and then make the add comparable slices in mods (plus
  `exports` and maybe `imports`) which index into the interpreter
  `u32s` vector.  `mods` and `funcs` should be `union`s to distinguish
  between native and internal.
* [x] code, test: test new interpreter
* [x] code, test: add rest of memory get/set test (`tests/wat/03-mem.wat`)
* [x] code, test: add global get/set test (`tests/wat/04-global.wat`)
* [x] cli: add command line tool (added `cli/`)
* [x] cli, doc: add `help` command (added `cli/cmds/help.c`)
* [x] cli, test: add `test` command (e.g. `test foo-bar`, `test all`,
  etc) (added `cli/cmds/test.c`)
* [x] test: refactor tests (added `cli/tests/`)
* [x] code, cli: `wat`: add `tables`
* [x] code, cli: `wat`: add `start`
* [x] code, cli: `wat`: add `elem`
* [x] code, cli: `wat`: add `data`
* [x] code, cli: `wat`: add `br_table` immediate
* [x] code, cli: `wat`: add `call_indirect` immediate
* [x] code, cli: `wat`: support `if`, `block`, `loop`, etc
* [x] cli: add `wat` command (added `cli/cmd/wat.c`)
* [x] build, test: add test suite (under `cli/tests/*.c`)
* [x] doc: add user guide (added `mkdocs` with `mkdocs-material` theme)
* [x] cli: add `exports` command (list module exports as CSV)

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
