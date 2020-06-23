# PWASM Library

## Overview

The [PWASM][] library allows you to parse [WebAssembly][] modules and
and run functions from [WebAssembly][] modules inside your application.

## Features

The [PWASM][] library has the following features:

* Easy to embed. Two files: `pwasm.c` and `pwasm.h`.
* Supports isolated execution environments.
* Built-in [interpreter][] which should run just about anywhere.
* Modular architecture.  Use the parser and ignore the interpreter,
  write your own [JIT][], etc.
* No dependencies other than the [C standard library][stdlib].
* Customizable memory allocator.
* Parser uses amortized O(1) memory allocation.
* "Native" module support.  Call native functions from a [WebAssembly][]
  module.
* Written in modern [C11][].
* [MIT-licensed][mit].
* Multi-value block, [SIMD][], and `trunc_sat` extended opcode support.

**Coming Soon**

* x86-64 [AOT][] [JIT][] compiler (via [DynASM][]).
* Threaded parser.

## Usage

PWASM is meant to be embedded in an existing application.

Here's how:

1. Copy `pwasm.h` and `pwasm.c` into the source directory of an existing
   application.
2. Add `pwasm.c` to your build.
3. Link against `-lm`.

To execute functions from a [WebAssembly][] module, do the following:

1. Create a PWASM memory context.
2. Read the contents of the module.
3. Parse the module with `pwasm_mod_init()`.
4. Create an interpreter environment with `pwasm_env_init()`.
5. Add the parsed module into the environment with `pwasm_env_add_mod()`.
6. Call module functions with `pwasm_call()`.

## Example

The example below does the following:

1. Parses a [WebAssembly][] module.
2. Creates an interpreter environment.
3. Adds the parsed module to the interpreter.
4. Executes the `f32.pythag()` module function.
5. Prints the result to standard output.
6. Executes `f64.pythag()` module function.
7. Prints the result to standard output.
8. Finalizes the interpreter and the parsed module.

```c
/**
 * example-00-pythag.c: minimal standalone pwasm example.
 *
 * Usage:
 *   # compile 00-pythag.c and pwasm.c
 *   cc -c -W -Wall -Wextra -Werror -pedantic -std=c11 -I. -O3 examples/example-00-pythag.c
 *   cc -c -W -Wall -Wextra -Werror -pedantic -std=c11 -I. -O3 pwasm.c
 *
 *   # link and build as ./example-00-pythag
 *   cc -o ./example-00-pythag {00-pythag,pwasm}.o -lm
 *
 * Output:
 *   # run example-00-pythag
 *   > ./example-00-pythag
 *   f32.pythag(3.0, 4.0) = 5.000000
 *   f64.pythag(5.0, 6.0) = 7.810250
 *
 */

#include <stdlib.h> // EXIT_FAILURE
#include <stdio.h> // printf()
#include <stdint.h> // uint8_t, etc
#include <err.h> // errx()
#include <pwasm.h>

/**
 * Blob containing a small WebAssembly (WASM) module.
 *
 * This WASM module exports two functions:
 *
 * * f32.pythag (f32, f32 -> f32): Calculate the length of the
 *   hypotenuse of a right triangle from the lengths of the other
 *   two sides of the triangle.
 *
 * * f64.pythag (f64, f64 -> f64): Calculate the length of the
 *   hypotenuse of a right triangle from the lengths of the other
 *   two sides of the triangle.
 */
static const uint8_t PYTHAG_WASM[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x0D, 0x02, 0x60, 0x02, 0x7E, 0x7E, 0x01,
  0x7E, 0x60, 0x02, 0x7C, 0x7C, 0x01, 0x7C, 0x03,
  0x03, 0x02, 0x00, 0x01, 0x07, 0x1B, 0x02, 0x0A,
  'f',  '3',  '2',  '.',  'p',  'y',  't',  'h',
  'a',  'g',  0x00, 0x00, 0x0A, 'f',  '6',  '4',
  '.',  'p',  'y',  't',  'h',  'a',  'g',  0x00,
  0x01, 0x0A, 0x1F, 0x02, 0x0E, 0x00, 0x20, 0x00,
  0x20, 0x00, 0x94, 0x20, 0x01, 0x20, 0x01, 0x94,
  0x92, 0x91, 0x0B, 0x0E, 0x00, 0x20, 0x00, 0x20,
  0x00, 0xA2, 0x20, 0x01, 0x20, 0x01, 0xA2, 0xA0,
  0x9F, 0x0B,
};

static void test_f32_pythag(
  pwasm_env_t * const env,
  pwasm_stack_t * const stack
) {
  // set parameters values and parameter count
  stack->ptr[0].f32 = 3;
  stack->ptr[1].f32 = 4;
  stack->pos = 2;

  // call function "f32.pythag" in the "pythag" module, check for error
  if (!pwasm_call(env, "pythag", "f32.pythag")) {
    errx(EXIT_FAILURE, "f32.pythag: pwasm_call() failed");
  }

  // print result (the first stack entry) to standard output
  printf("f32.pythag(3.0, 4.0) = %f\n", stack->ptr[0].f32);
}

static void test_f64_pythag(
  pwasm_env_t * const env,
  pwasm_stack_t * const stack
) {
  // set parameters
  stack->ptr[0].f64 = 5;
  stack->ptr[1].f64 = 6;
  stack->pos = 2;

  // call function, check for error
  if (!pwasm_call(env, "pythag", "f64.pythag")) {
    errx(EXIT_FAILURE, "f64.pythag: pwasm_call() failed");
  }

  // print result (the first stack entry) to standard output
  printf("f64.pythag(5.0, 6.0) = %f\n", stack->ptr[0].f64);
}

int main(void) {
  // create a memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  pwasm_mod_t mod;
  {
    // wrap pythag.wasm data in buffer
    pwasm_buf_t buf = { PYTHAG_WASM, sizeof(PYTHAG_WASM) };

    // parse module, check for error
    if (!pwasm_mod_init(&mem_ctx, &mod, buf)) {
      errx(EXIT_FAILURE, "pwasm_mod_init() failed");
    }
  }

  // set up stack (used to pass parameters and results and
  // to execute the stack machine inside functions)
  pwasm_val_t stack_vals[10];
  pwasm_stack_t stack = {
    .ptr = stack_vals,
    .len = 10,
  };

  // get interpreter callbacks
  const pwasm_env_cbs_t * const interp_cbs = pwasm_new_interpreter_get_cbs();

  // create interpreter environment, check for error
  pwasm_env_t env;
  if (!pwasm_env_init(&env, &mem_ctx, interp_cbs, &stack, NULL)) {
    errx(EXIT_FAILURE, "pwasm_env_init() failed");
  }

  // add parsed module to interpreter environment with a
  // name of "pythag", check for error
  if (!pwasm_env_add_mod(&env, "pythag", &mod)) {
    errx(EXIT_FAILURE, "pythag: pwasm_env_add_mod() failed");
  }

  // call "f32.pythag" function
  test_f32_pythag(&env, &stack);

  // call "f64.pythag" function
  test_f64_pythag(&env, &stack);

  // finalize interpreter environment and parsed module
  pwasm_env_fini(&env);
  pwasm_mod_fini(&mod);

  // return success
  return EXIT_SUCCESS;
}
```

## API Documentation

The [API][] documentation covers the [PWASM][] library [API][].

The latest [PWASM][] [API][] documentation is always available online at
the following URL:

<https://pwasm.org/docs/latest/api/>

The [PWASM][] [API][] documentation is generated from annotations in the
`pwasm.h` header file.  You can build the [API][] documentation yourself
with [Doxygen][] by doing the following:

1. Clone the [PWASM Git repository][pwasm-git].
2. Switch to the directory of the cloned Git repository.
3. Run `doxygen` to generate the [API][] documentation in the
   `api-docs/` directory.

**Note:** The [API][] documentation is currently incomplete.

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
[mit]: https://opensource.org/licenses/MIT
  "MIT license"
[doxygen]: http://www.doxygen.nl/
  "Doxygen API documentation generator"
[api]: https://en.wikipedia.org/wiki/Application_programming_interface
  "Application Programming Interface (API)"
[simd]: https://en.wikipedia.org/wiki/SIMD
  "Single Instruction, Multiple Data"
[dynasm]: https://luajit.org/dynasm.html
  "DynASM dynamic assembler"
