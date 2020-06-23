#ifndef CLI_TESTS_H
#define CLI_TESTS_H

typedef struct cli_test_t cli_test_t;
typedef struct cli_test_ctx_t cli_test_ctx_t;

struct cli_test_t {
  const char * const suite; // test suite name
  const char * const test;  // test name
  const char * const text;  // short test description
  void (*func)(cli_test_ctx_t *, const cli_test_t *);
};

typedef struct {
  // invoked when a test passes
  void (*on_pass)(
    cli_test_ctx_t *, // ctx
    const cli_test_t *, // suite and test
    const char *  // assertion
  );

  // invoked when a test fails
  void (*on_fail)(
    cli_test_ctx_t *, // ctx
    const cli_test_t *, // test
    const char *  // assertion
  );

  // invoked when an error occurs
  void (*on_error)(
    cli_test_ctx_t *, // ctx
    const char * // text
  );
} cli_test_ctx_cbs_t;

struct cli_test_ctx_t {
  const cli_test_ctx_cbs_t * const cbs;
  void *data;
};

cli_test_ctx_t cli_test_ctx_init(const cli_test_ctx_cbs_t *, void *);
void *cli_test_ctx_get_data(const cli_test_ctx_t *);
void cli_test_pass(cli_test_ctx_t *, const cli_test_t *, const char *);
void cli_test_fail(cli_test_ctx_t *, const cli_test_t *, const char *);
void cli_test_error(cli_test_ctx_t *, const char *);

typedef void (*cli_test_cb_t)(cli_test_ctx_t *, const cli_test_t *);

void cli_each_test(
  const int,
  const char **,
  void (*)(const cli_test_t *, void *),
  void *
);

void test_cli_null(cli_test_ctx_t *, const cli_test_t *);
void test_init_mods(cli_test_ctx_t *, const cli_test_t *);
void test_native_calls(cli_test_ctx_t *, const cli_test_t *);
void test_wasm_calls(cli_test_ctx_t *, const cli_test_t *);
void test_aot_jit(cli_test_ctx_t *, const cli_test_t *);
// TODO: void test_aot_init(cli_test_ctx_t *, const cli_test_t *);
// TODO: void test_aot_calls(cli_test_ctx_t *, const cli_test_t *);

#endif /* CLI_TESTS_H */
