# pwasm TODO

## TODO

Each entry in this list is prefixed with category tags.

See the "Tag Definitions" section below for a brief description of each
tag.

* [ ] code, test: fix start
* [ ] code, test: test `br` to outermost block (according to `return` documentation this should work)
* [ ] code, test: add invalid code tests (e.g. checker assertion tests)
* [ ] test: add tests for all opcodes
* [ ] test: add tests for all module sections
* [ ] code, test: fix memory leaks on parse/validation/exec errors
* [ ] code: remove redundant validation checks in interp/env calls
* [ ] code, jit: add jit (`dynasm`?)
* [ ] code, jit: add jit modes (lazy, aot, optimize, etc)
* [ ] doc: add internal documentation
* [ ] code, cleanup: consider calloc() for memory init (zero mem, bounds check)
* [ ] code, test: check uses of realloc() for overflow (security)
* [ ] cli, cleanup: properly escape values in CSV columns
* [ ] cli: add `dump` command (e.g. `objdump` to JSON)
* [ ] build: add meson support
* [ ] build, test: add clang static analysis support
* [ ] build: build dynamic library
* [ ] ci, test: run full test suite on push
* [ ] ci, test: build on all compilers on push
* [ ] ci, test: run static analysis passes on push
* [ ] ci, web: regenerate user guides for all tags and master on push (e.g. `pablotron.github.io/pwasm/$TAG/docs/guide/`, `docs.pwasm.org/$TAG/guide/`, etc)
* [ ] ci, web: regenerate api docs for all tags and master on push (e.g. `pablotron.github.io/pwasm/$TAG/docs/api/`, `docs.pwasm.org/$TAG/api/`, etc)
* [ ] ci, web: regenerate sites for all tags and master on push
* [ ] code, cli: add `java` and `c` commands?
* [ ] code, cli: `wat`: fix alignment
* [ ] test: add wat2wasm round-trip tests
* [ ] code: add `uint64_t pwasm_platform_get_value()` (e.g. compile-time limits, flags, etc)

## In Progress
* [ ] code: add global, table, and mem init, call start func (added,
      but untested)
* [ ] code, test: add v128 tests

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
* [x] code, cli: add `func` command to show parameters and results
* [x] cli: add `imports` command (list module imports as CSV)
* [x] doc: add full API documentation (`doxygen`)
* [x] code, check: add global, elem, and segment expr validation
* [x] code, test: add `br_table` test
* [x] code, test: add import test (added `data/wat/06-imports.wasm`)
* [x] code, test: add table tests (added `08-call_indirect.wasm`)
* [x] code, check: check `call_indirect` at invocation
* [x] code, check: add control stack validation (added `pwasm_checker_t`)
* [x] code, cleanup: remove old interpreter
* [x] code, check: merge `insts` into  `stack` tests
* [x] code, parse: cache control target offsets at parse time
* [x] code, ops: add `trunc_sat` (`0xFC`) ops (added stub parser)
* [x] code, ops: add `simd` (`0xFD`) ops (added stub parser)
* [x] code, ops: add `trunc_sat` type checks
* [x] code, ops: add `simd` type checks
* [x] code, ops: add `trunc_sat` interpreter impls
* [x] code, ops: add `simd` interpreter impls
* [x] code: regen simd enum/data
* [x] code, test: fix `get_table`

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
