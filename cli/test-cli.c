#include "cli-tests.h"
#include "../pwasm.h"

void test_cli_null(
  cli_test_ctx_t * const test_ctx,
  const cli_test_t * const cli_test
) {
  cli_test_pass(test_ctx, cli_test, "null test");
}
