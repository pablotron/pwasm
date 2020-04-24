# pwasm

PWASM (pronounced "possum") is an embeddable [WebAssembly][] parser and
interpreter, written in [C11][].

Features:
* Written in modern [C11][].
* Easy to embed.
* MIT-licensed.
* Built-in [interpreter][], runs on any platform that supports [C11][]
  and a minimal subset of the [C standard library][stdlib].
* Modular architecture.  Use the parser and ignore the interpreter,
  write your own [JIT][] environment, etc.
* Amortized O(1) memory allocation.
* "Native" module support.  Call native functions from a [WebAssembly][]
  module.
* Custom memory allocator support.

Coming soon:
* [JIT][] and [AOT][] compiler.
* Command-line utility (e.g. `objdump`, `disasm`, etc)
* Threaded parser.
* Vector instruction extension.
* Documentation.

## Usage

PWASM is meant to be embedded in an existing application.

Here's how:

1. Copy `pwasm.[hc]` the source directory of an existing application.
2. Add `pwasm.c` to your build.
3. Link against `-lm`.

To execute functions from a [WebAssembly][] module, do the following:

1. Initialize a PWASM memory context (`pwasm_mem_ctx_init_defaults()`).
2. Parse one or more modules (`pwasm_mod_init()`).
3. Initialize an interpreter environment (`pwasm_env_init()`).
4. Add the parsed modules into the environment (`pwasm_env_add_mod()`).
5. Call [WebAssembly][] functions.
6. Finalize the environment (`pwasam_env_fini()`).
7. Finalize the parsed modules (`pwasm_mod_fini()`).

## Example

Below is a self-contained example which does the following:

1. Parses a [WebAssembly][] module containing two functions.
2. Initializes an interpreter environment.
3. Adds the parsed module to the interpreter.
4. Executes the `f32.pythag()` function from the parsed module.
5. Prints the result to standard output.
6. Executes `f64.pythag()` function from the parsed module.
7. Prints the result to standard output.
8. Finalizes the interpreter and the parsed module.

```c
/**
 * example-00-pythag.c: minimal standalone pwasm example.
 *
 * Usage:
 *   # compile examples/00-pythag.c and pwasm.c
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
  const pwasm_env_cbs_t * const interp_cbs = pwasm_interpreter_get_cbs();

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

[webassembly]: https://en.wikipedia.org/wiki/WebAssembly
[c11]: https://en.wikipedia.org/wiki/C11_(C_standard_revision)
[jit]: https://en.wikipedia.org/wiki/Just-in-time_compilation
[aot]: https://en.wikipedia.org/wiki/Ahead-of-time_compilation
[interpreter]: https://en.wikipedia.org/wiki/Interpreter_(computing)
[stdlib]: https://en.wikipedia.org/wiki/C_standard_library
