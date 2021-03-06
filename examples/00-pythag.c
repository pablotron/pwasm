/**
 * 00-pythag.c: minimal standalone PWASM example.
 *
 * Usage:
 *   # compile examples/00-pythag.c and pwasm.c
 *   cc -c -W -Wall -Wextra -Werror -pedantic -std=c11 -I. -O3 examples/00-pythag.c
 *   cc -c -W -Wall -Wextra -Werror -pedantic -std=c11 -I. -O3 pwasm.c
 *
 *   # link and build as ./example-00-pythag
 *   cc -o ./example-00-pythag {00-pythag,pwasm}.o -lm
 *
 * Output:
 *   # run example-00-pythag
 *   > ./example-00-pythag
 *   pythag.f32(3.0, 4.0) = 5.000000
 *   pythag.f64(5.0, 6.0) = 7.810250
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
 * * f32 (f32, f32 -> f32): Calculate the length of the
 *   hypotenuse of a right triangle from the lengths of the other
 *   two sides of the triangle.
 *
 * * f64 (f64, f64 -> f64): Calculate the length of the
 *   hypotenuse of a right triangle from the lengths of the other
 *   two sides of the triangle.
 */
static const uint8_t PYTHAG_WASM[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x0d, 0x02, 0x60, 0x02, 0x7d, 0x7d, 0x01,
  0x7d, 0x60, 0x02, 0x7c, 0x7c, 0x01, 0x7c, 0x03,
  0x03, 0x02, 0x00, 0x01, 0x07, 0x0d, 0x02, 0x03,
  0x66, 0x33, 0x32, 0x00, 0x00, 0x03, 0x66, 0x36,
  0x34, 0x00, 0x01, 0x0a, 0x1f, 0x02, 0x0e, 0x00,
  0x20, 0x00, 0x20, 0x00, 0x94, 0x20, 0x01, 0x20,
  0x01, 0x94, 0x92, 0x91, 0x0b, 0x0e, 0x00, 0x20,
  0x00, 0x20, 0x00, 0xa2, 0x20, 0x01, 0x20, 0x01,
  0xa2, 0xa0, 0x9f, 0x0b
};

static void test_pythag_f32(pwasm_env_t * const env) {
  pwasm_stack_t * const stack = env->stack;

  // set parameters values and parameter count
  stack->ptr[0].f32 = 3;
  stack->ptr[1].f32 = 4;
  stack->pos = 2;

  // call function, check for error
  if (!pwasm_call(env, "pythag", "f32")) {
    errx(EXIT_FAILURE, "pythag.f32: pwasm_call() failed");
  }

  // print result (the first stack entry) to standard output
  printf("pythag.f32(3.0, 4.0) = %f\n", stack->ptr[0].f32);
}

static void test_pythag_f64(pwasm_env_t * const env) {
  // get stack from environment
  pwasm_stack_t * const stack = env->stack;

  // set parameters
  stack->ptr[0].f64 = 5;
  stack->ptr[1].f64 = 6;
  stack->pos = 2;

  // call function, check for error
  if (!pwasm_call(env, "pythag", "f64")) {
    errx(EXIT_FAILURE, "f64: pwasm_call() failed");
  }

  // print result (the first stack entry) to standard output
  printf("pythag.f64(5.0, 6.0) = %f\n", stack->ptr[0].f64);
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

  // call "f32" function
  test_pythag_f32(&env);

  // call "f64" function
  test_pythag_f64(&env);

  // finalize interpreter environment and parsed module
  pwasm_env_fini(&env);
  pwasm_mod_fini(&mod);

  // return success
  return EXIT_SUCCESS;
}
