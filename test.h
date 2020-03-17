#ifndef TEST_H
#define TEST_H

#include <stddef.h> // size_t

typedef struct {
  char *name;
  _Bool want;
  size_t ofs;
  size_t len;
} test_t;

typedef struct {
  const size_t num_tests;
  const test_t * const tests;
  const uint8_t * const data;
} suite_t;

#endif /* TEST_H */
