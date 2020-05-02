# Test WAT Files

This directory contains several tests in [WAT][] format.

They can be compiled to WebAssembly with `wat2wasm`, which is
distributed with [WABT][].

Note: Many of these compiled tests are embedded directoy in the built-in
test suite, in `cli/tests/wasm.c`.

[wat]: https://webassembly.github.io/spec/core/text/index.html
  "WebAssembly text format"
[wabt]: https://github.com/WebAssembly/wabt
  "WebAssembly Binary Toolkit"
