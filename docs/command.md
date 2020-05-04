# Command

## Overview

The `pwasm` command lets you inspect and disassemble [WebAssembly][]
modules.

## Features

The `pwasm` tool can:

* Disassemble [WebAssembly][] modules into [WebAssembly Text (WAT)][wat]
  files.
* Extract the data from custom sections of [WebAssembly][] modules.
* Run the built-in test suite.

## Usage

Use the `pwasm help` command for a list of command-line options:

```
> pwasm help
Usage:
  pwasm <command> [args]

Commands:
  help: Show help.  Use "help <command>" for help on a command.
  test: Run tests.
  wat: Convert one or more WASM files to WAT files.
  list-custom: List custom sections in WASM file.
  cat-custom: Extract custom section from WASM file.
```

## Example

Here's an example which uses the `pwasm` command-line tool to
disassemble a [WebAssembly][] module named `03-mem.wasm` into
[WebAssembly text (WAT)][wat] format.

```
> pwasm wat ./03-mem.wasm
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

[pwasm]: https://pwasm.org/
[webassembly]: https://en.wikipedia.org/wiki/WebAssembly
[c11]: https://en.wikipedia.org/wiki/C11_(C_standard_revision)
[jit]: https://en.wikipedia.org/wiki/Just-in-time_compilation
[aot]: https://en.wikipedia.org/wiki/Ahead-of-time_compilation
[interpreter]: https://en.wikipedia.org/wiki/Interpreter_(computing)
[stdlib]: https://en.wikipedia.org/wiki/C_standard_library
[wat]: https://webassembly.github.io/spec/core/text/index.html
