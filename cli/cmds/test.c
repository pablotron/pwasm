#include <stdlib.h> // size_t
#include <stdio.h> // fprintf()
#include <err.h> // errx
#include "../cmds.h" // cli_cmd_t, cli_get_cmds()
#include "../tests.h"

typedef struct {
  FILE *io;
  size_t num_passes;
  size_t num_fails;
} cmd_test_t;

static void
cmd_test_on_pass(
  cli_test_ctx_t * const ctx,
  const cli_test_t * const test,
  const char * const assertion
) {
  cmd_test_t * const data = cli_test_ctx_get_data(ctx);
  data->num_passes++;
  fprintf(data->io, "PASS,%s,%s,%s\n", test->suite, test->test, assertion);
}

static void
cmd_test_on_fail(
  cli_test_ctx_t * const ctx,
  const cli_test_t * const test,
  const char * const assertion
) {
  cmd_test_t * const data = cli_test_ctx_get_data(ctx);
  data->num_fails++;
  fprintf(data->io, "FAIL,%s,%s,%s\n", test->suite, test->test, assertion);
}

static void
cmd_test_on_error(
  cli_test_ctx_t * const ctx,
  const char * const text
) {
  (void) ctx;
  errx(EXIT_FAILURE, "%s", text);
}

static const cli_test_ctx_cbs_t TEST_CBS = {
  .on_pass  = cmd_test_on_pass,
  .on_fail  = cmd_test_on_fail,
  .on_error = cmd_test_on_error,
};

static void cmd_test_on_test(
  const cli_test_t * const test,
  void * const data
) {
  cli_test_ctx_t * const ctx = data;
  test->func(ctx, test);
}

int cmd_test(
  const int argc,
  const char ** argv
) {
  cmd_test_t data = {
    .io = stdout,
  };

  cli_test_ctx_t ctx = cli_test_ctx_init(&TEST_CBS, &data);

  // print header
  fprintf(data.io,"result,suite,test,assertion\n");
  cli_each_test(
    (argc > 1) ? (argc - 2) : 0,
    (argc > 1) ? argv + 2 : NULL,
    cmd_test_on_test,
    &ctx
  );

  // print summary
  const size_t sum = data.num_passes + data.num_fails;
  fprintf(data.io,"%zu/%zu\n", data.num_passes, sum);

  return (sum == data.num_passes) ? 0 : -1;
}
