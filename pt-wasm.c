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

// FIXME: limit to DEBUG
#include <stdio.h>
#define D(fmt, ...) fprintf( \
  stderr, \
  "%s:%d:%s(): " fmt "\n", \
  __FILE__, __LINE__, __func__, __VA_ARGS__ \
)

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

// static size_t
// pt_wasm_decode_i32(
//   int32_t * const dst,
//   void * const src_ptr,
//   const size_t src_len
// ) {
//   const size_t num_bits = sizeof(int32_t) * 8;
//   const uint8_t * const src = src_ptr;
//   uint64_t shift = 0;
//   int64_t val = 0;
// 
//   for (size_t i = 0; i < MIN(5, src_len); i++, shift += 7) {
//     const uint64_t b = src[i];
//     val |= (b & 0x7F) << shift;
// 
//     if (!(b & 0x80)) {
//       if ((shift < num_bits) && (b & 0x40)) {
//         val |= ~((((uint64_t) 1) << (shift + 7)) - 1);
//       }
// 
//       if (dst) {
//         // write result
//         *dst = val;
//       }
// 
//       // return length (success)
//       return i + 1;
//     }
//   }
// 
//   // return zero (failure)
//   return 0;
// }
// 
// static size_t
// pt_wasm_decode_i64(
//   int64_t * const dst,
//   void * const src_ptr,
//   const size_t src_len
// ) {
//   const size_t num_bits = sizeof(int64_t) * 8;
//   const uint8_t * const src = src_ptr;
//   uint64_t shift = 0;
//   int64_t val = 0;
// 
//   for (size_t i = 0; i < MIN(10, src_len); i++, shift += 7) {
//     const uint64_t b = src[i];
//     val |= (b & 0x7F) << shift;
// 
//     if (!(b & 0x80)) {
//       if ((shift < num_bits) && (b & 0x40)) {
//         val |= ~((((uint64_t) 1) << (shift + 7)) - 1);
//       }
// 
//       if (dst) {
//         // write result
//         *dst = val;
//       }
// 
//       // return length (success)
//       return i + 1;
//     }
//   }
// 
//   // return zero (failure)
//   return 0;
// }

#define NAME_FAIL(msg) do { \
  if (cbs && cbs->on_error) { \
    cbs->on_error(msg, cb_data); \
  } \
  return 0; \
} while (0)

static size_t
pt_wasm_parse_name(
  pt_wasm_buf_t * ret_buf,
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (!src_len) {
    NAME_FAIL("empty custom section name");
  }

  // decode name length, check for error
  uint32_t len = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&len, src, src_len);
  if (!len_ofs) {
    NAME_FAIL("bad custom section name length");
  }

  // D("src: %p, src_len = %zu, len = %u, len_ofs = %zu", src, src_len, len, len_ofs);

  // calculate total length, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src_len) {
    NAME_FAIL("truncated custom section name");
  }

  // build result
  const pt_wasm_buf_t buf = {
    .ptr = src + len_ofs,
    .len = len,
  };

  if (ret_buf) {
    // save to result
    *ret_buf = buf;
  }

  // return section length, in bytes
  return num_bytes;
}

/**
 * Is this value a valid value type?
 *
 * From section 5.3.1 of the WebAssembly documentation.
 */
static inline bool
pt_wasm_is_valid_value_type(
  const uint8_t v
) {
  return ((v == 0x7F) || (v == 0x7E) || (v == 0x7D) || (v == 0x7C));
}

/**
 * Is this value a valid result type?
 *
 * From section 5.3.2 of the WebAssembly documentation.
 */
static inline bool
pt_wasm_is_valid_result_type(
  const uint8_t v
) {
  return ((v == 0x40) || pt_wasm_is_valid_value_type(v));
}

#define VALUE_TYPE_LIST_FAIL(msg) do { \
  if (cbs && cbs->on_error) { \
    cbs->on_error(msg, cb_data); \
  } \
  return 0; \
} while (0)

static size_t
pt_wasm_parse_value_type_list(
  pt_wasm_buf_t * const ret_buf,
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (!src_len) {
    VALUE_TYPE_LIST_FAIL("empty value type list");
  }

  // decode buffer length, check for error
  uint32_t len = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&len, src, src_len);
  if (!len_ofs) {
    VALUE_TYPE_LIST_FAIL("bad value type list length");
  }

  // calculate total number of bytes, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src_len) {
    VALUE_TYPE_LIST_FAIL("value type list length too long");
  }

  // build result
  const pt_wasm_buf_t buf = {
    .ptr = src + len_ofs,
    .len = len,
  };

  // check value types
  for (size_t i = 0; i < buf.len; i++) {
    // D("buf[%zu] = %02x", i, buf.ptr[i]);
    if (!pt_wasm_is_valid_value_type(buf.ptr[i])) {
      // invalid value type, return error
      VALUE_TYPE_LIST_FAIL("bad value type list entry");
    }
  }

  if (ret_buf) {
    // save to result
    *ret_buf = buf;
  }

  // return section length, in bytes
  return num_bytes;
}

#define FAIL(msg) do { \
  if (cbs && cbs->on_error) { \
    cbs->on_error(msg, cb_data); \
  } \
  return false; \
} while (0)

static bool
pt_wasm_parse_custom_section(
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // parse name, check for error
  pt_wasm_buf_t name;
  const size_t ofs = pt_wasm_parse_name(&name, cbs, src, src_len, cb_data);
  if (!ofs) {
    return false;
  }

  // build custom section
  const pt_wasm_custom_section_t section = {
    .name = {
      .ptr = name.ptr,
      .len = name.len,
    },

    .data = {
      .ptr = src + ofs,
      .len = src_len - ofs,
    },
  };

  if (cbs && cbs->on_custom_section) {
    // pass to callback
    cbs->on_custom_section(&section, cb_data);
  }

  // return success
  return true;
}

/**
 * Parse function type declaration into +dst_func_type+ from source
 * buffer +src+, consuming a maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_function_type(
  pt_wasm_function_type_t * const dst_func_type,
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check length
  if (!src_len) {
    FAIL("empty function type");
  }

  // check leading byte
  if (src[0] != 0x60) {
    FAIL("invalid function type header");
  }

  // check function type has space for parameters
  if (src_len < 2) {
    FAIL("bad function type: missing parameters");
  }

  // parse params, check for error
  pt_wasm_buf_t params;
  const size_t params_len = pt_wasm_parse_value_type_list(&params, cbs, src + 1, src_len - 1, cb_data);
  if (!params_len) {
    return 0;
  }

  // build results offset, check for error
  const size_t results_ofs = 1 + params_len;
  if (results_ofs >= src_len) {
    FAIL("bad function type: missing results");
  }

  // parse params, check for error
  pt_wasm_buf_t results;
  const size_t results_len = pt_wasm_parse_value_type_list(&results, cbs, src + results_ofs, src_len - results_ofs, cb_data);
  if (!results_len) {
    return 0;
  }

  // build result
  const pt_wasm_function_type_t src_func_type = {
    .params = {
      .ptr = params.ptr,
      .len = params.len,
    },

    .results = {
      .ptr = results.ptr,
      .len = results.len,
    },
  };

  // save result
  *dst_func_type = src_func_type;

  // return total number of bytes
  return results_ofs + results_len;
}

// number of function types (NOTE: must be power of two)
#define PT_WASM_FUNCTION_TYPE_SET_SIZE 128

static bool
pt_wasm_parse_type_section(
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // get number of types, check for error
  uint32_t num_types = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_types, src, src_len);
  if (!len_ofs) {
    FAIL("invalid type section vector length");
  }

  pt_wasm_function_type_t types[PT_WASM_FUNCTION_TYPE_SET_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_types; i++) {
    const size_t types_ofs = (i & (PT_WASM_FUNCTION_TYPE_SET_SIZE - 1));
    // parse function type, check for error
    const size_t type_len = pt_wasm_parse_function_type(
      types + types_ofs,
      cbs,
      src + ofs,
      src_len - ofs,
      cb_data
    );

    if (!type_len) {
      // return failure
      return false;
    }

    // increment offset, check for error
    ofs += type_len;
    if (ofs > src_len) {
      FAIL("type section length overflow");
    }

    if (types_ofs == (sizeof(types) - sizeof(types[0]))) {
      if (cbs && cbs->on_function_types) {
        cbs->on_function_types(types, types_ofs, cb_data);
      }
    }
  }

  // flush remaining function types
  const size_t num_left = num_types % PT_WASM_FUNCTION_TYPE_SET_SIZE;
  if (num_left && cbs && cbs->on_function_types) {
    cbs->on_function_types(types, num_left, cb_data);
  }

  // return success
  return true;
}

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

  switch (sec_type) {
  case PT_WASM_SECTION_TYPE_CUSTOM:
    return pt_wasm_parse_custom_section(cbs, src, src_len, cb_data);
    break;
  case PT_WASM_SECTION_TYPE_TYPE:
    return pt_wasm_parse_type_section(cbs, src, src_len, cb_data);
    break;
  default:
    break;
  }

  // return success
  return true;
}

static const uint8_t WASM_HEADER[] = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };

bool
pt_wasm_parse(
  const void * const src_ptr,
  const size_t src_len,
  const pt_wasm_parse_cbs_t * const cbs,
  void * const cb_data
) {
  const uint8_t * const src = src_ptr;

  // check length
  if (src_len < 8) {
    FAIL("module too small");
  }

  // fprintf(stderr," sizeof(WASM_HEADER) = %zu\n", sizeof(WASM_HEADER));
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

    // check to make sure u32 ptr doesn't exceed input size
    if (ofs + 1 >= src_len) {
      FAIL("truncated section size");
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

    // check total length to make sure it doesn't exceed size
    if (ofs + 1 + len_ofs + data_len > src_len) {
      FAIL("truncated section");
    }

    // is the on_section cb defined?
    // build section data pointer
    const uint8_t * const data_ptr = src + ofs + 1 + len_ofs;

    // parse section, check for error
    if (!pt_wasm_parse_section(cbs, sec_type, data_ptr, data_len, cb_data)) {
      // return failure
      return false;
    }

    // increment source offset
    ofs += 1 + len_ofs + data_len;
  }

  // return success
  return true;
}
