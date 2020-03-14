#include <stdbool.h> // bool
#include <stdint.h>  // uint32_t, int32_t, etc
#include <string.h> // memcmp()
#include "pt-wasm.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

#define PT_WASM_SECTION_TYPE(a, b) b,
static const char *PT_WASM_SECTION_TYPE_NAMES[] = {
PT_WASM_SECTION_TYPES
};
#undef PT_WASM_SECTION_TYPE

const char *
pt_wasm_section_type_get_name(
  const pt_wasm_section_type_t type
) {
  const size_t ofs = MIN(PT_WASM_SECTION_TYPE_LAST, type);
  return PT_WASM_SECTION_TYPE_NAMES[ofs];
}


static inline size_t
pt_wasm_decode_u32(
  uint32_t * const dst,
  const void * const src_ptr,
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
  if (cbs && cbs->on_error) { \
    cbs->on_error(msg, cb_data); \
  } \
  return false; \
} while (0)

static bool
pt_wasm_parse_section(
  const pt_wasm_parse_cbs_t * const cbs,
  const pt_wasm_section_type_t sec_type,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  (void) cbs;
  (void) sec_type;
  (void) src;
  (void) src_len;
  (void) cb_data;

  // return success
  return true;
}

static const uint8_t WASM_HEADER[] = "\0asm\1\0\0\0";

bool
pt_wasm_parse(
  const void * const src_ptr,
  const size_t src_len,
  const pt_wasm_parse_cbs_t * const cbs,
  void * const cb_data
) {
  const uint8_t * const src = src_ptr;

  // check length
  if (src_len < 5) {
    FAIL("module too small");
  }

  // check magic and version
  if (memcmp(src, WASM_HEADER, sizeof(WASM_HEADER))) {
    FAIL("invalid module header");
  }

  for (size_t ofs = 8; ofs < src_len;) {
    // parse section type, check for error
    const pt_wasm_section_type_t sec_type = src[ofs];
    if (sec_type >= PT_WASM_SECTION_TYPE_LAST) {
      // FIXME: show section type?
      FAIL("invalid section type");
    }

    // calculate u32 offset and maximum length
    const uint8_t * const u32_ptr = src + ofs + 1;
    const size_t u32_len = src_len - ofs - 1;

    // parse section data length, check for error
    uint32_t data_len = 0;
    const size_t len_ofs = pt_wasm_decode_u32(&data_len, u32_ptr, u32_len);
    if (!len_ofs) {
      FAIL("invalid section length");
    }

    // is the on_section cb defined?
    // build section data pointer
    const uint8_t * const data_ptr = src + ofs + len_ofs;

    // parse section, check for error
    if (!pt_wasm_parse_section(cbs, sec_type, data_ptr, data_len, cb_data)) {
      // return failure
      return false;
    }
  }

  // return success
  return true;
}
