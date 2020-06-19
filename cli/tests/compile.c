#include <stdbool.h> // bool
#include <stdio.h> // snprintf
#include "../tests.h"
#include "../../pwasm.h"
#include "../../pwasm-compile.h"

// maximum test stack depth
#define MAX_STACK_DEPTH 100

// aot-basics.wasm: basic aot tests
// generated by: xxd -c 8 -i data/wat/17-aot-basics.wasm
// (source: data/wat/17-aot-basics.wat)
static const uint8_t AOT_BASICS_WASM[] = {
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x10, 0x03, 0x60, 0x00, 0x01, 0x7f, 0x60,
  0x01, 0x7f, 0x01, 0x7f, 0x60, 0x02, 0x7f, 0x7f,
  0x01, 0x7f, 0x03, 0x0b, 0x0a, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x01, 0x07,
  0x6b, 0x0a, 0x08, 0x61, 0x64, 0x64, 0x5f, 0x69,
  0x33, 0x32, 0x73, 0x00, 0x00, 0x04, 0x74, 0x72,
  0x61, 0x70, 0x00, 0x01, 0x0c, 0x69, 0x66, 0x5f,
  0x65, 0x6c, 0x73, 0x65, 0x5f, 0x74, 0x72, 0x75,
  0x65, 0x00, 0x02, 0x0d, 0x69, 0x66, 0x5f, 0x65,
  0x6c, 0x73, 0x65, 0x5f, 0x66, 0x61, 0x6c, 0x73,
  0x65, 0x00, 0x03, 0x07, 0x69, 0x66, 0x5f, 0x74,
  0x72, 0x75, 0x65, 0x00, 0x04, 0x08, 0x69, 0x66,
  0x5f, 0x66, 0x61, 0x6c, 0x73, 0x65, 0x00, 0x05,
  0x08, 0x62, 0x72, 0x5f, 0x6f, 0x75, 0x74, 0x65,
  0x72, 0x00, 0x06, 0x08, 0x62, 0x72, 0x5f, 0x69,
  0x6e, 0x6e, 0x65, 0x72, 0x00, 0x07, 0x03, 0x73,
  0x75, 0x62, 0x00, 0x08, 0x05, 0x69, 0x73, 0x5f,
  0x39, 0x39, 0x00, 0x09, 0x0a, 0x73, 0x0a, 0x09,
  0x00, 0x41, 0xfb, 0x00, 0x41, 0xc8, 0x03, 0x6a,
  0x0b, 0x03, 0x00, 0x00, 0x0b, 0x0e, 0x00, 0x41,
  0x01, 0x04, 0x7f, 0x41, 0xc1, 0x02, 0x05, 0x41,
  0xc8, 0x03, 0x0b, 0x0b, 0x0c, 0x00, 0x41, 0x00,
  0x04, 0x7f, 0x41, 0x20, 0x05, 0x41, 0x2d, 0x0b,
  0x0b, 0x0f, 0x00, 0x41, 0x80, 0x08, 0x41, 0x01,
  0x04, 0x01, 0x1a, 0x41, 0xaf, 0x96, 0x13, 0x0b,
  0x0b, 0x0c, 0x00, 0x41, 0x16, 0x41, 0x00, 0x04,
  0x01, 0x1a, 0x41, 0x2c, 0x0b, 0x0b, 0x08, 0x00,
  0x41, 0xd2, 0x09, 0x0c, 0x00, 0x00, 0x0b, 0x0b,
  0x00, 0x41, 0xae, 0x2c, 0x02, 0x40, 0x0c, 0x00,
  0x00, 0x0b, 0x0b, 0x07, 0x00, 0x20, 0x00, 0x20,
  0x01, 0x6b, 0x0b, 0x0d, 0x00, 0x41, 0x00, 0x41,
  0x01, 0x41, 0xe3, 0x00, 0x20, 0x00, 0x6b, 0x1b,
  0x0b
};

void test_compile(
  cli_test_ctx_t * const test_ctx,
  const cli_test_t * const cli_test
) {
  // create a memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  // set up stack
  pwasm_val_t stack_vals[MAX_STACK_DEPTH];
  pwasm_stack_t stack = {
    .ptr = stack_vals,
    .len = MAX_STACK_DEPTH,
  };

  // get interpreter callbacks
  pwasm_env_cbs_t cbs;
  pwasm_aot_jit_get_cbs(&cbs, pwasm_compile);

  // create environment, check for error
  pwasm_env_t env;
  if (!pwasm_env_init(&env, &mem_ctx, &cbs, &stack, NULL)) {
    cli_test_error(test_ctx, "pwasm_env_init() failed");
    return;
  }

  // build buffer
  const pwasm_buf_t buf = { AOT_BASICS_WASM, sizeof(AOT_BASICS_WASM) };

  // parse mod, check for error
  pwasm_mod_t mod;
  if (!pwasm_mod_init(&mem_ctx, &mod, buf)) {
    cli_test_error(test_ctx, "pwasm_mod_init() failed");
    return;
  }

  // add mod to env, check for error
  if (!pwasm_env_add_mod(&env, "aot-basics", &mod)) {
    cli_test_error(test_ctx, "pwasm_env_add_mod() failed");
    return;
  }

  {
    // call "add_i32s"
    const bool call_ok = pwasm_call(&env, "aot-basics", "add_i32s");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "add_i32s: pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "add_i32s: pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "add_i32s: stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "add_i32s: stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 579)) {
      cli_test_pass(test_ctx, cli_test, "add_i32s: result: 579");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "add_i32s: result: got %u, expected 579", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // call "trap", expect failure
    const bool call_ok = pwasm_call(&env, "aot-basics", "trap");
    if (call_ok) {
      cli_test_fail(test_ctx, cli_test, "trap: pwasm_call()");
    } else {
      cli_test_pass(test_ctx, cli_test, "trap: pwasm_call()");
    }
  }

  {
    // clear stack
    stack.pos = 0;

    // call "if_else_true"
    const bool call_ok = pwasm_call(&env, "aot-basics", "if_else_true");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "if_else_true: pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "if_else_true: pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "if_else_true: stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_else_true: stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 321)) {
      cli_test_pass(test_ctx, cli_test, "if_else_true: result: 321");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_else_true: result: got %u, expected 321", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // clear stack
    stack.pos = 0;

    // call "if_else_false"
    const bool call_ok = pwasm_call(&env, "aot-basics", "if_else_false");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "if_else_false: pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "if_else_false: pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "if_else_false: stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_else_false: stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 45)) {
      cli_test_pass(test_ctx, cli_test, "if_else_false: result: 45");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_else_false: result: got %u, expected 45", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // clear stack
    stack.pos = 0;

    // call "if_true"
    const bool call_ok = pwasm_call(&env, "aot-basics", "if_true");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "if_true: pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "if_true: pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "if_true: stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_true: stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 314159)) {
      cli_test_pass(test_ctx, cli_test, "if_true: result: 314159");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_true: result: got %u, expected 314159", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // clear stack
    stack.pos = 0;

    // call "if_false"
    const bool call_ok = pwasm_call(&env, "aot-basics", "if_false");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "if_false: pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "if_false: pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "if_false: stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_false: stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 22)) {
      cli_test_pass(test_ctx, cli_test, "if_false: result: 22");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "if_false: result: got %u, expected 22", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // clear stack
    stack.pos = 0;

    // call "if_false"
    const bool call_ok = pwasm_call(&env, "aot-basics", "br_outer");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "br_outer: pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "br_outer: pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "br_outer: stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "br_outer: stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 1234)) {
      cli_test_pass(test_ctx, cli_test, "br_outer: result: 1234");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "br_outer: result: got %u, expected 1234", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // clear stack
    stack.pos = 0;

    // call "if_false"
    const bool call_ok = pwasm_call(&env, "aot-basics", "br_inner");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "br_inner: pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "br_inner: pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "br_inner: stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "br_inner: stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 5678)) {
      cli_test_pass(test_ctx, cli_test, "br_inner: result: 5678");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "br_inner: result: got %u, expected 5678", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // populate paramters
    stack.pos = 2;
    stack.ptr[0].i32 = 99;
    stack.ptr[1].i32 = 77;

    // call "is_99"
    const bool call_ok = pwasm_call(&env, "aot-basics", "sub");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "sub(99, 77): pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "sub(99, 77): pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "sub(99, 77): stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "sub(99, 77): stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 22)) {
      cli_test_pass(test_ctx, cli_test, "sub(99, 77): result: 22");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "sub(99, 77): result: got %u, expected 22", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // populate paramters
    stack.pos = 1;
    stack.ptr[0].i32 = 99;

    // call "is_99"
    const bool call_ok = pwasm_call(&env, "aot-basics", "is_99");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "is_99(99): pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "is_99(99): pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "is_99(99): stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "is_99(99): stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 1)) {
      cli_test_pass(test_ctx, cli_test, "is_99(99): result: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "is_99(99): result: got %u, expected 1", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }

  {
    // populate paramters
    stack.pos = 1;
    stack.ptr[0].i32 = 2;

    // call "is_99"
    const bool call_ok = pwasm_call(&env, "aot-basics", "is_99");
    if (call_ok) {
      cli_test_pass(test_ctx, cli_test, "is_99(2): pwasm_call()");
    } else {
      cli_test_fail(test_ctx, cli_test, "is_99(2): pwasm_call()");
    }

    if (call_ok && (stack.pos == 1)) {
      cli_test_pass(test_ctx, cli_test, "is_99(2): stack size: 1");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "is_99(2): stack size: got %zu, expected 1", stack.pos);
      cli_test_fail(test_ctx, cli_test, buf);
    }

    if (call_ok && (stack.pos == 1) && (stack.ptr[0].i32 == 0)) {
      cli_test_pass(test_ctx, cli_test, "is_99(2): result: 0");
    } else {
      char buf[512];
      snprintf(buf, sizeof(buf), "is_99(2): result: got %u, expected 0", stack.ptr[0].i32);
      cli_test_fail(test_ctx, cli_test, buf);
    }
  }
}
