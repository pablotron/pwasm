# PWASM Command

## Overview

The `pwasm` command lets you inspect [WebAssembly][] modules.

Use the [pwasm help](#pwasm-help) command to see a complete list of
available actions:

```
> pwasm help
Usage:
  pwasm <command> [args]

Module Commands:
  cat: Extract data for a custom section from a WASM file.
  customs: List custom sections in a WASM file.
  exports: List exports in a WASM file.
  func: Show parameters and results for an exported function.
  imports: List imports in a WASM file.
  wat: Convert one or more WASM files to WAT files.

Other Commands:
  help: Show help.
  test: Run tests.

Use "help <command>" for more details on a specific command.
```

## Features

The `pwasm` tool can:

* Disassemble modules into [WebAssembly Text (WAT)][wat] files.
* List the imports and exports in a module file.
* Extract the contents of a custom section from a module file.
* Run the built-in test suite.

## Module Commands

The commands in this section display information about [WebAssembly][]
module files.

### `pwasm cat`

#### Description

The `pwasm cat` command extracts the data for a custom section from a
module file and prints the data to standard output.

**Note:** The contents of custom sections are typically binary rather
than terminal-friendly text, so you may want to write them to a file
rather than displaying them on standard output like the example below.

#### Example

This example uses the `customs` command to list the ID and name of each
custom section in the [WebAssembly][] module stored in the file
`05-custom.wasm`, and then uses the `cat` command to extract the data
for the custom section with an ID of `0`.

```
> pwasm customs 05-custom.wasm
id,name
0,"hello"
> pwasm cat 05-custom.wasm 0
there
```

### `pwasm customs`

#### Description

The `pwasm customs` command prints the custom sections in a module to
standard output in [CSV][] format.

#### Example

This example uses the `customs` command to list the ID and name of each
custom section in the [WebAssembly][] module stored in the file
`05-custom.wasm`, and then uses the `cat` command to extract the data
for the custom section with an ID of `0`.

```
> pwasm customs 05-custom.wasm
id,name
0,"hello"
> pwasm cat 05-custom.wasm 0
there
```

### `pwasm exports`

#### Description

The `pwasm exports` command prints the exports in a module to standard
output in [CSV][] format.

An *export* is a module component that can be accessed by [PWASM][] or
imported into another module.  For example, exported functions can be
called by [PWASM][] using the `pwasm_call()` function.

Each available export type is described in the table below.

|Type|Name|Description|
|----|----|-----------|
|`func`|Function|Exported functions can be called by [PWASM][] using the `pwasm_call()` function.|
|`global`|Global Variable|Exported global variables can be read and written by [PWASM][] using the `pwasm_get_global()` and `pwasm_set_global()` functions, respectively.|
|`memory`|Memory|Exported memory blocks can be accessed by [PWASM][] via the `pwasm_get_mem()` function.|
|`table`|Table|Tables contain function references and are used by the `call_indirect` opcode in [WebAssembly][] modules.|

#### Example

This example uses the `exports` command to list the exports in the
[WebAssembly][] module file `01-fib.wasm`.

```
> pwasm exports 01-fib.wasm
type,name
func,"fib_recurse"
func,"fib_iterate"
```

### `pwasm func`

#### Description

The `pwasm func` command prints the parameters and results of an
exported function in a module to standard output in [CSV][] format.

The columns of the [CSV][] are:

* `function`: The name of the function.
* `row type`: Row type.  One of: `param` or `result`.
* `sort`: The sort order of the entry.
* `value type`: The type of parameter or result.  One of: `i32`, `i64`, `f32`, or `f64`.

#### Example

This example uses the `func` command to list the parameters and results
of the exported function `v2.store` in the [WebAssembly][] module file
`02-vec.wasm`.

```
> pwasm func 02-vec.wasm v2.store
function name,row type,sort,value type
"v2.store",param,0,i32
"v2.store",param,1,f32
"v2.store",param,2,f32
"v2.store",result,0,i32
```

### `pwasm imports`

#### Description

The `pwasm imports` command prints the imports in a module to standard
output in [CSV][] format.

An *import* is a component that a [WebAssembly][] module needs in order
to function.

Each available import type is described in the table below.

|Type|Name|Description|
|----|----|-----------|
|`func`|Function|A function that is exported by a another module.|
|`global`|Global Variable|A global variable that is exported by another module.|
|`memory`|Memory|A memory component that is exported by another module.|
|`table`|Table|A table component that is exported by another module.|

#### Example

This example uses the `imports` command to list the exports in the
[WebAssembly][] module file `06-imports.wasm`.

```
> pwasm imports 06-imports.wasm
type,module,name
func,"trek","kirk"
func,"trek","picard"
func,"trek","sisko"
func,"trek","janeway"
func,"trek","archer"
```

### `pwasm wat`

#### Description

The `pwasm wat` command converts a [WebAssmebly][] module file to
[WebAssembly Text (WAT)][] format and prints the result to standard
output.

#### Example

This example uses the `wat` command to convert a [WebAssembly][] module
file `03-mem.wasm` to [WebAssembly text (WAT)][wat] format.

```
> pwasm wat 03-mem.wasm
(module
  (memory $m0 1)
  (func $f0 (param $v0 i32) (result i32)
    (local.get $v0)
    (i32.load)
  )
  (func $f1 (param $v0 i32) (param $v1 i32) (result i32)
    (local.get $v0)
    (local.get $v1)
    (i32.store)
    (local.get $v1)
  )
  (export "mem" (memory $m0))
  (export "get" (func $f0))
  (export "set" (func $f1)))
```

## Other Commands

The commands in this section display information about [PWASM][] itself
or allow you to run the test suite.

### `pwasm help`

#### Description

Use `pwasm help` to see a complete list of available commands.

#### Example

```
> pwasm help
Usage:
  pwasm <command> [args]

Module Commands:
  cat: Extract data for a custom section from a WASM file.
  customs: List custom sections in a WASM file.
  exports: List exports in a WASM file.
  func: Show parameters and results for an exported function.
  imports: List imports in a WASM file.
  wat: Convert one or more WASM files to WAT files.

Other Commands:
  help: Show help.
  test: Run tests.

Use "help <command>" for more details on a specific command.
```

### `pwasm test`

#### Description

The `pwasm test` command runs the internal [PWASM][] test suite and
prints the results to standard output in [CSV][] format.

Each row of the results contains the following columns:

* `result`: The test result.  One of `PASS` or `FAIL`.
* `suite`: The test suite.
* `test`: The test name.
* `assertion`: The name of the individual assertion in the parent test.

The last line of the results is a fraction.  The numerator indicates the
number of successful assertions, and the denominator indicates the total
number of test assertions.

The `pwasm test` command returns a non-zero exit code if the number of
successful tests is less than the total number of test assertions.

#### Example

This example runs the `test` command.  I have omitted most of the output
for brevity.

```
pabs@flex:~/git/pwasm> ./pwasm test
result,suite,test,assertion
PASS,cli,null,null test
PASS,init,mods,short length
PASS,init,mods,bad header
PASS,init,mods,good header
... (lots of rows omitted) ...
108/110
```

[pwasm]: https://pwasm.org/
  "PWASM"
[pwasm-git]: https://github.com/pablotron/pwasm
  "PWASM Git repository"
[webassembly]: https://en.wikipedia.org/wiki/WebAssembly
  "WebAssembly"
[c11]: https://en.wikipedia.org/wiki/C11_(C_standard_revision)
  "C11 standard"
[jit]: https://en.wikipedia.org/wiki/Just-in-time_compilation
  "Just-in-time compiler"
[aot]: https://en.wikipedia.org/wiki/Ahead-of-time_compilation
  "Ahead-of-time compiler"
[interpreter]: https://en.wikipedia.org/wiki/Interpreter_(computing)
  "Interpreter"
[stdlib]: https://en.wikipedia.org/wiki/C_standard_library
  "C standard library"
[wat]: https://webassembly.github.io/spec/core/text/index.html
  "WebAssembly text format"
[mkdocs]: https://mkdocs.org/
  "Project documentation with Markdown"
[me]: https://github.com/pablotron
  "My GitHub page"
[csv]: https://en.wikipedia.org/wiki/Comma-separated_values
  "Comma-separated value"
