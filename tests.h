#ifndef TESTS_H
#define TESTS_H

#include <stddef.h> // size_t

typedef struct {
  char *name;
  _Bool want;
  size_t ofs;
  size_t len;
} test_t;

size_t get_num_tests(void);
const test_t *get_tests(void);
const uint8_t *get_test_data(void);

#endif /* TESTS_H */
