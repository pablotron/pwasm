#include <stdbool.h> // bool
#include <stdint.h>  // uint32_t, int32_t, etc
#include <string.h> // memcmp()
#include "pt-wasm.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

static inline size_t
pt_wasm_decode_u32(
  uint32_t * const dst,
  void * const src_ptr,
  const size_t src_len
) {
  const uint8_t * const src = src_ptr;
  uint64_t val = 0, shift = 0;

  for (size_t i = 0; i < MIN(5, src_len); i++) {
    const uint64_t b = src[i];
    val |= ((b & 0x7F) << shift);

    if (!(b & 0x80)) {
      if (dst) {
        // write result
        *dst = val;
      }

      // return length (success)
      return i + 1;
    }

    shift += 7;
  }

  // return zero (failure)
  return 0;
}

static inline size_t
pt_wasm_decode_u64(
  uint64_t * const dst,
  void * const src_ptr,
  const size_t src_len
) {
  const uint8_t * const src = src_ptr;
  uint64_t val = 0, shift = 0;

  for (size_t i = 0; i < MIN(10, src_len); i++) {
    const uint64_t b = src[i];
    val |= ((b & 0x7F) << shift);

    if (!(b & 0x80)) {
      if (dst) {
        // write result
        *dst = val;
      }

      // return length (success)
      return i + 1;
    }

    shift += 7;
  }

  // return zero (failure)
  return 0;
}

static size_t
pt_wasm_decode_i32(
  int32_t * const dst,
  void * const src_ptr,
  const size_t src_len
) {
  const size_t num_bits = sizeof(int32_t) * 8;
  const uint8_t * const src = src_ptr;
  uint64_t shift = 0;
  int64_t val = 0;

  for (size_t i = 0; i < MIN(5, src_len); i++, shift += 7) {
    const uint64_t b = src[i];
    val |= (b & 0x7F) << shift;

    if (!(b & 0x80)) {
      if ((shift < num_bits) && (b & 0x40)) {
        val |= ~((((uint64_t) 1) << (shift + 7)) - 1);
      }

      if (dst) {
        // write result
        *dst = val;
      }

      // return length (success)
      return i + 1;
    }
  }

  // return zero (failure)
  return 0;
}

static size_t
pt_wasm_decode_i64(
  int64_t * const dst,
  void * const src_ptr,
  const size_t src_len
) {
  const size_t num_bits = sizeof(int64_t) * 8;
  const uint8_t * const src = src_ptr;
  uint64_t shift = 0;
  int64_t val = 0;

  for (size_t i = 0; i < MIN(10, src_len); i++, shift += 7) {
    const uint64_t b = src[i];
    val |= (b & 0x7F) << shift;

    if (!(b & 0x80)) {
      if ((shift < num_bits) && (b & 0x40)) {
        val |= ~((((uint64_t) 1) << (shift + 7)) - 1);
      }

      if (dst) {
        // write result
        *dst = val;
      }

      // return length (success)
      return i + 1;
    }
  }

  // return zero (failure)
  return 0;
}

#define FAIL(msg) do { \
  if (cbs->on_error) { \
    cbs->on_error(msg, data); \
  } \
  return false; \
} while (0)

static const uint8_t WASM_HEADER[] = "\0asm\1\0\0\0";

bool
pt_wasm_parse(
  const void * const src_ptr,
  const size_t src_len,
  const pt_wasm_parse_cbs_t * const cbs,
  void * const data
) {
  const uint8_t * const src = src_ptr;

  // check length
  if (src_len < 5) {
    FAIL("module too small");
  }

  // check magic and version
  if (memcmp(src, WASM_HEADER, sizeof(WASM_HEADER))) {
    FAIL("bad module header");
  }

  // return success
  return true;
}
