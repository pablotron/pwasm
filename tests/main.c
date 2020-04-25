#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include <stdlib.h> // malloc()
#include <string.h> // strlen()
#include <stdio.h> // fopen()
#include <err.h> // err()
#include "../pwasm.h"
#include "mod-tests.h"
#include "func-tests.h"

#define LEN(a) (sizeof(a) / sizeof((a)[0]))

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
  const pwasm_native_instance_t * const instance
) {
  (void) instance;

  PWASM_PEEK(env->stack, 0).i32 += 1;

  return true;
}

static bool
run_env_test_on_mul_two(
  pwasm_env_t * const env,
  const pwasm_native_instance_t * const instance
) {
  (void) instance;

  const uint32_t a = PWASM_PEEK(env->stack, 1).i32;
  const uint32_t b = PWASM_PEEK(env->stack, 0).i32;
  PWASM_PEEK(env->stack, 1).i32 = a * b;
  env->stack->pos--;

  // return success
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

// test module with one func "life" (void -> i32)
static const uint8_t GUIDE_WASM[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F, 0x03,
  0x02, 0x01, 0x00, 0x07, 0x08, 0x01, 0x04, 'l',
  'i',  'f',  'e',  0x00, 0x00, 0x0A, 0x06, 0x01,
  0x04, 0x00, 0x41, 0x2A, 0x0B,
};

// pythag.wasm: test module with two functions:
// * f32.pythag(f32, f32) -> f32
// * f64.pythag(f64, f64) -> f64
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

// fib.wasm: test module with two functions:
// - fib_recurse(i32) -> i32
// - fib_iterate(i32) -> i32
//
// (source: tests/wat/01-fib.wasm)
uint8_t FIB_WASM[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x06, 0x01, 0x60, 0x01, 0x7f, 0x01, 0x7f,
  0x03, 0x03, 0x02, 0x00, 0x00, 0x07, 0x1d, 0x02,
  0x0b, 0x66, 0x69, 0x62, 0x5f, 0x72, 0x65, 0x63,
  0x75, 0x72, 0x73, 0x65, 0x00, 0x00, 0x0b, 0x66,
  0x69, 0x62, 0x5f, 0x69, 0x74, 0x65, 0x72, 0x61,
  0x74, 0x65, 0x00, 0x01, 0x0a, 0x56, 0x02, 0x1c,
  0x00, 0x20, 0x00, 0x41, 0x02, 0x49, 0x04, 0x7f,
  0x41, 0x01, 0x05, 0x20, 0x00, 0x41, 0x02, 0x6b,
  0x10, 0x00, 0x20, 0x00, 0x41, 0x01, 0x6b, 0x10,
  0x00, 0x6a, 0x0b, 0x0b, 0x37, 0x01, 0x02, 0x7f,
  0x20, 0x00, 0x41, 0x02, 0x49, 0x04, 0x7f, 0x41,
  0x01, 0x05, 0x20, 0x00, 0x41, 0x01, 0x6b, 0x21,
  0x00, 0x41, 0x01, 0x21, 0x01, 0x41, 0x01, 0x21,
  0x02, 0x03, 0x7f, 0x20, 0x01, 0x20, 0x01, 0x20,
  0x02, 0x6a, 0x21, 0x01, 0x21, 0x02, 0x20, 0x00,
  0x41, 0x01, 0x6b, 0x22, 0x00, 0x0d, 0x00, 0x20,
  0x01, 0x0b, 0x0b, 0x0b
};

static const struct {
  const char * const name;
  const pwasm_buf_t data;
} WASM_TEST_BLOBS[] = {{
  .name = "guide",
  .data = { GUIDE_WASM, sizeof(GUIDE_WASM) },
}, {
  .name = "pythag",
  .data = { PYTHAG_WASM, sizeof(PYTHAG_WASM) },
}, {
  .name = "fib",
  .data = { FIB_WASM, sizeof(FIB_WASM) },
}};

static const pwasm_val_t
WASM_TEST_VALS[] = {
  // mod: "native", func: "add_one", test: 1, type: "params", num: 1
  { .i32 = 3 },

  // mod: "native", func: "add_one", test: 1, type: "result", num: 1
  { .i32 = 4 },

  // mod: "native", func: "add_two", test: 1, type: "params", num: 2
  { .i32 = 3 },
  { .i32 = 4 },

  // mod: "native", func: "add_two", test: 1, type: "result", num: 1
  { .i32 = 12 },

  // mod: "guide", func: "life", test: 1, type: "params", num: 0

  // mod: "guide", func: "life", test: 1, type: "result", num: 1
  { .i32 = 42 },

  // mod: "pythag", func: "f32.pythag", test: 1, type: "params", num: 2
  { .f32 = 3.0f },
  { .f32 = 4.0f },

  // mod: "pythag", func: "f32.pythag", test: 1, type: "result", num: 1
  { .f32 = 5.0f },

  // mod: "pythag", func: "f64.pythag", test: 1, type: "params", num: 2
  { .f64 = 5.0f },
  { .f64 = 6.0f },

  // mod: "pythag", func: "f64.pythag", test: 1, type: "result", num: 1
  { .f64 = 7.810250f },

  // mod: "fib", func: "fib_recurse", test: 1, type: "params", num: 1
  { .i32 = 3 },

  // mod: "fib", func: "fib_recurse", test: 1, type: "result", num: 1
  { .i32 = 3 },

  // mod: "fib", func: "fib_recurse", test: 2, type: "params", num: 1
  { .i32 = 4 },

  // mod: "fib", func: "fib_recurse", test: 2, type: "result", num: 1
  { .i32 = 5 },

  // mod: "fib", func: "fib_iterate", test: 1, type: "params", num: 1
  { .i32 = 3 },

  // mod: "fib", func: "fib_iterate", test: 1, type: "result", num: 1
  { .i32 = 3 },

  // mod: "fib", func: "fib_iterate", test: 2, type: "params", num: 1
  { .i32 = 4 },

  // mod: "fib", func: "fib_iterate", test: 2, type: "result", num: 1
  { .i32 = 5 },
};

typedef struct {
  const char * const text;
  const char * const mod;
  const char * const func;
  const pwasm_slice_t params;
  const pwasm_slice_t result;
  const pwasm_result_type_t type;
} wasm_test_call_t;

static const wasm_test_call_t
WASM_TEST_CALLS[] = {{
  .text   = "native.add_one(3)",
  .mod    = "native",
  .func   = "add_one",
  .params = { 0, 1 },
  .result = { 1, 1 },
  .type   = PWASM_RESULT_TYPE_I32,
}, {
  .text   = "native.mul_two(3, 4)",
  .mod    = "native",
  .func   = "mul_two",
  .params = { 2, 2 },
  .result = { 4, 1 },
  .type   = PWASM_RESULT_TYPE_I32,
}, {
  .text   = "guide.life()",
  .mod    = "guide",
  .func   = "life",
  .params = { 0, 0 },
  .result = { 5, 1 },
  .type   = PWASM_RESULT_TYPE_I32,
}, {
  .text   = "pythag.f32.pythag(3, 4)",
  .mod    = "pythag",
  .func   = "f32.pythag",
  .params = { 6, 2 },
  .result = { 8, 1 },
  .type   = PWASM_RESULT_TYPE_F32,
}, {
  .text   = "pythag.f64.pythag(5, 6)",
  .mod    = "pythag",
  .func   = "f64.pythag",
  .params = { 9, 2 },
  .result = { 11, 1 },
  .type   = PWASM_RESULT_TYPE_F64,
}, {
  .text   = "fib.fib_recurse(3) (test 1)",
  .mod    = "fib",
  .func   = "fib_recurse",
  .params = { 12, 1 },
  .result = { 13, 1 },
  .type   = PWASM_RESULT_TYPE_I32,
}, {
  .text   = "fib.fib_recurse(4) (test 2)",
  .mod    = "fib",
  .func   = "fib_recurse",
  .params = { 14, 1 },
  .result = { 15, 1 },
  .type   = PWASM_RESULT_TYPE_I32,
}, {
  .text   = "fib.fib_iterate(3) (test 1)",
  .mod    = "fib",
  .func   = "fib_recurse",
  .params = { 16, 1 },
  .result = { 17, 1 },
  .type   = PWASM_RESULT_TYPE_I32,
}, {
  .text   = "fib.fib_iterate(4) (test 2)",
  .mod    = "fib",
  .func   = "fib_iterate",
  .params = { 18, 1 },
  .result = { 19, 1 },
  .type   = PWASM_RESULT_TYPE_I32,
}};

static result_t
run_env_tests(void) {
  const size_t num_fails = 0,
               num_tests = 1;

  // create a memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

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
  warnx("env.cbs = %p", (void*) env.cbs);

  // add native mod
  if (!pwasm_env_add_native(&env, "native", &NATIVE)) {
    errx(EXIT_FAILURE, "pwasm_env_add_native() failed");
  }

  // parse and add wasms
  pwasm_mod_t mods[LEN(WASM_TEST_BLOBS)];
  for (size_t i = 0; i < LEN(WASM_TEST_BLOBS); i++) {
    // get name and data
    const char * const name = WASM_TEST_BLOBS[i].name;
    pwasm_buf_t buf = WASM_TEST_BLOBS[i].data;

    // parse blob into mod, check for error
    if (!pwasm_mod_init(&mem_ctx, &(mods[i]), buf)) {
      errx(EXIT_FAILURE, "%s.wasm: pwasm_mod_init() failed", name);
    }

    // add mod to env, check for error
    if (!pwasm_env_add_mod(&env, name, &(mods[i]))) {
      errx(EXIT_FAILURE, "%s: pwasm_env_add_mod() failed", name);
    }
  }

  for (size_t i = 0; i < LEN(WASM_TEST_CALLS); i++) {
    // get test
    const wasm_test_call_t test = WASM_TEST_CALLS[i];

    // populate stack
    stack.pos = test.params.len;
    if (test.params.len > 0) {
      const size_t num_bytes = sizeof(pwasm_val_t) * test.params.len;
      memcpy(stack.ptr, WASM_TEST_VALS + test.params.ofs, num_bytes);
    }

    // invoke function, check for error
    if (!pwasm_call(&env, test.mod, test.func)) {
      errx(EXIT_FAILURE, "%s.%s: pwasm_call() failed", test.mod, test.func);
    }

    if (test.result.len > 0) {
      // print test result
      switch (test.type) {
      case PWASM_RESULT_TYPE_I32:
        printf("%s = %u\n", test.text, stack.ptr[0].i32);
        break;
      case PWASM_RESULT_TYPE_I64:
        printf("%s = %lu\n", test.text, stack.ptr[0].i64);
        break;
      case PWASM_RESULT_TYPE_F32:
        printf("%s = %f\n", test.text, stack.ptr[0].f32);
        break;
      case PWASM_RESULT_TYPE_F64:
        printf("%s = %f\n", test.text, stack.ptr[0].f64);
        break;
      case PWASM_RESULT_TYPE_VOID:
        printf("%s: passed\n", test.text);
        break;
      default:
        errx(EXIT_FAILURE, "unknown test result type: %u", test.type);
      }
    }
  }

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