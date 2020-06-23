#include <stdlib.h>
#include <string.h> // strlen()
#include "tests.h"

static const cli_test_t TESTS[] = {{
  .suite  = "cli",
  .test   = "null",
  .text   = "Test test suite itself.",
  .func   = test_cli_null,
}, {
  .suite  = "init",
  .test   = "mods",
  .text   = "Test mod parsing with pwasm_mod_init().",
  .func   = test_init_mods,
}, {
  .suite  = "native",
  .test   = "calls",
  .text   = "Test native function calls.",
  .func   = test_native_calls,
}, {
  .suite  = "wasm",
  .test   = "calls",
  .text   = "Test function calls into WASM modules.",
  .func   = test_wasm_calls,
}, {
  .suite  = "aot-jit",
  .test   = "call",
  .text   = "Test DynASM AOT JIT compiler.",
  .func   = test_aot_jit,
}};

cli_test_ctx_t cli_test_ctx_init(
  const cli_test_ctx_cbs_t * const cbs,
  void *data
) {
  return (cli_test_ctx_t) {
    .cbs  = cbs,
    .data = data,
  };
}

void *cli_test_ctx_get_data(
  const cli_test_ctx_t * const ctx
) {
  return ctx->data;
}

void cli_test_pass(
  cli_test_ctx_t * const ctx, 
  const cli_test_t * const test, 
  const char * const assertion
) {
  if (ctx->cbs && ctx->cbs->on_pass) {
    ctx->cbs->on_pass(ctx, test, assertion);
  }
}

void cli_test_fail(
  cli_test_ctx_t * const ctx, 
  const cli_test_t * const test, 
  const char * const assertion
) {
  if (ctx->cbs && ctx->cbs->on_fail) {
    ctx->cbs->on_fail(ctx, test, assertion);
  }
}

void cli_test_error(
  cli_test_ctx_t * const ctx, 
  const char * const text
) {
  if (ctx->cbs && ctx->cbs->on_error) {
    ctx->cbs->on_error(ctx, text);
  }
}

void cli_each_test(
  const int argc,
  const char **argv,
  void (*on_test)(const cli_test_t *, void *),
  void *data
) {
  const char * const suite = (argc) ? argv[0] : NULL;
  const char * const test = (argc > 1) ? argv[1] : NULL;
  const size_t suite_len = (argc) ? strlen(argv[0]) : 0;
  const size_t test_len = (argc > 1) ? strlen(argv[1]) : 0;

  for (size_t i = 0; i < sizeof(TESTS) / sizeof(TESTS[0]); i++) {
    if (
      (!suite || !strncmp(suite, TESTS[i].suite, suite_len)) &&
      (!test || !strncmp(test, TESTS[i].test, test_len))
    ) {
      // emit matching test
      on_test(TESTS + i, data);
    }
  }
}
