// check implementation of trunc_sat instructions
#include <stdint.h>

// clamp value to range
#define CLAMP(v, lo, hi) (((v) < (lo)) ? (lo) : (((v) > (hi)) ? (hi) : (v)))

uint32_t i32_trunc_sat_f32_u(const float a) {
  return (uint32_t) CLAMP(a, (float) 0UL, (float) UINT32_MAX);
}

uint32_t i32_trunc_sat_f64_u(const double a) {
  return (uint32_t) CLAMP(a, (float) 0UL, (float) UINT32_MAX);
}

uint64_t i64_trunc_sat_f32_u(const float a) {
  return (uint64_t) CLAMP(a, (float) 0ULL, (float) UINT64_MAX);
}

uint64_t i64_trunc_sat_f64_u(const double a) {
  return (uint64_t) CLAMP(a, (float) 0ULL, (float) UINT64_MAX);
}

