#include <stdlib.h>
#include <stdint.h>

typedef struct {
  uint64_t a, b, c;
  uint32_t d, e, f;
} test_t;

uint64_t do_stuff(uint32_t a, uint32_t b, const test_t t) {
  return a + b + t.a + t.c;
}

int main(int argc, char *argv[]) {
  (void) argc;
  (void) argv;

  test_t t = { .a = 12, .c = 34, .e = 56 };
  do_stuff(22, 15, t);

  return 0;
}
