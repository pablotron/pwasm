#include <stdint.h>

uint32_t shift(const uint32_t a, const uint32_t b) {
  return a << b;
}

int main(int argc, char *argv[]) {
  (void) argc;
  (void) argv;
  return shift(22, 32);
}
