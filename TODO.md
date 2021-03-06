# pwasm TODO

## TODO

Each entry in this list is prefixed with category tags.

See the "Tag Definitions" section below for a brief description of each
tag.

* [ ] code: add `get_handle` to replace `get_*_index` env cbs
* [ ] code, test: add tests with random values for math ops
* [ ] code: cache 0xFF... in xmm7 (e.g. pcmpeqd xmm7, xmm7) for negating
      v128 ops
* [ ] code: count `call` and `call_indirect` to elide prologue/epilogue
* [ ] code, test: check jit functions to make sure they are supported
      by cpuflags, and add fallback implementations
* [ ] code, test: loop with params
* [ ] code, test: test single `mem_id` (and remove aot-jit `mem_id`
      hack)
* [ ] code, test: check `call_immediate` table ID in validation layer
      instead of parser
* [ ] code, test: check stack size at start of call
* [ ] code, test: add invalid code tests (e.g. checker assertion tests)
* [ ] code, test: unify testing code in `cli/tests/{wasm,compile.c}`
* [ ] code, test: fix memory leaks on parse/validation/exec errors
* [ ] code: remove redundant validation checks in interp/env calls
* [ ] code, jit: add jit modes (lazy, optimize, etc)
* [ ] doc: add internal documentation
* [ ] doc, test: document v128 `avgr_u` rounding
* [ ] doc, test: investigate/document rounding mode for `f32/f64.div` (fenv)
* [ ] code, test: confirm `roundss` rounding modes (used for
      `f32.{floor,ceil,trunc,nearest}` insts)
* [ ] code, cleanup: consider calloc() for memory init (zero mem, bounds check)
* [ ] code, test: check uses of realloc() for overflow (security)
* [ ] cli, cleanup: properly escape values in CSV columns
* [ ] cli: add `dump` command (e.g. `objdump` to JSON)
* [ ] build: add meson support
* [ ] build, test: add clang static analysis support
* [ ] build, test: add valgrind memory tests
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
* [ ] doc: document JIT and `pwasm-dynasm-jit.c` build (`docs/jit.md`
  and `examples/01-jit.c`)

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
* [x] code, test: fix start
* [x] code: add global, table, and mem init, call start func
* [x] test: add tests for all module sections
* [x] code, test: add main opcode tests (`ops.wasm` in `tests/wasm.c`)
* [x] code, test: add v128 opcode tests
* [x] add {i32,i64,f32,f64}.const tests
* [x] code: support multi-result blocks
* [x] code, test: test `br` to outermost block (according to `return` documentation this should work)
* [x] code: remove `PWASM_RESULT_TYPE_*` (moved to `cli/result-type.h`)
* [x] code, test: check multi-parameter blocks (`data/wat/15-multi.wat`)
* [x] code: implement `pwasm_new_interp_init_segments()`
* [x] code, test: add tests to verify local indices bounds checks, and
      remove them from `eval_expr`
* [x] code: remove fixed-size stack in `eval_expr` (added
  `pwasm_ctrl_stack_t`)
* [x] code: switch compile function to `compiler_t`, and do cpuid checks
      in `compiler_init`
* [x] code, jit: add jit (added dynasm sysv x86-64 JIT)

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
