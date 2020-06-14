#include <stdbool.h> // bool
#include <string.h> // memcpy()
#include <stdio.h> // snprintf()
#include <float.h> // FLT_EPSILON, DBL_EPSILON
#include "../tests.h"
#include "../../pwasm.h"
#include "../result-type.h"

#define LEN(ary) (sizeof(ary) / sizeof(ary[0]))

static bool
test_native_on_add_one(
  pwasm_env_t * const env,
  const pwasm_native_t * const native
) {
  (void) native;

  PWASM_PEEK(env->stack, 0).i32 += 1;

  return true;
}

static bool
test_native_on_mul_two(
  pwasm_env_t * const env,
  const pwasm_native_t * const native
) {
  (void) native;

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
  .func = test_native_on_add_one,
  .type = {
    { NATIVE_VALS_ONE_I32, 1 },
    { NATIVE_VALS_ONE_I32, 1 },
  },
}, {
  .name = "mul_two",
  .func = test_native_on_mul_two,
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

static const pwasm_val_t
TEST_VALS[] = {
  // mod: "native", func: "add_one", test: 1, type: "params", num: 1
  { .i32 = 3 },

  // mod: "native", func: "add_one", test: 1, type: "result", num: 1
  { .i32 = 4 },

  // mod: "native", func: "add_two", test: 1, type: "params", num: 2
  { .i32 = 3 },
  { .i32 = 4 },

  // mod: "native", func: "add_two", test: 1, type: "result", num: 1
  { .i32 = 12 },
};

typedef struct {
  const char * const text;
  const char * const mod;
  const char * const func;
  const pwasm_slice_t params;
  const pwasm_slice_t result;
  const result_type_t type;
} wasm_test_call_t;

static const wasm_test_call_t
WASM_TEST_CALLS[] = {{
  .text   = "native.add_one(3)",
  .mod    = "native",
  .func   = "add_one",
  .params = { 0, 1 },
  .result = { 1, 1 },
  .type   = RESULT_TYPE_I32,
}, {
  .text   = "native.mul_two(3, 4)",
  .mod    = "native",
  .func   = "mul_two",
  .params = { 2, 2 },
  .result = { 4, 1 },
  .type   = RESULT_TYPE_I32,
}};

static bool got_expected_result_value(
  const wasm_test_call_t test,
  const pwasm_stack_t * const stack
) {
  const pwasm_val_t got_val = stack->ptr[0];
  const pwasm_val_t exp_val = TEST_VALS[test.result.ofs];

  return ((
    (test.type == RESULT_TYPE_I32) &&
    (stack->len == 1) &&
    (got_val.i32 == exp_val.i32)
  ) || (
    (test.type == RESULT_TYPE_I64) &&
    (stack->len == 1) &&
    (got_val.i64 == exp_val.i64)
  ) || (
    (test.type == RESULT_TYPE_F32) &&
    (stack->len == 1) &&
    (got_val.f32 - FLT_EPSILON <= exp_val.f32) &&
    (got_val.f32 + FLT_EPSILON >= exp_val.f32)
  ) || (
    (test.type == RESULT_TYPE_F64) &&
    (stack->len == 1) &&
    (got_val.f64 - DBL_EPSILON <= exp_val.f64) &&
    (got_val.f64 + DBL_EPSILON >= exp_val.f64)
  ) || (
    (test.type == RESULT_TYPE_VOID) &&
    (stack->len == 0)
  ));
}

void test_native_calls(
  cli_test_ctx_t * const test_ctx,
  const cli_test_t * const cli_test
) {
  char buf[1024];

  // create a memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  // set up stack
  pwasm_val_t stack_vals[10];
  pwasm_stack_t stack = {
    .ptr = stack_vals,
    .len = 10,
  };

  // get interpreter callbacks
  const pwasm_env_cbs_t * const cbs = pwasm_new_interpreter_get_cbs();

  // create environment, check for error
  pwasm_env_t env;
  if (!pwasm_env_init(&env, &mem_ctx, cbs, &stack, NULL)) {
    cli_test_error(test_ctx, "pwasm_env_init() failed");
  }

  // add native mod
  if (!pwasm_env_add_native(&env, "native", &NATIVE)) {
    cli_test_error(test_ctx, "pwasm_env_add_native() failed");
  }

  for (size_t i = 0; i < LEN(WASM_TEST_CALLS); i++) {
    // get test
    const wasm_test_call_t test = WASM_TEST_CALLS[i];

    // check for valid test type
    if (!result_type_is_valid(test.type)) {
      snprintf(buf, sizeof(buf), "pwasm_call(&env, \"%s\", \"%s\") result: unknown test result type: %u", test.mod, test.func, test.type);
      cli_test_error(test_ctx, buf);
    }

    // populate stack
    stack.pos = test.params.len;
    if (test.params.len > 0) {
      const size_t num_bytes = sizeof(pwasm_val_t) * test.params.len;
      memcpy(stack.ptr, TEST_VALS + test.params.ofs, num_bytes);
    }

    // build assertion name
    snprintf(buf, sizeof(buf), "call pwasm_call(&env, \"%s\", \"%s\")", test.mod, test.func);

    // invoke function
    const bool call_ok = pwasm_call(&env, test.mod, test.func);

    // check pwasm_call() result
    if (call_ok) {
      // pass assertion
      cli_test_pass(test_ctx, cli_test, buf);
    } else {
      // fail assertion
      cli_test_fail(test_ctx, cli_test, buf);
    }

    // build assertion name
    snprintf(buf, sizeof(buf), "check result (%s) of pwasm_call(&env, \"%s\", \"%s\")", result_type_get_name(test.type), test.mod, test.func);

    // check result value
    if (call_ok && got_expected_result_value(test, &stack)) {
      cli_test_pass(test_ctx, cli_test, buf);
    } else {
      cli_test_pass(test_ctx, cli_test, buf);
    }
  }

  // finalize environment
  pwasm_env_fini(&env);
}
