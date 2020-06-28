/**
 * 01-jit.c: minimal pwasm jit example.
 *
 * Usage:
 *   # compile examples/01-jit.c and pwasm.c
 *   cc -c -W -Wall -Wextra -Werror -pedantic -std=c11 -I. -O3 examples/01-jit.c
 *   cc -c -W -Wall -Wextra -Werror -pedantic -std=c11 -I. -O3 pwasm.c
 *   cc -c -W -Wall -Wextra -Werror -pedantic -std=c11 -I. -Ipath/to/luajit-2.0 -O3 pwasm-dynasm-jit.c
 *
 *   # link and build as ./example-01-jit
 *   cc -o ./example-01-jit {01-jit,pwasm,pwasm-dynasm-jit}.o -ldl -lm
 *
 * Output:
 *   # run example-01-jit
 *   > ./example-01-jit
 *   pythag.f32(3.0, 4.0) = 5.000000
 *   pythag.f64(5.0, 6.0) = 7.810250
 *
 */

#include <stdlib.h> // EXIT_FAILURE
#include <stdio.h> // printf()
#include <stdint.h> // uint8_t, etc
#include <err.h> // errx()
#include <pwasm.h> // PWASM
#include <pwasm-dynasm-jit.h> // PWASM JIT

/**
 * Blob containing a small WebAssembly (WASM) module.  The module
 * exports two functions: 
 *
 * * f32: Takes the length of the adjacent and opposite sides of a
 *   right triangle and returns the length of the hypotenuse.  All
 *   lengths are 32-bit, single-precision floating point values.
 *
 * * f64: Takes the length of the adjacent and opposite sides of a
 *   right triangle and returns the length of the hypotenuse.  All
 *   lengths are 64-bit, double-precision floating point values.
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
  // get stack from environment
  pwasm_stack_t * const stack = env->stack;

  // set parameters values and parameter count
  stack->ptr[0].f32 = 3;
  stack->ptr[1].f32 = 4;
  stack->pos = 2;

  // call function "f32" in the "pythag" module, check for error
  if (!pwasm_call(env, "pythag", "f32")) {
    errx(EXIT_FAILURE, "f32: pwasm_call() failed");
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

  // initialize jit compiler
  pwasm_jit_t jit;
  if (!pwasm_dynasm_jit_init(&jit, &mem_ctx)) {
    errx(EXIT_FAILURE, "pwasm_dynasm_jit_init() failed");
  }

  // get AOT JIT callbacks
  pwasm_env_cbs_t cbs;
  pwasm_aot_jit_get_cbs(&cbs, &jit);

  // set up stack (used to pass parameters and results and
  // to execute the stack machine inside functions)
  pwasm_val_t stack_vals[10];
  pwasm_stack_t stack = {
    .ptr = stack_vals,
    .len = 10,
  };

  // create execution environment, check for error
  pwasm_env_t env;
  if (!pwasm_env_init(&env, &mem_ctx, &cbs, &stack, NULL)) {
    errx(EXIT_FAILURE, "pwasm_env_init() failed");
  }

  // add parsed module to interpreter environment with a
  // name of "pythag", check for error
  if (!pwasm_env_add_mod(&env, "pythag", &mod)) {
    errx(EXIT_FAILURE, "pythag: pwasm_env_add_mod() failed");
  }

  // call "pythag.f32" function and print result
  test_pythag_f32(&env);

  // call "pythag.f64" function and print result
  test_pythag_f64(&env);

  // finalize environment, jit, and parsed module
  pwasm_env_fini(&env);
  pwasm_jit_fini(&jit);
  pwasm_mod_fini(&mod);

  // return success
  return EXIT_SUCCESS;
}
