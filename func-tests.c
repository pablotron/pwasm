#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include "func-tests.h"

#define NUM_ITEMS(ary) (sizeof(ary) / sizeof(ary[0]))

static const uint8_t DATA[] = {
  // bad header (fail, ofs: 0, len: 8)
};

static const test_t TESTS[] = {
  { "short length",                       false,    0,    0 },
};

static const suite_t SUITE = {
  .num_tests  = NUM_ITEMS(TESTS),
  .tests      = TESTS,
  .data       = DATA,
};

suite_t get_func_tests(void) {
  return SUITE;
}
