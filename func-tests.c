#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include "func-tests.h"

#define NUM_ITEMS(ary) (sizeof(ary) / sizeof(ary[0]))

static const uint8_t DATA[] = {
  // truncated (fail, ofs: 0, len: 1)
  0x00,

  // end (pass, ofs: 1, len, 2)
  0x00, 0x0B,

  // add i32s (pass, ofs: 3, len, 7)
  0x00, 0x41, 0x01, 0x41, 0x02, 0x6A, 0x0B,

  // mul i32s (pass, ofs: 10, len, 7)
  0x00, 0x41, 0x06, 0x41, 0x07, 0x6C, 0x0B,
};

static const test_t TESTS[] = {
  { "short length",                       false,    0,    0 },
  { "truncated",                          false,    0,    1 },
  { "end",                                true,     1,    2 },
  { "add i32s",                           true,     3,    7 },
  { "mul i32s",                           true,    10,    7 },
};

static const suite_t SUITE = {
  .num_tests  = NUM_ITEMS(TESTS),
  .tests      = TESTS,
  .data       = DATA,
};

suite_t get_func_tests(void) {
  return SUITE;
}
