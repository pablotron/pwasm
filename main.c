#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include <stdlib.h> // malloc()
#include <string.h> // strlen()
#include <stdio.h> // fopen()
#include <err.h> // err()
#include "pwasm.h"
#include "mod-tests.h"
#include "func-tests.h"

typedef struct {
  size_t num_fails;
  size_t num_tests;
} result_t;

static inline result_t
result(
  const size_t num_fails,
  const size_t num_tests
) {
  const result_t r = { num_fails, num_tests };
  return r;
}

static inline result_t
add_results(
  const result_t a,
  const result_t b
) {
  const result_t r = {
    .num_fails = a.num_fails + b.num_fails,
    .num_tests = a.num_tests + b.num_tests,
  };

  return r;
}

static char *
read_file(
  const char * const path,
  size_t * const ret_len
) {
  // open file, check for error
  FILE *fh = fopen(path, "rb");
  if (!fh) {
    // exit with error
    err(EXIT_FAILURE, "fopen(\"%s\")", path);
  }

  // seek to end, check for error
  if (fseek(fh, 0, SEEK_END)) {
    // exit with error
    err(EXIT_FAILURE, "fseek()");
  }

  // get length, check for error
  long len = ftell(fh);
  if (len < 0) {
    // exit with error
    err(EXIT_FAILURE, "ftell()");
  }

  // seek to start, check for error
  if (fseek(fh, 0, SEEK_SET)) {
    // exit with error
    err(EXIT_FAILURE, "fseek()");
  }

  // alloc memory, check for error
  void * const mem = malloc(len);
  if (!mem) {
    // exit with error
    err(EXIT_FAILURE, "malloc()");
  }

  // read file
  if (!fread(mem, len, 1, fh)) {
    // exit with error
    err(EXIT_FAILURE, "fread()");
  }

  // close file, check for error
  if (fclose(fh)) {
    // log error, continue
    warn("fclose()");
  }

  if (ret_len) {
    // return length
    *ret_len = len;
  }

  // return pointer to memory
  return mem;
}

static result_t
run_mod_init_tests(void) {
  pwasm_mem_ctx_t ctx = pwasm_mem_ctx_init_defaults(NULL);
  const suite_t suite = get_mod_tests();
  size_t num_fails = 0;

  for (size_t i = 0; i < suite.num_tests; i++) {
    // get test, run it, and get result
    const test_t * const test = suite.tests + i;
    const pwasm_buf_t buf = {
      (suite.data + test->ofs),
      test->len,
    };

    warnx("running mod_init test: %s", test->name);
    // run test, get result
    pwasm_mod_t mod;
    const size_t len = pwasm_mod_init(&ctx, &mod, buf);

    // check result, increment failure count
    const bool ok = ((len > 0) == test->want);
    num_fails += ok ? 0 : 1;

    if (!ok) {
      // free mod
      pwasm_mod_fini(&mod);
    } else {
      // warn on failure
      warnx("FAIL mod_init test: %s", test->name);
    }
  }

  // return results
  return result(num_fails, suite.num_tests);
}

static bool
run_env_test_on_add_one(
  pwasm_env_t * const env,
  pwasm_stack_t * const stack
) {
  (void) env;
  stack->ptr[stack->pos - 1].i32 += 1;
  return true;
}

static bool
run_env_test_on_mul_two(
  pwasm_env_t * const env,
  pwasm_stack_t * const stack
) {
  (void) env;

  const uint32_t a = stack->ptr[stack->pos - 2].i32;
  const uint32_t b = stack->ptr[stack->pos - 1].i32;
  stack->ptr[stack->pos - 2].i32 = a * b;
  stack->pos--;

  return true;
}

static const pwasm_value_type_t
NATIVE_VALS_ONE_I32[] = { PWASM_VALUE_TYPE_I32 };

static const pwasm_value_type_t
NATIVE_VALS_TWO_I32S[] = {
  PWASM_VALUE_TYPE_I32,
  PWASM_VALUE_TYPE_I32,
};

static const pwasm_native_func_t
NATIVE_FUNCS[] = {{
  .name = "add_one",
  .func = run_env_test_on_add_one,
  .type = {
    { NATIVE_VALS_ONE_I32, 1 },
    { NATIVE_VALS_ONE_I32, 1 },
  },
}, {
  .name = "mul_two",
  .func = run_env_test_on_mul_two,
  .type = {
    { NATIVE_VALS_TWO_I32S, 2 },
    { NATIVE_VALS_ONE_I32, 1 },
  },
}};

static const pwasm_native_t
NATIVE = {
  .num_funcs = 2,
  .funcs = NATIVE_FUNCS,
};

// test module with one method "life" (void -> i32)
static const uint8_t GUIDE_WASM[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F, 0x03,
  0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 'l',
  'i',  'f',  'e',  0x00, 0x00, 0x0A, 0x06, 0x01,
  0x04, 0x00, 0x41, 0x2A, 0x0B,
};

static result_t
run_env_tests(void) {
  const size_t num_fails = 0,
               num_tests = 1;

  // create a memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  // parse guide.wasm into mod
  pwasm_mod_t mod;
  pwasm_buf_t buf = { GUIDE_WASM, sizeof(GUIDE_WASM) };
  if (!pwasm_mod_init(&mem_ctx, &mod, buf)) {
    errx(EXIT_FAILURE, "pwasm_env_init() failed");
  }

  // set up stack
  pwasm_val_t stack_vals[10];
  pwasm_stack_t stack = {
    .ptr = stack_vals,
    .len = 10,
  };

  // get interpreter callbacks
  const pwasm_env_cbs_t * const cbs = pwasm_interpreter_get_cbs();

  // create environment, check for error
  pwasm_env_t env;
  if (!pwasm_env_init(&env, &mem_ctx, cbs, &stack, NULL)) {
    errx(EXIT_FAILURE, "pwasm_env_init() failed");
  }
  warnx("env.cbs = %p", env.cbs);

  // add native mod
  if (!pwasm_env_add_native(&env, "native", &NATIVE)) {
    errx(EXIT_FAILURE, "pwasm_env_add_native() failed");
  }

  // add mod
  if (!pwasm_env_add_mod(&env, "guide", &mod)) {
    errx(EXIT_FAILURE, "pwasm_env_add_mod() failed");
  }

  // init params
  stack.ptr[0].i32 = 3;
  stack.pos = 1;

  // call native.add_one
  if (!pwasm_env_call(&env, "native", "add_one")) {
    errx(EXIT_FAILURE, "pwasm_env_call() failed");
  }

  printf("native.add_one(3) = %u\n", stack.ptr[0].i32);

  // init params
  stack.ptr[0].i32 = 3;
  stack.ptr[1].i32 = 4;
  stack.pos = 2;

  // call native.add_one
  if (!pwasm_env_call(&env, "native", "mul_two")) {
    errx(EXIT_FAILURE, "pwasm_env_call() failed");
  }

  printf("native.mul_two(3, 4) = %u\n", stack.ptr[0].i32);

  // init params
  stack.pos = 0;

  // call guide.life
  if (!pwasm_env_call(&env, "guide", "life")) {
    errx(EXIT_FAILURE, "pwasm_env_call() failed");
  }

  // print result
  printf("guide.life() = %u\n", stack.ptr[0].i32);

  // finalize environment
  pwasm_env_fini(&env);

  // return results
  return result(num_fails, num_tests);
}

static result_t (*SUITES[])(void) = {
  // run_parse_mod_tests,
  // run_parse_func_tests,
  // run_get_mod_sizes_tests,
  run_mod_init_tests,
  run_env_tests,
};

static bool
run_tests(void) {
  // run all test suites
  result_t sum = { 0, 0 };
  for (size_t i = 0; i < (sizeof(SUITES) / sizeof(SUITES[0])); i++) {
    // run suite, add to results
    sum = add_results(sum, SUITES[i]());
  }

  // print results
  const size_t num_passed = sum.num_tests - sum.num_fails;
  printf("Tests: %zu/%zu\n", num_passed, sum.num_tests);

  // return result
  return sum.num_fails > 0;
}

int main(int argc, char *argv[]) {
  // run tests, check for error
  if (run_tests()) {
    // return failure
    return EXIT_FAILURE;
  }

  // loop through files
  for (int i = 1; i < argc; i++) {
    // read file contents
    size_t len;
    void * const mem = read_file(argv[i], &len);

    // free memory
    free(mem);
  }

  // return success
  return 0;
}
