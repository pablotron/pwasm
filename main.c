#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include <stdlib.h> // malloc()
#include <string.h> // strlen()
#include <stdio.h> // fopen()
#include <err.h> // err()
#include "pt-wasm.h"

#define NUM_ITEMS(ary) (sizeof(ary) / sizeof(ary[0]))

static const uint8_t
TEST_DATA[] = {
  // bad header
  0x01, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,

  // good header
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
};

typedef struct {
  char *name;
  bool want;
  size_t ofs;
  size_t len;
} test_t;

static const test_t TESTS[] = {{
  .name = "short length",
  .want = false,
  .ofs  = 0,
  .len  = 0,
}, {
  .name = "bad header",
  .want = false,
  .ofs  = 0,
  .len  = 8,
}, {
  .name = "good header",
  .want = true,
  .ofs  = 8,
  .len  = 8,
}};

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

static void
run_tests(void) {
  const size_t NUM_TESTS = NUM_ITEMS(TESTS);
  size_t num_fails = 0;
  for (size_t i = 0; i < NUM_TESTS; i++) {
    const test_t * const test = TESTS + i;
    const bool ok = (test->want == pt_wasm_parse(TEST_DATA + test->ofs, test->len, NULL, NULL));
    num_fails += ok ? 0 : 1;
    if (!ok) {
      warnx("FAIL: %s", test->name);
    }
  }

  printf("Tests: %zu/%zu\n", NUM_TESTS - num_fails, NUM_TESTS);
  if (num_fails > 0) {
    exit(EXIT_FAILURE);
  }
}

int main(int argc, char *argv[]) {
  run_tests();
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
