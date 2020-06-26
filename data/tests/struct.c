#include <stdlib.h>
#include <stdint.h>

typedef struct {
  uint64_t a, b;
} test_t;

uint64_t do_stuff(uint32_t a, uint32_t b, const test_t t) {
  return a + b + t.a + t.b;
}

int main(int argc, char *argv[]) {
  (void) argc;
  (void) argv;

  test_t t = { .a = 12, .b = 34 };
  do_stuff(22, 15, t);

  return 0;
}
