#include <stdbool.h> // bool
#include <string.h> // memcmp()
#include <stdlib.h> // realloc()
#include <unistd.h> // sysconf()
#include <string.h> // snprintf()
#include <math.h> // fabs(), fabsf(), etc
#include "pwasm.h"

/**
 * Batch size.
 *
 * Used to batch up function types, imports, functions, etc, when
 * dispatching to parsing callbacks.
 *
 * Note: must be a power of two.
 */
#define PWASM_BATCH_SIZE (1 << 7)

/**
 * Page size of WASM memory chunks.
 *
 * Note: Defined by the spec to be 2^16
 */
#define PWASM_PAGE_SIZE (1 << 16)

/**
 * Void block type.
 *
 * WASM 1.0 void block results were encoded with a single 0x40 byte,
 * which decodes to -64 using the newer s32 block type encoding.
 */
#define PWASM_BLOCK_TYPE_VOID -64

// return minimum of two values
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))

// clamp value to range
#define CLAMP(v, lo, hi) (((v) < (lo)) ? (lo) : (((v) > (hi)) ? (hi) : (v)))

// get the number of elements in a static array
#define LEN(ary) (sizeof(ary) / sizeof((ary)[0]))

#ifdef PWASM_DEBUG
#include <stdio.h>
#define D(fmt, ...) fprintf( \
  stderr, \
  "D %s:%d %s(): " fmt "\n", \
  __FILE__, __LINE__, __func__, __VA_ARGS__ \
)
#else
#define D(fmt, ...)
#endif /* PWASM_DEBUG */

/**
 * Create new buffer by advancing existing buffer the given number of
 * bytes.
 */
static inline pwasm_buf_t
pwasm_buf_step(
  const pwasm_buf_t src,
  const size_t ofs
) {
  return (pwasm_buf_t) { src.ptr + ofs, src.len - ofs };
}

/**
 * Convert null-terminated string to pwasm_buf_t.
 *
 * Note: Returns a buffer that points to NULL and has a length of zero
 * if passed a NULL pointer.
 */
static inline pwasm_buf_t
pwasm_buf_str(
  const char * const s
) {
  return (pwasm_buf_t) {
    .ptr = (uint8_t*) (s ? s : NULL),
    .len = s ? strlen(s) : 0,
  };
}

/**
 * Get the size (in bytes) of the UTF-8 codepoint beginning with the
 * given byte.
 */
static inline size_t
pwasm_utf8_get_codepoint_size(
  const uint8_t c
) {
  return (
    (((c & 0x80) == 0x00) ? 1 : 0) |
    (((c & 0xE0) == 0xC0) ? 2 : 0) |
    (((c & 0xF0) == 0xE0) ? 3 : 0) |
    (((c & 0xF8) == 0xF0) ? 4 : 0)
  );
}

// is continuing byte
#define IS_CB(b) (((b) & 0xC0) == 0x80)

// cast, mask, and shift byte
#define CMS(val, mask, shift) (((uint32_t) ((val) & (mask))) << (shift))

/**
 * Decode the UTF-8 codepoint of length +len+ from the string pointed to
 * by +s+.
 *
 * Returns 0xFFFFFFFF if the length or the continuation bytes are
 * invalid.
 */
static inline uint32_t
pwasm_utf8_get_codepoint(
  const uint8_t * const s,
  const size_t len
) {
  if (len == 1) {
    return s[0];
  } else if ((len == 2) && IS_CB(s[1])) {
    return (
      CMS(s[0], 0x1F, 6) |
      CMS(s[1], 0x3F, 0)
    );
  } else if ((len == 3) && IS_CB(s[1]) && IS_CB(s[2])) {
    return (
      CMS(s[0], 0x0F, 12) |
      CMS(s[1], 0x3F,  6) |
      CMS(s[2], 0x3F,  0)
    );
  } else if ((len == 4) && IS_CB(s[1]) && IS_CB(s[2]) && IS_CB(s[3])) {
    return (
      CMS(s[0], 0x03, 18) |
      CMS(s[1], 0x3F, 12) |
      CMS(s[2], 0x3F,  6) |
      CMS(s[3], 0x3F,  0)
    );
  } else {
    // return invalid codepoint
    return 0xFFFFFFFF;
  }
}
#undef IS_CB
#undef CMS

/**
 * Returns true if the given buffer contains a sequence of valid UTF_8
 * codepoints, and false otherwise.
 */
static inline bool
pwasm_utf8_is_valid(
  const pwasm_buf_t src
) {
  for (size_t i = 0; i < src.len;) {
    // get length of next utf-8 codepoint (in bytes), check for error
    const size_t len = pwasm_utf8_get_codepoint_size(src.ptr[i]);
    if (!len) {
      // invalid codepoint, return failure
      return false;
    }

    // make sure codepoint isn't truncated
    if (i + len > src.len) {
      // truncated codepoint, return failure
      return false;
    }

    // decode/check codepoint
    const uint32_t code = pwasm_utf8_get_codepoint(src.ptr + i, len);
    if (code > 0x1FFFFF) {
      // invalid codepoint, return failure
      return false;
    }

    // skip decoded bytes
    i += len;
  }

  // return success
  return true;
}

/**
 * Decode the LEB128-encoded unsigned 32-bit integer at the beginning of
 * the buffer +src+ and return the value in +dst+.
 *
 * Returns the number of bytes consumed, or 0 on error.
 */
static inline size_t
pwasm_u32_decode(
  uint32_t * const dst,
  const pwasm_buf_t src
) {
  const size_t len = MIN(5, src.len);

  if (dst) {
    uint32_t val = 0, shift = 0;

    for (size_t i = 0; i < len; i++) {
      const uint32_t b = src.ptr[i];
      val |= ((b & 0x7F) << shift);

      if (!(b & 0x80)) {
        // write result
        *dst = val;

        // return length (success)
        return i + 1;
      }

      shift += 7;
    }
  } else {
    // no destination, just count size

    for (size_t i = 0; i < len; i++) {
      const uint32_t b = src.ptr[i];

      if (!(b & 0x80)) {
        // return length (success)
        return i + 1;
      }
    }
  }

  // return zero (failure)
  return 0;
}

/**
 * Decode the LEB128-encoded unsigned 64-bit integer at the beginning of
 * the buffer +src+ and return the value in +dst+.
 *
 * Returns the number of bytes consumed, or 0 on error.
 */
static inline size_t
pwasm_u64_decode(
  uint64_t * const dst,
  const pwasm_buf_t src
) {
  const size_t len = MIN(10, src.len);

  if (dst) {
    uint64_t val = 0, shift = 0;

    for (size_t i = 0; i < len; i++) {
      const uint64_t b = src.ptr[i];
      val |= ((b & 0x7F) << shift);

      if (!(b & 0x80)) {
        // write result
        *dst = val;

        // return length (success)
        return i + 1;
      }

      shift += 7;
    }
  } else {
    // no destination, just count size

    for (size_t i = 0; i < len; i++) {
      const uint64_t b = src.ptr[i];

      if (!(b & 0x80)) {
        // return length (success)
        return i + 1;
      }
    }
  }

  // return zero (failure)
  return 0;
}

static void
pwasm_s32_dump_decode(
  const pwasm_buf_t src, // src buf
  const size_t used, // number of bytes consumed
  const int32_t val // result value
) {
  #ifdef PWASM_DEBUG
  char buf[512];
  size_t ofs = 0;

  for (size_t i = 0; i < used; i++) {
    if (i > 0) {
      memcpy(buf + ofs, ", ", 2);
      ofs += 2;
    }
    ofs += sprintf(buf, "0x%02x", src.ptr[i]);
  }
  buf[ofs] = 0;

  D("{ %s } -> val = %d", buf, val);

  #else /* PWASM_DEBUG */
  (void) src;
  (void) used;
  (void) val;
  #endif /* PWASM_DEBUG */
}

/**
 * Decode the LEB128-encoded signed 32-bit integer at the beginning of
 * the buffer +src+ and return the value in +dst+.
 *
 * Returns the number of bytes consumed, or 0 on error.
 */
static inline size_t
pwasm_s32_decode(
  int32_t * const dst,
  const pwasm_buf_t src
) {
  const size_t len = MIN(5, src.len);

  if (dst) {
    uint32_t val = 0, shift = 0;

    for (size_t i = 0; i < len; i++) {
      const uint32_t b = src.ptr[i];
      val |= ((b & 0x7F) << shift);

      shift += 7;

      if (!(b & 0x80)) {
        if ((shift < 32) && (b & 0x40)) {
          val |= (~0UL << shift);
        }

        // dump decoded result
        pwasm_s32_dump_decode(src, i + 1, val);

        // write result
        *dst = val;

        // return length (success)
        return i + 1;
      }
    }
  }

  // return zero (failure)
  return 0;
}

/**
 * Define a vector parser for the given name and type.
 *
 * This macro also defines a function named pwasm_mod_parse_{NAME}s,
 * which parses a vector of TYPE elements and passes batches of them to
 * the on_{NAME}s callback of the given callback structure.
 *
 * On error, returns 0 and calls the +on_error+ callback of the given
 * callback structure (if provided).
 *
 * On success, returns the number of bytes consumed.
 */
#define DEF_VEC_PARSER(NAME, TYPE) \
  /* forward declaration */ \
  static size_t \
  pwasm_mod_parse_ ## NAME ( \
    TYPE * const dst, \
    const pwasm_buf_t src, \
    const pwasm_mod_parse_cbs_t * const cbs, \
    void *cb_data \
  ); \
  \
  static void \
  pwasm_mod_parse_ ## NAME ## s_null_on_rows( \
    const TYPE * rows, \
    const size_t num, \
    void *cb_data \
  ) { \
    (void) rows; \
    (void) num; \
    (void) cb_data; \
  } \
  \
  static size_t \
  pwasm_mod_parse_ ## NAME ## s ( \
    const pwasm_buf_t src, \
    const pwasm_mod_parse_cbs_t * const cbs, \
    void *cb_data \
  ) { \
    void (*on_error)( \
      const char *, \
      void * \
    ) = cbs->on_error ? cbs->on_error : pwasm_null_on_error; \
    \
    void (*on_rows)( \
      const TYPE *, \
      const size_t, \
      void * \
    ) = (cbs->on_ ## NAME ## s) ? \
      (cbs->on_ ## NAME ## s) : \
      &pwasm_mod_parse_ ## NAME ## s_null_on_rows; \
    \
    pwasm_buf_t curr = src; \
    size_t num_bytes = 0; \
    \
    /* get count, check for error */ \
    uint32_t count = 0; \
    { \
      size_t len = pwasm_u32_decode(&count, src); \
      if (!len) { \
        on_error(#NAME "s: invalid count", cb_data); \
        return 0; \
      } \
      \
      /* advance */ \
      curr = pwasm_buf_step(curr, len); \
      num_bytes += len; \
    } \
    \
    D("count = %u", count); \
    \
    /* element buffer and offset */ \
    TYPE dst[PWASM_BATCH_SIZE]; \
    size_t ofs = 0; \
    \
    /* parse items */ \
    for (size_t i = 0; i < count; i++) { \
      /* check for underflow */ \
      if (!curr.len) { \
        on_error(#NAME "s: underflow", cb_data); \
        return 0; \
      } \
      \
      D("i = %zu/%u", i, count); \
      \
      /* parse element, check for error */ \
      const size_t len = pwasm_mod_parse_ ## NAME (dst + ofs, curr, cbs, cb_data); \
      if (!len) { \
        return 0; \
      } \
      \
      /* increment num_bytes, increment offset, advance buffer */ \
      curr = pwasm_buf_step(curr, len); \
      num_bytes += len; \
      \
      D("i = %zu/%u, ofs = %zu", i, count, ofs); \
      \
      ofs++; \
      if (ofs == LEN(dst)) { \
        D("flush: i = %zu/%u, ofs = %zu", i, count, ofs); \
        \
        /* flush batch */ \
        on_rows(dst, ofs, cb_data); \
        \
        ofs = 0; \
      } \
    } \
    \
    if (ofs > 0) { \
      D("flush (fini): count = %u, ofs = %zu", count, ofs); \
      /* flush remaining items */ \
      on_rows(dst, ofs, cb_data); \
      D("flush (fini, after): count = %u, ofs = %zu", count, ofs); \
    } \
    \
    D("count = %u, num_bytes = %zu", count, num_bytes); \
    \
    /* return number of bytes consumed */ \
    return num_bytes; \
  }

#define DEF_GET_NAMES(NAME_LC, NAME_UC) \
  const char * pwasm_ ## NAME_LC ## _get_name ( \
    const pwasm_ ## NAME_LC ## _t val \
  ) { \
    const size_t ofs = MIN(PWASM_ ## NAME_UC ## _LAST, val); \
    return PWASM_## NAME_UC ## _NAMES[ofs]; \
  }

#define PWASM_SECTION_TYPE(a, b) #b,
static const char *PWASM_SECTION_TYPE_NAMES[] = {
PWASM_SECTION_TYPES
};
#undef PWASM_SECTION_TYPE

DEF_GET_NAMES(section_type, SECTION_TYPE)

#define PWASM_IMPORT_TYPE(a, b, c) b,
static const char *PWASM_IMPORT_TYPE_NAMES[] = {
PWASM_IMPORT_TYPES
};
#undef PWASM_IMPORT_TYPE

DEF_GET_NAMES(import_type, IMPORT_TYPE)

/**
 * Parse import type into `dst` from source buffer `src`.
 *
 * Returns number of bytes consumed, or `0` on error.
 */
static size_t
pwasm_parse_import_type(
  pwasm_import_type_t * const dst,
  const pwasm_buf_t src
) {
  // check length
  if (!src.len) {
    // return failure
    return 0;
  }

  // get value, check for error
  const uint8_t val = src.ptr[0];
  if (val >= PWASM_IMPORT_TYPE_LAST) {
    // return failure
    return 0;
  }

  // save to destination, return number of bytes consumed
  *dst = val;
  return 1;
}

static const char *PWASM_IMM_NAMES[] = {
#define PWASM_IMM(a, b) b,
PWASM_IMM_DEFS
#undef PWASM_IMM
};

DEF_GET_NAMES(imm, IMM)

/**
 * Is this value a valid value type?
 *
 * From section 5.3.1 of the WebAssembly documentation.
 */
static inline bool
pwasm_is_valid_value_type(
  const uint8_t v
) {
  return ((v == 0x7F) || (v == 0x7E) || (v == 0x7D) || (v == 0x7C));
}

const char *
pwasm_value_type_get_name(
  const pwasm_value_type_t v
) {
  switch (v) {
#define PWASM_VALUE_TYPE(a, b, c) case a: return c;
PWASM_VALUE_TYPE_DEFS
#undef PWASM_VALUE_TYPE
  default:
    return pwasm_value_type_get_name(PWASM_VALUE_TYPE_LAST);
  }
}

/**
 * Kinds of block types.
 */
typedef enum {
  PWASM_BLOCK_TYPE_KIND_RESULT, //< Old single-result block.
  PWASM_BLOCK_TYPE_KIND_IMPORT, //< New multi-value block.
  PWASM_BLOCK_TYPE_KIND_LAST,
} pwasm_block_type_kind_t;

/**
 * Is the given block type an old-style result block type?
 */
static inline bool
pwasm_block_type_is_result_type(
  const int32_t block_type
) {
  return (block_type < 0) && (
    // i32, i64, f32, and f64
    (block_type >= -4) ||

    // void
    (block_type == PWASM_BLOCK_TYPE_VOID)
  );
}

/**
 * Is the given block type an new-style import block type?
 */
static inline bool
pwasm_block_type_is_import_type(
  const pwasm_mod_t * const mod,
  const int32_t block_type
) {
  return (block_type >= 0) && ((uint32_t) block_type < mod->num_types);
}

/**
 * Get the kind of the block type.
 *
 * Returns PWASM_BLOCK_TYPE_KIND_LAST on error.
 */
static inline pwasm_block_type_kind_t
pwasm_block_type_get_kind(
  const pwasm_mod_t * const mod,
  const int32_t block_type
) {
  if (pwasm_block_type_is_result_type(block_type)) {
    return PWASM_BLOCK_TYPE_KIND_RESULT;
  } else if (pwasm_block_type_is_import_type(mod, block_type)) {
    return PWASM_BLOCK_TYPE_KIND_IMPORT;
  } else {
    return PWASM_BLOCK_TYPE_KIND_LAST;
  }
}

/**
 * Convert an old-style result block type to a value type.
 *
 * Returns `PWASM_VALUE_TYPE_LAST` on error.
 */
static uint32_t
pwasm_block_type_result_get_value_type(
  const int32_t block_type
) {
  switch (block_type) {
  case -1: return PWASM_VALUE_TYPE_I32;
  case -2: return PWASM_VALUE_TYPE_I64;
  case -3: return PWASM_VALUE_TYPE_F32;
  case -4: return PWASM_VALUE_TYPE_F64;
  case -5: return PWASM_VALUE_TYPE_V128;
  default: return PWASM_VALUE_TYPE_LAST;
  }
}

#if 0
/**
 * Is this a valid block type?
 */
static inline bool
pwasm_block_type_is_valid(
  const pwasm_mod_t * const mod,
  const int32_t type
) {
  const pwasm_block_type_kind_t kind = pwasm_block_type_get_kind(mod, type);

  return (
    (kind == PWASM_BLOCK_TYPE_KIND_RESULT) ||
    (kind == PWASM_BLOCK_TYPE_KIND_IMPORT)
  );
}
#endif /* 0 */

/**
 * Get the number of parameters for the given block type.
 *
 * Returns `false` on error.
 */
bool
pwasm_block_type_params_get_size(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  size_t * const ret
) {
  switch (pwasm_block_type_get_kind(mod, block_type)) {
  case PWASM_BLOCK_TYPE_KIND_RESULT:
    if (ret) {
      // save result
      *ret = 0;
    }

    // return success
    return true;
  case PWASM_BLOCK_TYPE_KIND_IMPORT:
    if (ret) {
      // save result
      *ret = mod->types[block_type].params.len;
    }

    // return success
    return true;
  default:
    // return failure
    return false;
  }
}

/**
 * Get the Nth parameter for the given block type.
 *
 * Returns `false` on error.
 */
bool
pwasm_block_type_params_get_nth(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  const size_t pos,
  uint32_t * const ret_type
) {
  // get kind of block type, check for error
  const pwasm_block_type_kind_t kind = pwasm_block_type_get_kind(mod, block_type);
  if (kind != PWASM_BLOCK_TYPE_KIND_IMPORT) {
    // return failure
    return false;
  }

  // get params
  const pwasm_slice_t slice = mod->types[block_type].params;

  if (pos >= slice.len) {
    // return failure
    return false;
  }

  if (ret_type) {
    // save result
    *ret_type = mod->u32s[slice.ofs + pos];
  }

  // return success
  return true;
}

/**
 * Get the number of results for the given block type.
 *
 * Returns `false` on error.
 */
bool
pwasm_block_type_results_get_size(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  size_t * const ret
) {
  switch (pwasm_block_type_get_kind(mod, block_type)) {
  case PWASM_BLOCK_TYPE_KIND_RESULT:
    if (ret) {
      // save result
      *ret = (block_type == PWASM_BLOCK_TYPE_VOID) ? 0 : 1;
    }

    // return success
    return true;
  case PWASM_BLOCK_TYPE_KIND_IMPORT:
    if (ret) {
      // save result
      *ret = mod->types[block_type].results.len;
    }

    // return success
    return true;
  default:
    // return failure
    return false;
  }
}

/**
 * Get the Nth result for the given block type.
 *
 * Returns `false` on error.
 */
bool
pwasm_block_type_results_get_nth(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  const size_t pos,
  uint32_t * const ret_type
) {
  switch (pwasm_block_type_get_kind(mod, block_type)) {
  case PWASM_BLOCK_TYPE_KIND_RESULT:
    // check type
    if (block_type == PWASM_BLOCK_TYPE_VOID) {
      // return failure
      return false;
    }

    // check offset
    if (pos > 0) {
      // return failure
      return false;
    }

    if (ret_type) {
      // save result
      *ret_type = pwasm_block_type_result_get_value_type(block_type);
    }

    // return success
    return true;
  case PWASM_BLOCK_TYPE_KIND_IMPORT:
    {
      // get results
      const pwasm_slice_t slice = mod->types[block_type].results;

      // check offset
      if (pos >= slice.len) {
        return false;
      }

      if (ret_type) {
        // save result
        *ret_type = mod->u32s[slice.ofs + pos];
      }
    }

    // return success
    return true;
  default:
    // return failure
    return false;
  }
}

/**
 * Check to see if there is enough space to fill buffer with null-
 * terminated string description of block type.
 *
 * Returns `true` on if there is enough space, or `false` otherwise.
 */
static bool
pwasm_block_type_to_buf_check_size(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  const size_t buf_len
) {
  size_t sum = 0;

  // '['
  sum += 1;

  // get params.size, check for error
  size_t num_params;
  if (!pwasm_block_type_params_get_size(mod, block_type, &num_params)) {
    return false;
  }

  for (size_t i = 0; i < num_params; i++) {
    // ', '
    sum += (i > 0) ? 2 : 0;

    uint32_t val_type;
    if (!pwasm_block_type_params_get_nth(mod, block_type, i, &val_type)) {
      return false;
    }

    // get value type name
    const char *name = pwasm_value_type_get_name(val_type);

    sum += strlen(name);
  }

  // '] -> ['
  sum += 6;

  // get results.size, check for error
  size_t num_results;
  if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
    return false;
  }

  for (size_t i = 0; i < num_results; i++) {
    // ', '
    sum += (i > 0) ? 2 : 0;

    uint32_t val_type;
    if (!pwasm_block_type_results_get_nth(mod, block_type, i, &val_type)) {
      return false;
    }

    // get value type name
    const char *name = pwasm_value_type_get_name(val_type);

    sum += strlen(name);
  }

  // ']', '\0'
  sum += 2;

  // return result
  return sum < buf_len;
}

/**
 * Fill buffer with null-terminated string description of block type.
 *
 * Returns `true` on success or `false` on error.
 */
bool
pwasm_block_type_to_buf(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  char * const buf,
  const size_t buf_len
) {
  size_t ofs = 0;

  // check buffer size
  if (!pwasm_block_type_to_buf_check_size(mod, block_type, buf_len)) {
    // buffer too small, return false
    return false;
  }

  // push  '['
  buf[ofs++] = '[';

  // get params.size, check for error
  size_t num_params;
  if (!pwasm_block_type_params_get_size(mod, block_type, &num_params)) {
    return false;
  }

  for (size_t i = 0; i < num_params; i++) {
    // push ', '
    if (i > 0) {
      memcpy(buf + ofs, ", ", 2);
      ofs += 2;
    }

    // get value type, check for error
    uint32_t val_type;
    if (!pwasm_block_type_params_get_nth(mod, block_type, i, &val_type)) {
      return false;
    }

    // get value type name and length of value type name
    const char *name = pwasm_value_type_get_name(val_type);
    const size_t len = strlen(name);

    // push value type name
    memcpy(buf + ofs, name, len);
    ofs += len;
  }

  memcpy(buf + ofs, "] -> [", 6);
  ofs += 6;

  // get results.size, check for error
  size_t num_results;
  if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
    return false;
  }

  for (size_t i = 0; i < num_results; i++) {
    // ', '
    if (i > 0) {
      memcpy(buf + ofs, ", ", 2);
      ofs += 2;
    }

    uint32_t val_type;
    if (!pwasm_block_type_results_get_nth(mod, block_type, i, &val_type)) {
      return false;
    }

    // get value type name
    const char *name = pwasm_value_type_get_name(val_type);
    const size_t len = strlen(name);

    memcpy(buf + ofs, name, len);
    ofs += len;
  }

  // push ']', null-terminate
  buf[ofs] = ']';
  buf[ofs + 1] = 0;

  // return result
  return true;
}

/**
 * Compare parameters of two block types.
 *
 * Returns `false` if an error occurred.
 *
 * Otherwise, returns `true` and sets `ret_same` to the comparison
 * result (`true` if the parameters of both block types are the same,
 * and `false` otherwise).
 */
static bool
pwasm_block_type_compare_params(
  const pwasm_mod_t * const mod,
  const int32_t a,
  const int32_t b,
  bool * const ret_same
) {
  size_t num_diffs = 0;

  // get a.params.size, check for error
  size_t a_size;
  if (!pwasm_block_type_params_get_size(mod, a, &a_size)) {
    return false;
  }

  // get b.params.size, check for error
  size_t b_size;
  if (!pwasm_block_type_params_get_size(mod, b, &b_size)) {
    return false;
  }

  // compare sizes
  if (a_size != b_size) {
    // set comparison result, return success
    *ret_same = false;
    return true;
  }

  // compare types
  for (size_t i = 0; i < a_size; i++) {
    // get a.params[i], check for error
    uint32_t a_val_type;
    if (!pwasm_block_type_params_get_nth(mod, a, i, &a_val_type)) {
      return false;
    }

    // get b.params[i], check for error
    uint32_t b_val_type;
    if (!pwasm_block_type_params_get_nth(mod, b, i, &b_val_type)) {
      return false;
    }

    // check for difference
    num_diffs += (a_val_type != b_val_type);
  }

  // set comparison
  *ret_same = (num_diffs == 0);

  // return success
  return true;
}

/**
 * Compare results of two block types.
 *
 * Returns `false` if an error occurred.
 *
 * Otherwise, returns `true` and sets `ret_same` to the comparison
 * result (`true` if the results of both block types are the same, and
 * `false` otherwise).
 */
static bool
pwasm_block_type_compare_results(
  const pwasm_mod_t * const mod,
  const int32_t a,
  const int32_t b,
  bool * const ret_same
) {
  size_t num_diffs = 0;

  // get a.results.size, check for error
  size_t a_size;
  if (!pwasm_block_type_results_get_size(mod, a, &a_size)) {
    return false;
  }

  // get b.results.size, check for error
  size_t b_size;
  if (!pwasm_block_type_results_get_size(mod, b, &b_size)) {
    return false;
  }

  // compare sizes
  if (a_size != b_size) {
    // set comparison result, return success
    *ret_same = false;
    return true;
  }

  // compare types
  for (size_t i = 0; i < a_size; i++) {
    // get a.results[i], check for error
    uint32_t a_type;
    if (!pwasm_block_type_results_get_nth(mod, a, i, &a_type)) {
      return false;
    }

    // get b.results[i], check for error
    uint32_t b_type;
    if (!pwasm_block_type_results_get_nth(mod, b, i, &b_type)) {
      return false;
    }

    // check for difference
    num_diffs += (a_type != b_type);
  }

  #ifdef PWASM_DEBUG
  if (num_diffs != 0) {
    // convert a to string
    char a_buf[512];
    if (!pwasm_block_type_to_buf(mod, a, a_buf, sizeof(a_buf))) {
      D("couldn't convert block_type %c to buf", 'a');
      return false;
    }

    // convert b to string
    char b_buf[512];
    if (!pwasm_block_type_to_buf(mod, b, b_buf, sizeof(b_buf))) {
      D("couldn't convert block_type %c to buf", 'b');
      return false;
    }

    // dump bufs
    D("a = %s, b = %s", a_buf, b_buf);
  }
  #endif /* PWASM_DEBUG */

  // save result to destination
  *ret_same = (num_diffs == 0);

  // return success
  return true;
}

/**
 * Compare parameters and results of two block types.
 *
 * Returns `false` if an error occurred.
 *
 * Otherwise, returns `true` and sets `ret_same` to the comparison
 * result (`true` if the parameters and results of the block types are
 * the same, and `false` otherwise).
 */
bool
pwasm_block_type_compare(
  const pwasm_mod_t * const mod,
  const int32_t a,
  const int32_t b,
  bool * const ret_same
) {
  // compare params
  bool same_params;
  if (!pwasm_block_type_compare_params(mod, a, b, &same_params)) {
    return false;
  }

  // compare results
  bool same_results;
  if (!pwasm_block_type_compare_results(mod, a, b, &same_results)) {
    return false;
  }

  // set comparison result
  *ret_same = (same_params && same_results);

  // return success
  return true;
}

// generated by: bin/gen.rb op-data
static const pwasm_op_data_t
PWASM_OPS[] = {{
  .set        = PWASM_OPS_MAIN,
  .name       = "unreachable",
  .bytes      = { 0x00 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "nop",
  .bytes      = { 0x01 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "block",
  .bytes      = { 0x02 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_BLOCK,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "loop",
  .bytes      = { 0x03 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_BLOCK,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "if",
  .bytes      = { 0x04 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_BLOCK,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "else",
  .bytes      = { 0x05 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "end",
  .bytes      = { 0x0b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "br",
  .bytes      = { 0x0c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "br_if",
  .bytes      = { 0x0d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "br_table",
  .bytes      = { 0x0e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_BR_TABLE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "return",
  .bytes      = { 0x0f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "call",
  .bytes      = { 0x10 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "call_indirect",
  .bytes      = { 0x11 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_CALL_INDIRECT,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "drop",
  .bytes      = { 0x1a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "select",
  .bytes      = { 0x1b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "local.get",
  .bytes      = { 0x20 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "local.set",
  .bytes      = { 0x21 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "local.tee",
  .bytes      = { 0x22 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "global.get",
  .bytes      = { 0x23 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "global.set",
  .bytes      = { 0x24 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.load",
  .bytes      = { 0x28 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.load",
  .bytes      = { 0x29 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.load",
  .bytes      = { 0x2a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.load",
  .bytes      = { 0x2b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.load8_s",
  .bytes      = { 0x2c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 1,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.load8_u",
  .bytes      = { 0x2d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 1,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.load16_s",
  .bytes      = { 0x2e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 2,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.load16_u",
  .bytes      = { 0x2f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 2,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.load8_s",
  .bytes      = { 0x30 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 1,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.load8_u",
  .bytes      = { 0x31 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 1,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.load16_s",
  .bytes      = { 0x32 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 2,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.load16_u",
  .bytes      = { 0x33 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 2,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.load32_s",
  .bytes      = { 0x34 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.load32_u",
  .bytes      = { 0x35 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.store",
  .bytes      = { 0x36 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.store",
  .bytes      = { 0x37 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.store",
  .bytes      = { 0x38 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.store",
  .bytes      = { 0x39 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.store8",
  .bytes      = { 0x3a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 1,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.store16",
  .bytes      = { 0x3b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 2,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.store8",
  .bytes      = { 0x3c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 1,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.store16",
  .bytes      = { 0x3d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 2,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.store32",
  .bytes      = { 0x3e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "memory.size",
  .bytes      = { 0x3f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "memory.grow",
  .bytes      = { 0x40 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_INDEX,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.const",
  .bytes      = { 0x41 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_I32_CONST,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.const",
  .bytes      = { 0x42 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_I64_CONST,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.const",
  .bytes      = { 0x43 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_F32_CONST,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.const",
  .bytes      = { 0x44 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_F64_CONST,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.eqz",
  .bytes      = { 0x45 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.eq",
  .bytes      = { 0x46 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.ne",
  .bytes      = { 0x47 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.lt_s",
  .bytes      = { 0x48 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.lt_u",
  .bytes      = { 0x49 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.gt_s",
  .bytes      = { 0x4a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.gt_u",
  .bytes      = { 0x4b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.le_s",
  .bytes      = { 0x4c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.le_u",
  .bytes      = { 0x4d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.ge_s",
  .bytes      = { 0x4e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.ge_u",
  .bytes      = { 0x4f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.eqz",
  .bytes      = { 0x50 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.eq",
  .bytes      = { 0x51 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.ne",
  .bytes      = { 0x52 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.lt_s",
  .bytes      = { 0x53 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.lt_u",
  .bytes      = { 0x54 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.gt_s",
  .bytes      = { 0x55 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.gt_u",
  .bytes      = { 0x56 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.le_s",
  .bytes      = { 0x57 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.le_u",
  .bytes      = { 0x58 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.ge_s",
  .bytes      = { 0x59 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.ge_u",
  .bytes      = { 0x5a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.eq",
  .bytes      = { 0x5b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.ne",
  .bytes      = { 0x5c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.lt",
  .bytes      = { 0x5d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.gt",
  .bytes      = { 0x5e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.le",
  .bytes      = { 0x5f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.ge",
  .bytes      = { 0x60 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.eq",
  .bytes      = { 0x61 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.ne",
  .bytes      = { 0x62 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.lt",
  .bytes      = { 0x63 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.gt",
  .bytes      = { 0x64 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.le",
  .bytes      = { 0x65 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.ge",
  .bytes      = { 0x66 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.clz",
  .bytes      = { 0x67 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.ctz",
  .bytes      = { 0x68 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.popcnt",
  .bytes      = { 0x69 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.add",
  .bytes      = { 0x6a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.sub",
  .bytes      = { 0x6b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.mul",
  .bytes      = { 0x6c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.div_s",
  .bytes      = { 0x6d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.div_u",
  .bytes      = { 0x6e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.rem_s",
  .bytes      = { 0x6f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.rem_u",
  .bytes      = { 0x70 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.and",
  .bytes      = { 0x71 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.or",
  .bytes      = { 0x72 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.xor",
  .bytes      = { 0x73 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.shl",
  .bytes      = { 0x74 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.shr_s",
  .bytes      = { 0x75 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.shr_u",
  .bytes      = { 0x76 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.rotl",
  .bytes      = { 0x77 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.rotr",
  .bytes      = { 0x78 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.clz",
  .bytes      = { 0x79 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.ctz",
  .bytes      = { 0x7a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.popcnt",
  .bytes      = { 0x7b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.add",
  .bytes      = { 0x7c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.sub",
  .bytes      = { 0x7d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.mul",
  .bytes      = { 0x7e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.div_s",
  .bytes      = { 0x7f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.div_u",
  .bytes      = { 0x80 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.rem_s",
  .bytes      = { 0x81 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.rem_u",
  .bytes      = { 0x82 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.and",
  .bytes      = { 0x83 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.or",
  .bytes      = { 0x84 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.xor",
  .bytes      = { 0x85 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.shl",
  .bytes      = { 0x86 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.shr_s",
  .bytes      = { 0x87 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.shr_u",
  .bytes      = { 0x88 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.rotl",
  .bytes      = { 0x89 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.rotr",
  .bytes      = { 0x8a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.abs",
  .bytes      = { 0x8b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.neg",
  .bytes      = { 0x8c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.ceil",
  .bytes      = { 0x8d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.floor",
  .bytes      = { 0x8e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.trunc",
  .bytes      = { 0x8f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.nearest",
  .bytes      = { 0x90 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.sqrt",
  .bytes      = { 0x91 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.add",
  .bytes      = { 0x92 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.sub",
  .bytes      = { 0x93 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.mul",
  .bytes      = { 0x94 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.div",
  .bytes      = { 0x95 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.min",
  .bytes      = { 0x96 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.max",
  .bytes      = { 0x97 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.copysign",
  .bytes      = { 0x98 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.abs",
  .bytes      = { 0x99 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.neg",
  .bytes      = { 0x9a },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.ceil",
  .bytes      = { 0x9b },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.floor",
  .bytes      = { 0x9c },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.trunc",
  .bytes      = { 0x9d },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.nearest",
  .bytes      = { 0x9e },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.sqrt",
  .bytes      = { 0x9f },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.add",
  .bytes      = { 0xa0 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.sub",
  .bytes      = { 0xa1 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.mul",
  .bytes      = { 0xa2 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.div",
  .bytes      = { 0xa3 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.min",
  .bytes      = { 0xa4 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.max",
  .bytes      = { 0xa5 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.copysign",
  .bytes      = { 0xa6 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.wrap_i64",
  .bytes      = { 0xa7 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.trunc_f32_s",
  .bytes      = { 0xa8 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.trunc_f32_u",
  .bytes      = { 0xa9 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.trunc_f64_s",
  .bytes      = { 0xaa },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.trunc_f64_u",
  .bytes      = { 0xab },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.extend_i32_s",
  .bytes      = { 0xac },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.extend_i32_u",
  .bytes      = { 0xad },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.trunc_f32_s",
  .bytes      = { 0xae },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.trunc_f32_u",
  .bytes      = { 0xaf },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.trunc_f64_s",
  .bytes      = { 0xb0 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.trunc_f64_u",
  .bytes      = { 0xb1 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.convert_i32_s",
  .bytes      = { 0xb2 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.convert_i32_u",
  .bytes      = { 0xb3 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.convert_i64_s",
  .bytes      = { 0xb4 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.convert_i64_u",
  .bytes      = { 0xb5 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.demote_f64",
  .bytes      = { 0xb6 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.convert_i32_s",
  .bytes      = { 0xb7 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.convert_i32_u",
  .bytes      = { 0xb8 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.convert_i64_s",
  .bytes      = { 0xb9 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.convert_i64_u",
  .bytes      = { 0xba },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.promote_f32",
  .bytes      = { 0xbb },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.reinterpret_f32",
  .bytes      = { 0xbc },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.reinterpret_f64",
  .bytes      = { 0xbd },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f32.reinterpret_i32",
  .bytes      = { 0xbe },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "f64.reinterpret_i64",
  .bytes      = { 0xbf },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.extend8_s",
  .bytes      = { 0xc0 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i32.extend16_s",
  .bytes      = { 0xc1 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.extend8_s",
  .bytes      = { 0xc2 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.extend16_s",
  .bytes      = { 0xc3 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_MAIN,
  .name       = "i64.extend32_s",
  .bytes      = { 0xc4 },
  .num_bytes  = 1,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i32.trunc_sat_f32_s",
  .bytes      = { 0xfc, 0x00 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i32.trunc_sat_f32_u",
  .bytes      = { 0xfc, 0x01 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i32.trunc_sat_f64_s",
  .bytes      = { 0xfc, 0x02 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i32.trunc_sat_f64_u",
  .bytes      = { 0xfc, 0x03 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i64.trunc_sat_f32_s",
  .bytes      = { 0xfc, 0x04 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i64.trunc_sat_f32_u",
  .bytes      = { 0xfc, 0x05 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i64.trunc_sat_f64_s",
  .bytes      = { 0xfc, 0x06 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_TRUNC_SAT,
  .name       = "i64.trunc_sat_f64_u",
  .bytes      = { 0xfc, 0x07 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.load",
  .bytes      = { 0xfd, 0x00 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 16,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.load8x8_s",
  .bytes      = { 0xfd, 0x01 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.load8x8_u",
  .bytes      = { 0xfd, 0x02 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.load16x4_s",
  .bytes      = { 0xfd, 0x03 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.load16x4_u",
  .bytes      = { 0xfd, 0x04 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.load32x2_s",
  .bytes      = { 0xfd, 0x05 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.load32x2_u",
  .bytes      = { 0xfd, 0x06 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v8x16.load_splat",
  .bytes      = { 0xfd, 0x07 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 1,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v16x8.load_splat",
  .bytes      = { 0xfd, 0x08 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 2,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v32x4.load_splat",
  .bytes      = { 0xfd, 0x09 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 4,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v64x2.load_splat",
  .bytes      = { 0xfd, 0x0a },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 8,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.store",
  .bytes      = { 0xfd, 0x0b },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_MEM,
  .mem_size   = 16,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.const",
  .bytes      = { 0xfd, 0x0c },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_V128_CONST,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v8x16.shuffle",
  .bytes      = { 0xfd, 0x0d },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_V128_CONST,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v8x16.swizzle",
  .bytes      = { 0xfd, 0x0e },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.splat",
  .bytes      = { 0xfd, 0x0f },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.splat",
  .bytes      = { 0xfd, 0x10 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.splat",
  .bytes      = { 0xfd, 0x11 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.splat",
  .bytes      = { 0xfd, 0x12 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.splat",
  .bytes      = { 0xfd, 0x13 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.splat",
  .bytes      = { 0xfd, 0x14 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.extract_lane_s",
  .bytes      = { 0xfd, 0x15 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 16,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.extract_lane_u",
  .bytes      = { 0xfd, 0x16 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 16,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.replace_lane",
  .bytes      = { 0xfd, 0x17 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 16,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.extract_lane_s",
  .bytes      = { 0xfd, 0x18 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 8,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.extract_lane_u",
  .bytes      = { 0xfd, 0x19 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 8,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.replace_lane",
  .bytes      = { 0xfd, 0x1a },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 8,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.extract_lane",
  .bytes      = { 0xfd, 0x1b },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 4,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.replace_lane",
  .bytes      = { 0xfd, 0x1c },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 4,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.extract_lane",
  .bytes      = { 0xfd, 0x1d },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 2,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.replace_lane",
  .bytes      = { 0xfd, 0x1e },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 2,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.extract_lane",
  .bytes      = { 0xfd, 0x1f },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 4,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.replace_lane",
  .bytes      = { 0xfd, 0x20 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 4,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.extract_lane",
  .bytes      = { 0xfd, 0x21 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 2,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.replace_lane",
  .bytes      = { 0xfd, 0x22 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_LANE_INDEX,
  .mem_size   = 0,
  .num_lanes  = 2,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.eq",
  .bytes      = { 0xfd, 0x23 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.ne",
  .bytes      = { 0xfd, 0x24 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.lt_s",
  .bytes      = { 0xfd, 0x25 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.lt_u",
  .bytes      = { 0xfd, 0x26 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.gt_s",
  .bytes      = { 0xfd, 0x27 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.gt_u",
  .bytes      = { 0xfd, 0x28 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.le_s",
  .bytes      = { 0xfd, 0x29 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.le_u",
  .bytes      = { 0xfd, 0x2a },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.ge_s",
  .bytes      = { 0xfd, 0x2b },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.ge_u",
  .bytes      = { 0xfd, 0x2c },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.eq",
  .bytes      = { 0xfd, 0x2d },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.ne",
  .bytes      = { 0xfd, 0x2e },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.lt_s",
  .bytes      = { 0xfd, 0x2f },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.lt_u",
  .bytes      = { 0xfd, 0x30 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.gt_s",
  .bytes      = { 0xfd, 0x31 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.gt_u",
  .bytes      = { 0xfd, 0x32 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.le_s",
  .bytes      = { 0xfd, 0x33 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.le_u",
  .bytes      = { 0xfd, 0x34 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.ge_s",
  .bytes      = { 0xfd, 0x35 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.ge_u",
  .bytes      = { 0xfd, 0x36 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.eq",
  .bytes      = { 0xfd, 0x37 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.ne",
  .bytes      = { 0xfd, 0x38 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.lt_s",
  .bytes      = { 0xfd, 0x39 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.lt_u",
  .bytes      = { 0xfd, 0x3a },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.gt_s",
  .bytes      = { 0xfd, 0x3b },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.gt_u",
  .bytes      = { 0xfd, 0x3c },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.le_s",
  .bytes      = { 0xfd, 0x3d },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.le_u",
  .bytes      = { 0xfd, 0x3e },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.ge_s",
  .bytes      = { 0xfd, 0x3f },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.ge_u",
  .bytes      = { 0xfd, 0x40 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.eq",
  .bytes      = { 0xfd, 0x41 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.ne",
  .bytes      = { 0xfd, 0x42 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.lt",
  .bytes      = { 0xfd, 0x43 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.gt",
  .bytes      = { 0xfd, 0x44 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.le",
  .bytes      = { 0xfd, 0x45 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.ge",
  .bytes      = { 0xfd, 0x46 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.eq",
  .bytes      = { 0xfd, 0x47 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.ne",
  .bytes      = { 0xfd, 0x48 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.lt",
  .bytes      = { 0xfd, 0x49 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.gt",
  .bytes      = { 0xfd, 0x4a },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.le",
  .bytes      = { 0xfd, 0x4b },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.ge",
  .bytes      = { 0xfd, 0x4c },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.not",
  .bytes      = { 0xfd, 0x4d },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.and",
  .bytes      = { 0xfd, 0x4e },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.andnot",
  .bytes      = { 0xfd, 0x4f },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.or",
  .bytes      = { 0xfd, 0x50 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.xor",
  .bytes      = { 0xfd, 0x51 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "v128.bitselect",
  .bytes      = { 0xfd, 0x52 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.abs",
  .bytes      = { 0xfd, 0x60 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.neg",
  .bytes      = { 0xfd, 0x61 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.any_true",
  .bytes      = { 0xfd, 0x62 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.all_true",
  .bytes      = { 0xfd, 0x63 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.narrow_i16x8_s",
  .bytes      = { 0xfd, 0x65 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.narrow_i16x8_u",
  .bytes      = { 0xfd, 0x66 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.shl",
  .bytes      = { 0xfd, 0x6b },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.shr_s",
  .bytes      = { 0xfd, 0x6c },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.shr_u",
  .bytes      = { 0xfd, 0x6d },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.add",
  .bytes      = { 0xfd, 0x6e },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.add_saturate_s",
  .bytes      = { 0xfd, 0x6f },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.add_saturate_u",
  .bytes      = { 0xfd, 0x70 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.sub",
  .bytes      = { 0xfd, 0x71 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.sub_saturate_s",
  .bytes      = { 0xfd, 0x72 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.sub_saturate_u",
  .bytes      = { 0xfd, 0x73 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.min_s",
  .bytes      = { 0xfd, 0x76 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.min_u",
  .bytes      = { 0xfd, 0x77 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.max_s",
  .bytes      = { 0xfd, 0x78 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.max_u",
  .bytes      = { 0xfd, 0x79 },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i8x16.avgr_u",
  .bytes      = { 0xfd, 0x7b },
  .num_bytes  = 2,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.abs",
  .bytes      = { 0xfd, 0x80, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.neg",
  .bytes      = { 0xfd, 0x81, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.any_true",
  .bytes      = { 0xfd, 0x82, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.all_true",
  .bytes      = { 0xfd, 0x83, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.narrow_i32x4_s",
  .bytes      = { 0xfd, 0x85, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.narrow_i32x4_u",
  .bytes      = { 0xfd, 0x86, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.widen_low_i8x16_s",
  .bytes      = { 0xfd, 0x87, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.widen_high_i8x16_s",
  .bytes      = { 0xfd, 0x88, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.widen_low_i8x16_u",
  .bytes      = { 0xfd, 0x89, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.widen_high_i8x16_u",
  .bytes      = { 0xfd, 0x8a, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.shl",
  .bytes      = { 0xfd, 0x8b, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.shr_s",
  .bytes      = { 0xfd, 0x8c, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.shr_u",
  .bytes      = { 0xfd, 0x8d, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.add",
  .bytes      = { 0xfd, 0x8e, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.add_saturate_s",
  .bytes      = { 0xfd, 0x8f, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.add_saturate_u",
  .bytes      = { 0xfd, 0x90, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.sub",
  .bytes      = { 0xfd, 0x91, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.sub_saturate_s",
  .bytes      = { 0xfd, 0x92, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.sub_saturate_u",
  .bytes      = { 0xfd, 0x93, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.mul",
  .bytes      = { 0xfd, 0x95, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.min_s",
  .bytes      = { 0xfd, 0x96, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.min_u",
  .bytes      = { 0xfd, 0x97, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.max_s",
  .bytes      = { 0xfd, 0x98, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.max_u",
  .bytes      = { 0xfd, 0x99, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i16x8.avgr_u",
  .bytes      = { 0xfd, 0x9b, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.abs",
  .bytes      = { 0xfd, 0xa0, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.neg",
  .bytes      = { 0xfd, 0xa1, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.any_true",
  .bytes      = { 0xfd, 0xa2, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.all_true",
  .bytes      = { 0xfd, 0xa3, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.widen_low_i16x8_s",
  .bytes      = { 0xfd, 0xa7, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.widen_high_i16x8_s",
  .bytes      = { 0xfd, 0xa8, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.widen_low_i16x8_u",
  .bytes      = { 0xfd, 0xa9, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.widen_high_i16x8_u",
  .bytes      = { 0xfd, 0xaa, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.shl",
  .bytes      = { 0xfd, 0xab, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.shr_s",
  .bytes      = { 0xfd, 0xac, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.shr_u",
  .bytes      = { 0xfd, 0xad, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.add",
  .bytes      = { 0xfd, 0xae, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.sub",
  .bytes      = { 0xfd, 0xb1, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.mul",
  .bytes      = { 0xfd, 0xb5, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.min_s",
  .bytes      = { 0xfd, 0xb6, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.min_u",
  .bytes      = { 0xfd, 0xb7, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.max_s",
  .bytes      = { 0xfd, 0xb8, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.max_u",
  .bytes      = { 0xfd, 0xb9, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.neg",
  .bytes      = { 0xfd, 0xc1, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.shl",
  .bytes      = { 0xfd, 0xcb, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.shr_s",
  .bytes      = { 0xfd, 0xcc, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.shr_u",
  .bytes      = { 0xfd, 0xcd, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.add",
  .bytes      = { 0xfd, 0xce, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.sub",
  .bytes      = { 0xfd, 0xd1, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i64x2.mul",
  .bytes      = { 0xfd, 0xd5, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.abs",
  .bytes      = { 0xfd, 0xe0, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.neg",
  .bytes      = { 0xfd, 0xe1, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.sqrt",
  .bytes      = { 0xfd, 0xe3, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.add",
  .bytes      = { 0xfd, 0xe4, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.sub",
  .bytes      = { 0xfd, 0xe5, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.mul",
  .bytes      = { 0xfd, 0xe6, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.div",
  .bytes      = { 0xfd, 0xe7, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.min",
  .bytes      = { 0xfd, 0xe8, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.max",
  .bytes      = { 0xfd, 0xe9, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.abs",
  .bytes      = { 0xfd, 0xec, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.neg",
  .bytes      = { 0xfd, 0xed, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.sqrt",
  .bytes      = { 0xfd, 0xef, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.add",
  .bytes      = { 0xfd, 0xf0, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.sub",
  .bytes      = { 0xfd, 0xf1, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.mul",
  .bytes      = { 0xfd, 0xf2, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.div",
  .bytes      = { 0xfd, 0xf3, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.min",
  .bytes      = { 0xfd, 0xf4, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f64x2.max",
  .bytes      = { 0xfd, 0xf5, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.trunc_sat_f32x4_s",
  .bytes      = { 0xfd, 0xf8, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "i32x4.trunc_sat_f32x4_u",
  .bytes      = { 0xfd, 0xf9, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.convert_i32x4_s",
  .bytes      = { 0xfd, 0xfa, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}, {
  .set        = PWASM_OPS_SIMD,
  .name       = "f32x4.convert_i32x4_u",
  .bytes      = { 0xfd, 0xfb, 0x01 },
  .num_bytes  = 3,
  .imm        = PWASM_IMM_NONE,
  .mem_size   = 0,
  .num_lanes  = 0,
}};

/**
 * Get opcode name as a string.
 */
const char *
pwasm_op_get_name(
  const pwasm_op_t op
) {
  return (op < PWASM_OP_LAST) ? PWASM_OPS[op].name : "invalid opcode";
}

/**
 * Get immediate type for opcode.
 */
pwasm_imm_t
pwasm_op_get_imm(
  const pwasm_op_t op
) {
  return (op < PWASM_OP_LAST) ? PWASM_OPS[op].imm : PWASM_IMM_NONE;
}

/**
 * Packed bitmask of opcode values from all sets.  Used to test opcode
 * validity.
 *
 * Generated by `bin/gen.rb op-mask`.
 */
static const uint64_t
PWASM_VALID_OPS_MASK[] = {
  0xffffff1f0c03f83f, // main[0]
  0xffffffffffffffff, // main[1]
  0xffffffffffffffff, // main[2]
  0x000000000000001f, // main[3]
  0x00000000000000ff, // trunc_sat[0]
  0x0000000000000000, // trunc_sat[1]
  0x0000000000000000, // trunc_sat[2]
  0x0000000000000000, // trunc_sat[3]
  0xffffffffffffffff, // simd[0]
  0x0bcff86f0007ffff, // simd[1]
  0x03e27f8f0befffef, // simd[2]
  0x0f3fb3fb00227802, // simd[3]
};

/**
 * Returns true if the given set and opcode value are valid, and false
 * otherwise.
 */
static inline bool
pwasm_op_is_valid(
  const pwasm_ops_t set,
  const uint32_t val
) {
  const bool ok = (set < PWASM_OPS_LAST) && (val < 0x100);
  const size_t ofs = (4 * set) + val / 64;
  const size_t mask = ((uint64_t) 1 << (val & 0x3F));
  return ok && (PWASM_VALID_OPS_MASK[ofs] & mask);
}

static inline bool
pwasm_op_is_enter(
  const pwasm_op_t op
) {
  return (op == PWASM_OP_BLOCK) ||
         (op == PWASM_OP_LOOP) ||
         (op == PWASM_OP_IF);
}

static inline bool
pwasm_op_is_const(
  const uint8_t byte
) {
  return (
    (byte == PWASM_OP_I32_CONST) ||
    (byte == PWASM_OP_I64_CONST) ||
    (byte == PWASM_OP_F32_CONST) ||
    (byte == PWASM_OP_F64_CONST) ||
    (byte == PWASM_OP_V128_CONST)
  );
}

/**
 * Get the number of bytes of the target for the given memory
 * instruction.
 *
 * Used for memory alignment checking specified here:
 * https://webassembly.github.io/spec/core/valid/instructions.html#memory-instructions
 */
static inline uint8_t
pwasm_op_get_num_bytes(
  const pwasm_op_t op
) {
  return PWASM_OPS[op].mem_size;
}

typedef struct {
  size_t val; // current value
  size_t max; // maximum value (high water mark)
} pwasm_depth_t;

/**
 * add num to depth stack, check for overflow.
 */
static inline bool
pwasm_depth_add(
  pwasm_depth_t * const depth,
  const size_t num
) {
  const size_t new_val = depth->val + num;
  const bool ok = (new_val >= depth->val);
  depth->val = ok ? new_val : depth->val;
  depth->max = (new_val > depth->max) ? new_val : depth->max;
  return ok;
}

/**
 * subtract num from depth stack, check for underflow.
 */
static inline bool
pwasm_depth_sub(
  pwasm_depth_t * const depth,
  const size_t num
) {
  const size_t new_val = depth->val - num;
  const bool ok = (depth->val >= num);
  depth->val = ok ? new_val : depth->val;
  return ok;
}

/**
 * no-op on_error callback (used as a fallback to avoid having to do
 * null ptr checks on error).
 */
static void
pwasm_null_on_error(
  const char * const text,
  void * const cb_data
) {
  (void) text;
  (void) cb_data;
  D("null error: %s", text);
}

/**
 * no-op on_warning callback (used as a fallback to avoid having to do
 * null ptr checks on warning).
 */
static void
pwasm_null_on_warning(
  const char * const text,
  void * const cb_data
) {
  (void) text;
  (void) cb_data;
  D("null warning: %s", text);
}

typedef struct {
  void (*on_error)(const char *, void *);
} pwasm_parse_buf_cbs_t;

/**
 * Parse +src+ into +dst+ and return number of bytes consumed.
 *
 * Returns 0 on error.
 *
 * If +src_cbs+ callback structure pointer is non-NULL and the
 * +on_error+ member of src_cbs is non-NULL, then the on_error callback
 * is called with an error message and the provided callback data
 * +cb_data+.
 */
static size_t
pwasm_parse_buf(
  pwasm_buf_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_buf_cbs_t * const src_cbs,
  void *cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  const pwasm_parse_buf_cbs_t cbs = {
    .on_error = (src_cbs && src_cbs->on_error) ?
                src_cbs->on_error :
                pwasm_null_on_error,
  };

  // check source length
  if (!curr.len) {
    cbs.on_error("empty buffer", cb_data);
    return 0;
  }

  // decode count, check for error
  uint32_t count = 0;
  const size_t len = pwasm_u32_decode(&count, curr);
  if (!len) {
    cbs.on_error("bad name length", cb_data);
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, len);
  num_bytes += len;

  // D("src: %p, src_len = %zu, len = %u, len_ofs = %zu", src, src_len, len, len_ofs);

  // calculate total number of bytes, check for overflow
  if (count > curr.len) {
    cbs.on_error("truncated buffer", cb_data);
    return 0;
  }

  if (dst) {
    // build result, save result to destination
    *dst = (pwasm_buf_t) { curr.ptr, count };
  }

  // return number of bytes consumed
  return num_bytes + count;
}

/**
 * no-op on_count callback for pwasm_parse_u32s.
 *
 * (used as a fallback to elide null ptr checks).
 */
static void
pwasm_parse_u32s_null_on_count(
  const uint32_t count,
  void *cb_data
) {
  (void) count;
  (void) cb_data;
}

/**
 * no-op on_items callback for pwasm_parse_u32s.
 *
 * (used as a fallback to elide null ptr checks).
 */
static void
pwasm_parse_u32s_null_on_items(
  const uint32_t * const items,
  const size_t num,
  void *cb_data
) {
  (void) items;
  (void) num;
  (void) cb_data;
}

typedef struct {
  void (*on_count)(const uint32_t, void *);
  void (*on_items)(const uint32_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_u32s_cbs_t;

/**
 * Parse a vector of u32s in +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_u32s(
  const pwasm_buf_t src,
  const pwasm_parse_u32s_cbs_t * const src_cbs,
  void *cb_data
) {
  const pwasm_parse_u32s_cbs_t cbs = {
    .on_count = (src_cbs && src_cbs->on_count) ? src_cbs->on_count : pwasm_parse_u32s_null_on_count,
    .on_items = (src_cbs && src_cbs->on_items) ? src_cbs->on_items : pwasm_parse_u32s_null_on_items,
    .on_error = (src_cbs && src_cbs->on_error) ? src_cbs->on_error : pwasm_null_on_error,
  };

  // get count, check for error
  uint32_t count;
  const size_t count_len = pwasm_u32_decode(&count, src);
  if (!count_len) {
    cbs.on_error("bad u32 vector count", cb_data);
    return 0;
  }

  cbs.on_count(count, cb_data);

  // track number of bytes and current buffer
  size_t num_bytes = count_len;
  pwasm_buf_t curr = pwasm_buf_step(src, count_len);
  uint32_t items[PWASM_BATCH_SIZE];

  size_t ofs = 0;
  for (size_t i = 0; i < count; i++) {
    if (!curr.len) {
    }
    if (num_bytes > src.len) {
      cbs.on_error("u32 vector buffer overflow", cb_data);
      return 0;
    }

    // decode value, check for error
    const size_t len = pwasm_u32_decode(items + ofs, curr);
    if (!len) {
      cbs.on_error("bad u32 in u32 vector", cb_data);
      return 0;
    }

    // increment buffer, byte count, and offset
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
    ofs++;

    if (ofs == LEN(items)) {
      // flush batch, reset offset
      cbs.on_items(items, LEN(items), cb_data);
      ofs = 0;
    }
  }

  if (ofs > 0) {
    // flush remaining values
    cbs.on_items(items, ofs, cb_data);
  }

  // return success
  return num_bytes;
}

typedef struct {
  void (*on_count)(const size_t, void *);
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_labels_cbs_t;

typedef struct {
  const pwasm_parse_labels_cbs_t * const cbs;
  void *cb_data;
  pwasm_slice_t slice;
} pwasm_parse_labels_t;

static void
pwasm_parse_labels_on_count(
  const uint32_t count,
  void *cb_data
) {
  pwasm_parse_labels_t *data = cb_data;

  if (data->cbs && data->cbs->on_count) {
    data->cbs->on_count(count + 1, data->cb_data);
  }
}

static void
pwasm_parse_labels_on_items(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_parse_labels_t *data = cb_data;

  if (data->cbs && data->cbs->on_labels) {
    const pwasm_slice_t slice = data->cbs->on_labels(rows, num, data->cb_data);

    if (data->slice.len) {
      data->slice.len += slice.len;
    } else {
      data->slice = slice;
    }
  }
}

static void
pwasm_parse_labels_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_parse_labels_t *data = cb_data;

  if (data->cbs && data->cbs->on_error) {
    data->cbs->on_error(text, data->cb_data);
  }
}

static const pwasm_parse_u32s_cbs_t
PWASM_PARSE_LABELS_CBS = {
  .on_count = pwasm_parse_labels_on_count,
  .on_items = pwasm_parse_labels_on_items,
  .on_error = pwasm_parse_labels_on_error,
};

static size_t
pwasm_parse_labels(
  pwasm_slice_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_labels_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  pwasm_parse_labels_t data = {
    .cbs = cbs,
    .cb_data = cb_data,
    .slice = { 0, 0 },
  };

  {
    // parse labels, check for error
    const size_t len = pwasm_parse_u32s(curr, &PWASM_PARSE_LABELS_CBS, &data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // get default label, check for error
  uint32_t label;
  {
    const size_t len = pwasm_u32_decode(&label, curr);
    if (!len) {
      const char * const text = "br_table: bad default label";
      cbs->on_error(text, cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // pass default label to callback
  pwasm_parse_labels_on_items(&label, 1, &data);

  *dst = data.slice;

  // silence scanner
  (void) curr;

  // return total number of bytes consumed
  return num_bytes;
}

/**
 * Type parser callbacks.
 */
typedef struct {
  /**
   * Called with u32s that should be cached.
   *
   * Callback should append u32s to internal list of u32s list and a
   * slice indicating the offset.
   *
   * u32s are used by the WebAssembly types to indicate the value types
   * of parameters and results.
   */
  pwasm_slice_t (*on_u32s)(const uint32_t *, const size_t, void *);

  /**
   * Called when a parse error occurs.
   */
  void (*on_error)(const char *, void *);
} pwasm_parse_type_cbs_t;

/**
 * type parser state
 */
typedef struct {
  const pwasm_parse_type_cbs_t * const cbs; ///< callbacks
  void *cb_data; ///< callback data
  bool success; ///< result
  pwasm_slice_t * const slice; ///< u32s slice
} pwasm_parse_type_t;

static void
pwasm_parse_type_on_items(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_parse_type_t * const data = cb_data;
  const pwasm_slice_t slice = data->cbs->on_u32s(rows, num, data->cb_data);

  if (data->slice->len > 0) {
    // already have offset, increment length
    data->slice->len += num;
  } else {
    // save initial slice
    *(data->slice) = slice;
  }
}

static void
pwasm_parse_type_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_parse_type_t * const data = cb_data;

  if (data->success) {
    data->success = false;
    if (data->cbs->on_error) {
      data->cbs->on_error(text, data->cb_data);
    }
  }
}

static const pwasm_parse_u32s_cbs_t
PWASM_PARSE_TYPE_CBS = {
  .on_items = pwasm_parse_type_on_items,
  .on_error = pwasm_parse_type_on_error,
};

static size_t
pwasm_parse_type(
  pwasm_type_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_type_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_slice_t slices[2] = { 0 };
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // check length
  if (!curr.len) {
    cbs->on_error("missing type indicator", cb_data);
    return 0;
  }

  // check type indicator
  if (curr.ptr[0] != 0x60) {
    cbs->on_error("invalid type indicator", cb_data);
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

  D("curr.ptr[0] = %02x", curr.ptr[0]);

  for (size_t i = 0; i < 2; i++) {
    // build context
    pwasm_parse_type_t data = {
      .cbs = cbs,
      .cb_data = cb_data,
      .success = true,
      .slice = slices + i,
    };

    // parse params, check for error
    const size_t len = pwasm_parse_u32s(curr, &PWASM_PARSE_TYPE_CBS, &data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // save result to output
  *dst = (pwasm_type_t) {
    .params = slices[0],
    .results = slices[1],
  };

  // return success
  return num_bytes;
}

/**
 * Custom section parser callbacks.
 */
typedef struct {
  /**
   * Called when a custom section is encountered.
   */
  void (*on_custom_section)(const pwasm_custom_section_t *, void *);

  /**
   * Called with bytes that should be cached.
   *
   * Callback should append bytes to internal byte buffer and then
   * return a slice indicating the offset and length within the byte
   * buffer.
   *
   */
  pwasm_slice_t (*on_bytes)(const uint8_t *, size_t, void *);

  /**
   * Called when a custom section parse error occurs.
   */
  void (*on_error)(const char *, void *);
} pwasm_parse_custom_section_cbs_t;

/**
 * Custom section parser state.
 */
typedef struct {
  const pwasm_parse_custom_section_cbs_t * const cbs; ///< callbacks
  void *cb_data; ///< callback data
  bool success; ///< parse result
} pwasm_parse_custom_section_t;

static void
pwasm_parse_custom_section_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_parse_custom_section_t * const data = cb_data;

  if (data->success) {
    data->success = false;
    data->cbs->on_error(text, data->cb_data);
  }
}

static const pwasm_parse_buf_cbs_t
PWASM_PARSE_CUSTOM_SECTION_CBS = {
  .on_error = pwasm_parse_custom_section_on_error,
};

static size_t
pwasm_parse_custom_section(
  const pwasm_buf_t src,
  const pwasm_parse_custom_section_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  pwasm_parse_custom_section_t data = {
    .cbs = cbs,
    .cb_data = cb_data,
    .success = true,
  };

  // parse to buffer, check for error
  pwasm_buf_t buf;
  const size_t len = pwasm_parse_buf(&buf, curr, &PWASM_PARSE_CUSTOM_SECTION_CBS, &data);
  if (!len) {
    return 0;
  }
  D("len = %zu, buf.len = %zu", len, buf.len);

  // emint name bytes, get name slice, check for error
  const pwasm_slice_t name = cbs->on_bytes(buf.ptr, buf.len, cb_data);
  D("name = { .ofs = %zu, .len = %zu }", name.ofs, name.len);
  if (name.len != buf.len) {
    // return failure
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, len);
  num_bytes += len;

  // emit data bytes, get data slice, check for error
  const pwasm_slice_t rest = cbs->on_bytes(curr.ptr, curr.len, cb_data);
  if (rest.len != curr.len) {
    // return failure
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, rest.len);
  num_bytes += rest.len;

  // build custom section
  const pwasm_custom_section_t section = {
    .name = name,
    .data = rest,
  };

  D(
    "name = { .ofs = %zu, .len = %zu}, data = { .ofs = %zu, .len = %zu }",
    section.name.ofs, section.name.len,
    section.data.ofs, section.data.len
  );

  // emit custom section
  cbs->on_custom_section(&section, cb_data);

  // silence scanner
  (void) curr;

  // return result
  return data.success ? num_bytes : 0;
}

/**
 * Parse limits into +dst+ from buffer +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_limits(
  pwasm_limits_t * const dst,
  const pwasm_buf_t src,
  void (*on_error)(const char *, void *),
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // check for NULL callback
  on_error = on_error ? on_error : pwasm_null_on_error;

  // check length
  if (src.len < 2) {
    on_error("truncated limits", cb_data);
    return 0;
  }

  // get/check has_max flag
  const uint8_t flag = src.ptr[0];
  if ((flag != 0) && (flag != 1)) {
    on_error("truncated limits", cb_data);
    return 0;
  }

  // advance buffer
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

  uint32_t vals[2] = { 0, 0 };
  for (size_t i = 0; i < (flag ? 2 : 1); i++) {
    // parse value, check for error
    const size_t len = pwasm_u32_decode(vals + i, curr);
    if (!len) {
      on_error("bad limits value", cb_data);
      return 0;
    }

    // advance buffer
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // write result
  *dst = (pwasm_limits_t) {
    .has_max = (flag == 1),
    .min = vals[0],
    .max = vals[1],
  };

  // return number of bytes consumed
  return num_bytes;
}

/**
 * Parse table into +dst+ from buffer +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_table(
  pwasm_table_t * const dst,
  const pwasm_buf_t src,
  void (*on_error)(const char *, void *),
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // check for NULL callback
  on_error = on_error ? on_error : pwasm_null_on_error;

  if (src.len < 3) {
    on_error("incomplete table type", cb_data);
    return 0;
  }

  // get element type, check for error
  // NOTE: at the moment only one element type is supported
  const pwasm_elem_type_t elem_type = curr.ptr[0];
  if (elem_type != 0x70) {
    on_error("invalid table element type", cb_data);
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

  // parse limits, check for error
  pwasm_limits_t limits;
  const size_t len = pwasm_parse_limits(&limits, curr, on_error, cb_data);
  if (!len) {
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, len);
  num_bytes += len;

  // write result
  *dst = (pwasm_table_t) {
    .elem_type  = elem_type,
    .limits     = limits,
  };

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_inst_cbs_t;

// generated by bin/gen.rb op-map
static const pwasm_op_t
PWASM_OP_MAP[] = {
  PWASM_OP_UNREACHABLE, // 0x00
  PWASM_OP_NOP, // 0x01
  PWASM_OP_BLOCK, // 0x02
  PWASM_OP_LOOP, // 0x03
  PWASM_OP_IF, // 0x04
  PWASM_OP_ELSE, // 0x05
  PWASM_OP_LAST, // 0x06
  PWASM_OP_LAST, // 0x07
  PWASM_OP_LAST, // 0x08
  PWASM_OP_LAST, // 0x09
  PWASM_OP_LAST, // 0x0A
  PWASM_OP_END, // 0x0B
  PWASM_OP_BR, // 0x0C
  PWASM_OP_BR_IF, // 0x0D
  PWASM_OP_BR_TABLE, // 0x0E
  PWASM_OP_RETURN, // 0x0F
  PWASM_OP_CALL, // 0x10
  PWASM_OP_CALL_INDIRECT, // 0x11
  PWASM_OP_LAST, // 0x12
  PWASM_OP_LAST, // 0x13
  PWASM_OP_LAST, // 0x14
  PWASM_OP_LAST, // 0x15
  PWASM_OP_LAST, // 0x16
  PWASM_OP_LAST, // 0x17
  PWASM_OP_LAST, // 0x18
  PWASM_OP_LAST, // 0x19
  PWASM_OP_DROP, // 0x1A
  PWASM_OP_SELECT, // 0x1B
  PWASM_OP_LAST, // 0x1C
  PWASM_OP_LAST, // 0x1D
  PWASM_OP_LAST, // 0x1E
  PWASM_OP_LAST, // 0x1F
  PWASM_OP_LOCAL_GET, // 0x20
  PWASM_OP_LOCAL_SET, // 0x21
  PWASM_OP_LOCAL_TEE, // 0x22
  PWASM_OP_GLOBAL_GET, // 0x23
  PWASM_OP_GLOBAL_SET, // 0x24
  PWASM_OP_LAST, // 0x25
  PWASM_OP_LAST, // 0x26
  PWASM_OP_LAST, // 0x27
  PWASM_OP_I32_LOAD, // 0x28
  PWASM_OP_I64_LOAD, // 0x29
  PWASM_OP_F32_LOAD, // 0x2A
  PWASM_OP_F64_LOAD, // 0x2B
  PWASM_OP_I32_LOAD8_S, // 0x2C
  PWASM_OP_I32_LOAD8_U, // 0x2D
  PWASM_OP_I32_LOAD16_S, // 0x2E
  PWASM_OP_I32_LOAD16_U, // 0x2F
  PWASM_OP_I64_LOAD8_S, // 0x30
  PWASM_OP_I64_LOAD8_U, // 0x31
  PWASM_OP_I64_LOAD16_S, // 0x32
  PWASM_OP_I64_LOAD16_U, // 0x33
  PWASM_OP_I64_LOAD32_S, // 0x34
  PWASM_OP_I64_LOAD32_U, // 0x35
  PWASM_OP_I32_STORE, // 0x36
  PWASM_OP_I64_STORE, // 0x37
  PWASM_OP_F32_STORE, // 0x38
  PWASM_OP_F64_STORE, // 0x39
  PWASM_OP_I32_STORE8, // 0x3A
  PWASM_OP_I32_STORE16, // 0x3B
  PWASM_OP_I64_STORE8, // 0x3C
  PWASM_OP_I64_STORE16, // 0x3D
  PWASM_OP_I64_STORE32, // 0x3E
  PWASM_OP_MEMORY_SIZE, // 0x3F
  PWASM_OP_MEMORY_GROW, // 0x40
  PWASM_OP_I32_CONST, // 0x41
  PWASM_OP_I64_CONST, // 0x42
  PWASM_OP_F32_CONST, // 0x43
  PWASM_OP_F64_CONST, // 0x44
  PWASM_OP_I32_EQZ, // 0x45
  PWASM_OP_I32_EQ, // 0x46
  PWASM_OP_I32_NE, // 0x47
  PWASM_OP_I32_LT_S, // 0x48
  PWASM_OP_I32_LT_U, // 0x49
  PWASM_OP_I32_GT_S, // 0x4A
  PWASM_OP_I32_GT_U, // 0x4B
  PWASM_OP_I32_LE_S, // 0x4C
  PWASM_OP_I32_LE_U, // 0x4D
  PWASM_OP_I32_GE_S, // 0x4E
  PWASM_OP_I32_GE_U, // 0x4F
  PWASM_OP_I64_EQZ, // 0x50
  PWASM_OP_I64_EQ, // 0x51
  PWASM_OP_I64_NE, // 0x52
  PWASM_OP_I64_LT_S, // 0x53
  PWASM_OP_I64_LT_U, // 0x54
  PWASM_OP_I64_GT_S, // 0x55
  PWASM_OP_I64_GT_U, // 0x56
  PWASM_OP_I64_LE_S, // 0x57
  PWASM_OP_I64_LE_U, // 0x58
  PWASM_OP_I64_GE_S, // 0x59
  PWASM_OP_I64_GE_U, // 0x5A
  PWASM_OP_F32_EQ, // 0x5B
  PWASM_OP_F32_NE, // 0x5C
  PWASM_OP_F32_LT, // 0x5D
  PWASM_OP_F32_GT, // 0x5E
  PWASM_OP_F32_LE, // 0x5F
  PWASM_OP_F32_GE, // 0x60
  PWASM_OP_F64_EQ, // 0x61
  PWASM_OP_F64_NE, // 0x62
  PWASM_OP_F64_LT, // 0x63
  PWASM_OP_F64_GT, // 0x64
  PWASM_OP_F64_LE, // 0x65
  PWASM_OP_F64_GE, // 0x66
  PWASM_OP_I32_CLZ, // 0x67
  PWASM_OP_I32_CTZ, // 0x68
  PWASM_OP_I32_POPCNT, // 0x69
  PWASM_OP_I32_ADD, // 0x6A
  PWASM_OP_I32_SUB, // 0x6B
  PWASM_OP_I32_MUL, // 0x6C
  PWASM_OP_I32_DIV_S, // 0x6D
  PWASM_OP_I32_DIV_U, // 0x6E
  PWASM_OP_I32_REM_S, // 0x6F
  PWASM_OP_I32_REM_U, // 0x70
  PWASM_OP_I32_AND, // 0x71
  PWASM_OP_I32_OR, // 0x72
  PWASM_OP_I32_XOR, // 0x73
  PWASM_OP_I32_SHL, // 0x74
  PWASM_OP_I32_SHR_S, // 0x75
  PWASM_OP_I32_SHR_U, // 0x76
  PWASM_OP_I32_ROTL, // 0x77
  PWASM_OP_I32_ROTR, // 0x78
  PWASM_OP_I64_CLZ, // 0x79
  PWASM_OP_I64_CTZ, // 0x7A
  PWASM_OP_I64_POPCNT, // 0x7B
  PWASM_OP_I64_ADD, // 0x7C
  PWASM_OP_I64_SUB, // 0x7D
  PWASM_OP_I64_MUL, // 0x7E
  PWASM_OP_I64_DIV_S, // 0x7F
  PWASM_OP_I64_DIV_U, // 0x80
  PWASM_OP_I64_REM_S, // 0x81
  PWASM_OP_I64_REM_U, // 0x82
  PWASM_OP_I64_AND, // 0x83
  PWASM_OP_I64_OR, // 0x84
  PWASM_OP_I64_XOR, // 0x85
  PWASM_OP_I64_SHL, // 0x86
  PWASM_OP_I64_SHR_S, // 0x87
  PWASM_OP_I64_SHR_U, // 0x88
  PWASM_OP_I64_ROTL, // 0x89
  PWASM_OP_I64_ROTR, // 0x8A
  PWASM_OP_F32_ABS, // 0x8B
  PWASM_OP_F32_NEG, // 0x8C
  PWASM_OP_F32_CEIL, // 0x8D
  PWASM_OP_F32_FLOOR, // 0x8E
  PWASM_OP_F32_TRUNC, // 0x8F
  PWASM_OP_F32_NEAREST, // 0x90
  PWASM_OP_F32_SQRT, // 0x91
  PWASM_OP_F32_ADD, // 0x92
  PWASM_OP_F32_SUB, // 0x93
  PWASM_OP_F32_MUL, // 0x94
  PWASM_OP_F32_DIV, // 0x95
  PWASM_OP_F32_MIN, // 0x96
  PWASM_OP_F32_MAX, // 0x97
  PWASM_OP_F32_COPYSIGN, // 0x98
  PWASM_OP_F64_ABS, // 0x99
  PWASM_OP_F64_NEG, // 0x9A
  PWASM_OP_F64_CEIL, // 0x9B
  PWASM_OP_F64_FLOOR, // 0x9C
  PWASM_OP_F64_TRUNC, // 0x9D
  PWASM_OP_F64_NEAREST, // 0x9E
  PWASM_OP_F64_SQRT, // 0x9F
  PWASM_OP_F64_ADD, // 0xA0
  PWASM_OP_F64_SUB, // 0xA1
  PWASM_OP_F64_MUL, // 0xA2
  PWASM_OP_F64_DIV, // 0xA3
  PWASM_OP_F64_MIN, // 0xA4
  PWASM_OP_F64_MAX, // 0xA5
  PWASM_OP_F64_COPYSIGN, // 0xA6
  PWASM_OP_I32_WRAP_I64, // 0xA7
  PWASM_OP_I32_TRUNC_F32_S, // 0xA8
  PWASM_OP_I32_TRUNC_F32_U, // 0xA9
  PWASM_OP_I32_TRUNC_F64_S, // 0xAA
  PWASM_OP_I32_TRUNC_F64_U, // 0xAB
  PWASM_OP_I64_EXTEND_I32_S, // 0xAC
  PWASM_OP_I64_EXTEND_I32_U, // 0xAD
  PWASM_OP_I64_TRUNC_F32_S, // 0xAE
  PWASM_OP_I64_TRUNC_F32_U, // 0xAF
  PWASM_OP_I64_TRUNC_F64_S, // 0xB0
  PWASM_OP_I64_TRUNC_F64_U, // 0xB1
  PWASM_OP_F32_CONVERT_I32_S, // 0xB2
  PWASM_OP_F32_CONVERT_I32_U, // 0xB3
  PWASM_OP_F32_CONVERT_I64_S, // 0xB4
  PWASM_OP_F32_CONVERT_I64_U, // 0xB5
  PWASM_OP_F32_DEMOTE_F64, // 0xB6
  PWASM_OP_F64_CONVERT_I32_S, // 0xB7
  PWASM_OP_F64_CONVERT_I32_U, // 0xB8
  PWASM_OP_F64_CONVERT_I64_S, // 0xB9
  PWASM_OP_F64_CONVERT_I64_U, // 0xBA
  PWASM_OP_F64_PROMOTE_F32, // 0xBB
  PWASM_OP_I32_REINTERPRET_F32, // 0xBC
  PWASM_OP_I64_REINTERPRET_F64, // 0xBD
  PWASM_OP_F32_REINTERPRET_I32, // 0xBE
  PWASM_OP_F64_REINTERPRET_I64, // 0xBF
  PWASM_OP_I32_EXTEND8_S, // 0xC0
  PWASM_OP_I32_EXTEND16_S, // 0xC1
  PWASM_OP_I64_EXTEND8_S, // 0xC2
  PWASM_OP_I64_EXTEND16_S, // 0xC3
  PWASM_OP_I64_EXTEND32_S, // 0xC4
  PWASM_OP_LAST, // 0xC5
  PWASM_OP_LAST, // 0xC6
  PWASM_OP_LAST, // 0xC7
  PWASM_OP_LAST, // 0xC8
  PWASM_OP_LAST, // 0xC9
  PWASM_OP_LAST, // 0xCA
  PWASM_OP_LAST, // 0xCB
  PWASM_OP_LAST, // 0xCC
  PWASM_OP_LAST, // 0xCD
  PWASM_OP_LAST, // 0xCE
  PWASM_OP_LAST, // 0xCF
  PWASM_OP_LAST, // 0xD0
  PWASM_OP_LAST, // 0xD1
  PWASM_OP_LAST, // 0xD2
  PWASM_OP_LAST, // 0xD3
  PWASM_OP_LAST, // 0xD4
  PWASM_OP_LAST, // 0xD5
  PWASM_OP_LAST, // 0xD6
  PWASM_OP_LAST, // 0xD7
  PWASM_OP_LAST, // 0xD8
  PWASM_OP_LAST, // 0xD9
  PWASM_OP_LAST, // 0xDA
  PWASM_OP_LAST, // 0xDB
  PWASM_OP_LAST, // 0xDC
  PWASM_OP_LAST, // 0xDD
  PWASM_OP_LAST, // 0xDE
  PWASM_OP_LAST, // 0xDF
  PWASM_OP_LAST, // 0xE0
  PWASM_OP_LAST, // 0xE1
  PWASM_OP_LAST, // 0xE2
  PWASM_OP_LAST, // 0xE3
  PWASM_OP_LAST, // 0xE4
  PWASM_OP_LAST, // 0xE5
  PWASM_OP_LAST, // 0xE6
  PWASM_OP_LAST, // 0xE7
  PWASM_OP_LAST, // 0xE8
  PWASM_OP_LAST, // 0xE9
  PWASM_OP_LAST, // 0xEA
  PWASM_OP_LAST, // 0xEB
  PWASM_OP_LAST, // 0xEC
  PWASM_OP_LAST, // 0xED
  PWASM_OP_LAST, // 0xEE
  PWASM_OP_LAST, // 0xEF
  PWASM_OP_LAST, // 0xF0
  PWASM_OP_LAST, // 0xF1
  PWASM_OP_LAST, // 0xF2
  PWASM_OP_LAST, // 0xF3
  PWASM_OP_LAST, // 0xF4
  PWASM_OP_LAST, // 0xF5
  PWASM_OP_LAST, // 0xF6
  PWASM_OP_LAST, // 0xF7
  PWASM_OP_LAST, // 0xF8
  PWASM_OP_LAST, // 0xF9
  PWASM_OP_LAST, // 0xFA
  PWASM_OP_LAST, // 0xFB
  PWASM_OP_LAST, // 0xFC
  PWASM_OP_LAST, // 0xFD
  PWASM_OP_LAST, // 0xFE
  PWASM_OP_LAST, // 0xFF
  PWASM_OP_I32_TRUNC_SAT_F32_S, // 0x00
  PWASM_OP_I32_TRUNC_SAT_F32_U, // 0x01
  PWASM_OP_I32_TRUNC_SAT_F64_S, // 0x02
  PWASM_OP_I32_TRUNC_SAT_F64_U, // 0x03
  PWASM_OP_I64_TRUNC_SAT_F32_S, // 0x04
  PWASM_OP_I64_TRUNC_SAT_F32_U, // 0x05
  PWASM_OP_I64_TRUNC_SAT_F64_S, // 0x06
  PWASM_OP_I64_TRUNC_SAT_F64_U, // 0x07
  PWASM_OP_LAST, // 0x08
  PWASM_OP_LAST, // 0x09
  PWASM_OP_LAST, // 0x0A
  PWASM_OP_LAST, // 0x0B
  PWASM_OP_LAST, // 0x0C
  PWASM_OP_LAST, // 0x0D
  PWASM_OP_LAST, // 0x0E
  PWASM_OP_LAST, // 0x0F
  PWASM_OP_LAST, // 0x10
  PWASM_OP_LAST, // 0x11
  PWASM_OP_LAST, // 0x12
  PWASM_OP_LAST, // 0x13
  PWASM_OP_LAST, // 0x14
  PWASM_OP_LAST, // 0x15
  PWASM_OP_LAST, // 0x16
  PWASM_OP_LAST, // 0x17
  PWASM_OP_LAST, // 0x18
  PWASM_OP_LAST, // 0x19
  PWASM_OP_LAST, // 0x1A
  PWASM_OP_LAST, // 0x1B
  PWASM_OP_LAST, // 0x1C
  PWASM_OP_LAST, // 0x1D
  PWASM_OP_LAST, // 0x1E
  PWASM_OP_LAST, // 0x1F
  PWASM_OP_LAST, // 0x20
  PWASM_OP_LAST, // 0x21
  PWASM_OP_LAST, // 0x22
  PWASM_OP_LAST, // 0x23
  PWASM_OP_LAST, // 0x24
  PWASM_OP_LAST, // 0x25
  PWASM_OP_LAST, // 0x26
  PWASM_OP_LAST, // 0x27
  PWASM_OP_LAST, // 0x28
  PWASM_OP_LAST, // 0x29
  PWASM_OP_LAST, // 0x2A
  PWASM_OP_LAST, // 0x2B
  PWASM_OP_LAST, // 0x2C
  PWASM_OP_LAST, // 0x2D
  PWASM_OP_LAST, // 0x2E
  PWASM_OP_LAST, // 0x2F
  PWASM_OP_LAST, // 0x30
  PWASM_OP_LAST, // 0x31
  PWASM_OP_LAST, // 0x32
  PWASM_OP_LAST, // 0x33
  PWASM_OP_LAST, // 0x34
  PWASM_OP_LAST, // 0x35
  PWASM_OP_LAST, // 0x36
  PWASM_OP_LAST, // 0x37
  PWASM_OP_LAST, // 0x38
  PWASM_OP_LAST, // 0x39
  PWASM_OP_LAST, // 0x3A
  PWASM_OP_LAST, // 0x3B
  PWASM_OP_LAST, // 0x3C
  PWASM_OP_LAST, // 0x3D
  PWASM_OP_LAST, // 0x3E
  PWASM_OP_LAST, // 0x3F
  PWASM_OP_LAST, // 0x40
  PWASM_OP_LAST, // 0x41
  PWASM_OP_LAST, // 0x42
  PWASM_OP_LAST, // 0x43
  PWASM_OP_LAST, // 0x44
  PWASM_OP_LAST, // 0x45
  PWASM_OP_LAST, // 0x46
  PWASM_OP_LAST, // 0x47
  PWASM_OP_LAST, // 0x48
  PWASM_OP_LAST, // 0x49
  PWASM_OP_LAST, // 0x4A
  PWASM_OP_LAST, // 0x4B
  PWASM_OP_LAST, // 0x4C
  PWASM_OP_LAST, // 0x4D
  PWASM_OP_LAST, // 0x4E
  PWASM_OP_LAST, // 0x4F
  PWASM_OP_LAST, // 0x50
  PWASM_OP_LAST, // 0x51
  PWASM_OP_LAST, // 0x52
  PWASM_OP_LAST, // 0x53
  PWASM_OP_LAST, // 0x54
  PWASM_OP_LAST, // 0x55
  PWASM_OP_LAST, // 0x56
  PWASM_OP_LAST, // 0x57
  PWASM_OP_LAST, // 0x58
  PWASM_OP_LAST, // 0x59
  PWASM_OP_LAST, // 0x5A
  PWASM_OP_LAST, // 0x5B
  PWASM_OP_LAST, // 0x5C
  PWASM_OP_LAST, // 0x5D
  PWASM_OP_LAST, // 0x5E
  PWASM_OP_LAST, // 0x5F
  PWASM_OP_LAST, // 0x60
  PWASM_OP_LAST, // 0x61
  PWASM_OP_LAST, // 0x62
  PWASM_OP_LAST, // 0x63
  PWASM_OP_LAST, // 0x64
  PWASM_OP_LAST, // 0x65
  PWASM_OP_LAST, // 0x66
  PWASM_OP_LAST, // 0x67
  PWASM_OP_LAST, // 0x68
  PWASM_OP_LAST, // 0x69
  PWASM_OP_LAST, // 0x6A
  PWASM_OP_LAST, // 0x6B
  PWASM_OP_LAST, // 0x6C
  PWASM_OP_LAST, // 0x6D
  PWASM_OP_LAST, // 0x6E
  PWASM_OP_LAST, // 0x6F
  PWASM_OP_LAST, // 0x70
  PWASM_OP_LAST, // 0x71
  PWASM_OP_LAST, // 0x72
  PWASM_OP_LAST, // 0x73
  PWASM_OP_LAST, // 0x74
  PWASM_OP_LAST, // 0x75
  PWASM_OP_LAST, // 0x76
  PWASM_OP_LAST, // 0x77
  PWASM_OP_LAST, // 0x78
  PWASM_OP_LAST, // 0x79
  PWASM_OP_LAST, // 0x7A
  PWASM_OP_LAST, // 0x7B
  PWASM_OP_LAST, // 0x7C
  PWASM_OP_LAST, // 0x7D
  PWASM_OP_LAST, // 0x7E
  PWASM_OP_LAST, // 0x7F
  PWASM_OP_LAST, // 0x80
  PWASM_OP_LAST, // 0x81
  PWASM_OP_LAST, // 0x82
  PWASM_OP_LAST, // 0x83
  PWASM_OP_LAST, // 0x84
  PWASM_OP_LAST, // 0x85
  PWASM_OP_LAST, // 0x86
  PWASM_OP_LAST, // 0x87
  PWASM_OP_LAST, // 0x88
  PWASM_OP_LAST, // 0x89
  PWASM_OP_LAST, // 0x8A
  PWASM_OP_LAST, // 0x8B
  PWASM_OP_LAST, // 0x8C
  PWASM_OP_LAST, // 0x8D
  PWASM_OP_LAST, // 0x8E
  PWASM_OP_LAST, // 0x8F
  PWASM_OP_LAST, // 0x90
  PWASM_OP_LAST, // 0x91
  PWASM_OP_LAST, // 0x92
  PWASM_OP_LAST, // 0x93
  PWASM_OP_LAST, // 0x94
  PWASM_OP_LAST, // 0x95
  PWASM_OP_LAST, // 0x96
  PWASM_OP_LAST, // 0x97
  PWASM_OP_LAST, // 0x98
  PWASM_OP_LAST, // 0x99
  PWASM_OP_LAST, // 0x9A
  PWASM_OP_LAST, // 0x9B
  PWASM_OP_LAST, // 0x9C
  PWASM_OP_LAST, // 0x9D
  PWASM_OP_LAST, // 0x9E
  PWASM_OP_LAST, // 0x9F
  PWASM_OP_LAST, // 0xA0
  PWASM_OP_LAST, // 0xA1
  PWASM_OP_LAST, // 0xA2
  PWASM_OP_LAST, // 0xA3
  PWASM_OP_LAST, // 0xA4
  PWASM_OP_LAST, // 0xA5
  PWASM_OP_LAST, // 0xA6
  PWASM_OP_LAST, // 0xA7
  PWASM_OP_LAST, // 0xA8
  PWASM_OP_LAST, // 0xA9
  PWASM_OP_LAST, // 0xAA
  PWASM_OP_LAST, // 0xAB
  PWASM_OP_LAST, // 0xAC
  PWASM_OP_LAST, // 0xAD
  PWASM_OP_LAST, // 0xAE
  PWASM_OP_LAST, // 0xAF
  PWASM_OP_LAST, // 0xB0
  PWASM_OP_LAST, // 0xB1
  PWASM_OP_LAST, // 0xB2
  PWASM_OP_LAST, // 0xB3
  PWASM_OP_LAST, // 0xB4
  PWASM_OP_LAST, // 0xB5
  PWASM_OP_LAST, // 0xB6
  PWASM_OP_LAST, // 0xB7
  PWASM_OP_LAST, // 0xB8
  PWASM_OP_LAST, // 0xB9
  PWASM_OP_LAST, // 0xBA
  PWASM_OP_LAST, // 0xBB
  PWASM_OP_LAST, // 0xBC
  PWASM_OP_LAST, // 0xBD
  PWASM_OP_LAST, // 0xBE
  PWASM_OP_LAST, // 0xBF
  PWASM_OP_LAST, // 0xC0
  PWASM_OP_LAST, // 0xC1
  PWASM_OP_LAST, // 0xC2
  PWASM_OP_LAST, // 0xC3
  PWASM_OP_LAST, // 0xC4
  PWASM_OP_LAST, // 0xC5
  PWASM_OP_LAST, // 0xC6
  PWASM_OP_LAST, // 0xC7
  PWASM_OP_LAST, // 0xC8
  PWASM_OP_LAST, // 0xC9
  PWASM_OP_LAST, // 0xCA
  PWASM_OP_LAST, // 0xCB
  PWASM_OP_LAST, // 0xCC
  PWASM_OP_LAST, // 0xCD
  PWASM_OP_LAST, // 0xCE
  PWASM_OP_LAST, // 0xCF
  PWASM_OP_LAST, // 0xD0
  PWASM_OP_LAST, // 0xD1
  PWASM_OP_LAST, // 0xD2
  PWASM_OP_LAST, // 0xD3
  PWASM_OP_LAST, // 0xD4
  PWASM_OP_LAST, // 0xD5
  PWASM_OP_LAST, // 0xD6
  PWASM_OP_LAST, // 0xD7
  PWASM_OP_LAST, // 0xD8
  PWASM_OP_LAST, // 0xD9
  PWASM_OP_LAST, // 0xDA
  PWASM_OP_LAST, // 0xDB
  PWASM_OP_LAST, // 0xDC
  PWASM_OP_LAST, // 0xDD
  PWASM_OP_LAST, // 0xDE
  PWASM_OP_LAST, // 0xDF
  PWASM_OP_LAST, // 0xE0
  PWASM_OP_LAST, // 0xE1
  PWASM_OP_LAST, // 0xE2
  PWASM_OP_LAST, // 0xE3
  PWASM_OP_LAST, // 0xE4
  PWASM_OP_LAST, // 0xE5
  PWASM_OP_LAST, // 0xE6
  PWASM_OP_LAST, // 0xE7
  PWASM_OP_LAST, // 0xE8
  PWASM_OP_LAST, // 0xE9
  PWASM_OP_LAST, // 0xEA
  PWASM_OP_LAST, // 0xEB
  PWASM_OP_LAST, // 0xEC
  PWASM_OP_LAST, // 0xED
  PWASM_OP_LAST, // 0xEE
  PWASM_OP_LAST, // 0xEF
  PWASM_OP_LAST, // 0xF0
  PWASM_OP_LAST, // 0xF1
  PWASM_OP_LAST, // 0xF2
  PWASM_OP_LAST, // 0xF3
  PWASM_OP_LAST, // 0xF4
  PWASM_OP_LAST, // 0xF5
  PWASM_OP_LAST, // 0xF6
  PWASM_OP_LAST, // 0xF7
  PWASM_OP_LAST, // 0xF8
  PWASM_OP_LAST, // 0xF9
  PWASM_OP_LAST, // 0xFA
  PWASM_OP_LAST, // 0xFB
  PWASM_OP_LAST, // 0xFC
  PWASM_OP_LAST, // 0xFD
  PWASM_OP_LAST, // 0xFE
  PWASM_OP_LAST, // 0xFF
  PWASM_OP_V128_LOAD, // 0x00
  PWASM_OP_I16X8_LOAD8X8_S, // 0x01
  PWASM_OP_I16X8_LOAD8X8_U, // 0x02
  PWASM_OP_I32X4_LOAD16X4_S, // 0x03
  PWASM_OP_I32X4_LOAD16X4_U, // 0x04
  PWASM_OP_I64X2_LOAD32X2_S, // 0x05
  PWASM_OP_I64X2_LOAD32X2_U, // 0x06
  PWASM_OP_V8X16_LOAD_SPLAT, // 0x07
  PWASM_OP_V16X8_LOAD_SPLAT, // 0x08
  PWASM_OP_V32X4_LOAD_SPLAT, // 0x09
  PWASM_OP_V64X2_LOAD_SPLAT, // 0x0A
  PWASM_OP_V128_STORE, // 0x0B
  PWASM_OP_V128_CONST, // 0x0C
  PWASM_OP_V8X16_SHUFFLE, // 0x0D
  PWASM_OP_V8X16_SWIZZLE, // 0x0E
  PWASM_OP_I8X16_SPLAT, // 0x0F
  PWASM_OP_I16X8_SPLAT, // 0x10
  PWASM_OP_I32X4_SPLAT, // 0x11
  PWASM_OP_I64X2_SPLAT, // 0x12
  PWASM_OP_F32X4_SPLAT, // 0x13
  PWASM_OP_F64X2_SPLAT, // 0x14
  PWASM_OP_I8X16_EXTRACT_LANE_S, // 0x15
  PWASM_OP_I8X16_EXTRACT_LANE_U, // 0x16
  PWASM_OP_I8X16_REPLACE_LANE, // 0x17
  PWASM_OP_I16X8_EXTRACT_LANE_S, // 0x18
  PWASM_OP_I16X8_EXTRACT_LANE_U, // 0x19
  PWASM_OP_I16X8_REPLACE_LANE, // 0x1A
  PWASM_OP_I32X4_EXTRACT_LANE, // 0x1B
  PWASM_OP_I32X4_REPLACE_LANE, // 0x1C
  PWASM_OP_I64X2_EXTRACT_LANE, // 0x1D
  PWASM_OP_I64X2_REPLACE_LANE, // 0x1E
  PWASM_OP_F32X4_EXTRACT_LANE, // 0x1F
  PWASM_OP_F32X4_REPLACE_LANE, // 0x20
  PWASM_OP_F64X2_EXTRACT_LANE, // 0x21
  PWASM_OP_F64X2_REPLACE_LANE, // 0x22
  PWASM_OP_I8X16_EQ, // 0x23
  PWASM_OP_I8X16_NE, // 0x24
  PWASM_OP_I8X16_LT_S, // 0x25
  PWASM_OP_I8X16_LT_U, // 0x26
  PWASM_OP_I8X16_GT_S, // 0x27
  PWASM_OP_I8X16_GT_U, // 0x28
  PWASM_OP_I8X16_LE_S, // 0x29
  PWASM_OP_I8X16_LE_U, // 0x2A
  PWASM_OP_I8X16_GE_S, // 0x2B
  PWASM_OP_I8X16_GE_U, // 0x2C
  PWASM_OP_I16X8_EQ, // 0x2D
  PWASM_OP_I16X8_NE, // 0x2E
  PWASM_OP_I16X8_LT_S, // 0x2F
  PWASM_OP_I16X8_LT_U, // 0x30
  PWASM_OP_I16X8_GT_S, // 0x31
  PWASM_OP_I16X8_GT_U, // 0x32
  PWASM_OP_I16X8_LE_S, // 0x33
  PWASM_OP_I16X8_LE_U, // 0x34
  PWASM_OP_I16X8_GE_S, // 0x35
  PWASM_OP_I16X8_GE_U, // 0x36
  PWASM_OP_I32X4_EQ, // 0x37
  PWASM_OP_I32X4_NE, // 0x38
  PWASM_OP_I32X4_LT_S, // 0x39
  PWASM_OP_I32X4_LT_U, // 0x3A
  PWASM_OP_I32X4_GT_S, // 0x3B
  PWASM_OP_I32X4_GT_U, // 0x3C
  PWASM_OP_I32X4_LE_S, // 0x3D
  PWASM_OP_I32X4_LE_U, // 0x3E
  PWASM_OP_I32X4_GE_S, // 0x3F
  PWASM_OP_I32X4_GE_U, // 0x40
  PWASM_OP_F32X4_EQ, // 0x41
  PWASM_OP_F32X4_NE, // 0x42
  PWASM_OP_F32X4_LT, // 0x43
  PWASM_OP_F32X4_GT, // 0x44
  PWASM_OP_F32X4_LE, // 0x45
  PWASM_OP_F32X4_GE, // 0x46
  PWASM_OP_F64X2_EQ, // 0x47
  PWASM_OP_F64X2_NE, // 0x48
  PWASM_OP_F64X2_LT, // 0x49
  PWASM_OP_F64X2_GT, // 0x4A
  PWASM_OP_F64X2_LE, // 0x4B
  PWASM_OP_F64X2_GE, // 0x4C
  PWASM_OP_V128_NOT, // 0x4D
  PWASM_OP_V128_AND, // 0x4E
  PWASM_OP_V128_ANDNOT, // 0x4F
  PWASM_OP_V128_OR, // 0x50
  PWASM_OP_V128_XOR, // 0x51
  PWASM_OP_V128_BITSELECT, // 0x52
  PWASM_OP_LAST, // 0x53
  PWASM_OP_LAST, // 0x54
  PWASM_OP_LAST, // 0x55
  PWASM_OP_LAST, // 0x56
  PWASM_OP_LAST, // 0x57
  PWASM_OP_LAST, // 0x58
  PWASM_OP_LAST, // 0x59
  PWASM_OP_LAST, // 0x5A
  PWASM_OP_LAST, // 0x5B
  PWASM_OP_LAST, // 0x5C
  PWASM_OP_LAST, // 0x5D
  PWASM_OP_LAST, // 0x5E
  PWASM_OP_LAST, // 0x5F
  PWASM_OP_I8X16_ABS, // 0x60
  PWASM_OP_I8X16_NEG, // 0x61
  PWASM_OP_I8X16_ANY_TRUE, // 0x62
  PWASM_OP_I8X16_ALL_TRUE, // 0x63
  PWASM_OP_LAST, // 0x64
  PWASM_OP_I8X16_NARROW_I16X8_S, // 0x65
  PWASM_OP_I8X16_NARROW_I16X8_U, // 0x66
  PWASM_OP_LAST, // 0x67
  PWASM_OP_LAST, // 0x68
  PWASM_OP_LAST, // 0x69
  PWASM_OP_LAST, // 0x6A
  PWASM_OP_I8X16_SHL, // 0x6B
  PWASM_OP_I8X16_SHR_S, // 0x6C
  PWASM_OP_I8X16_SHR_U, // 0x6D
  PWASM_OP_I8X16_ADD, // 0x6E
  PWASM_OP_I8X16_ADD_SATURATE_S, // 0x6F
  PWASM_OP_I8X16_ADD_SATURATE_U, // 0x70
  PWASM_OP_I8X16_SUB, // 0x71
  PWASM_OP_I8X16_SUB_SATURATE_S, // 0x72
  PWASM_OP_I8X16_SUB_SATURATE_U, // 0x73
  PWASM_OP_LAST, // 0x74
  PWASM_OP_LAST, // 0x75
  PWASM_OP_I8X16_MIN_S, // 0x76
  PWASM_OP_I8X16_MIN_U, // 0x77
  PWASM_OP_I8X16_MAX_S, // 0x78
  PWASM_OP_I8X16_MAX_U, // 0x79
  PWASM_OP_LAST, // 0x7A
  PWASM_OP_I8X16_AVGR_U, // 0x7B
  PWASM_OP_LAST, // 0x7C
  PWASM_OP_LAST, // 0x7D
  PWASM_OP_LAST, // 0x7E
  PWASM_OP_LAST, // 0x7F
  PWASM_OP_I16X8_ABS, // 0x80
  PWASM_OP_I16X8_NEG, // 0x81
  PWASM_OP_I16X8_ANY_TRUE, // 0x82
  PWASM_OP_I16X8_ALL_TRUE, // 0x83
  PWASM_OP_LAST, // 0x84
  PWASM_OP_I16X8_NARROW_I32X4_S, // 0x85
  PWASM_OP_I16X8_NARROW_I32X4_U, // 0x86
  PWASM_OP_I16X8_WIDEN_LOW_I8X16_S, // 0x87
  PWASM_OP_I16X8_WIDEN_HIGH_I8X16_S, // 0x88
  PWASM_OP_I16X8_WIDEN_LOW_I8X16_U, // 0x89
  PWASM_OP_I16X8_WIDEN_HIGH_I8X16_U, // 0x8A
  PWASM_OP_I16X8_SHL, // 0x8B
  PWASM_OP_I16X8_SHR_S, // 0x8C
  PWASM_OP_I16X8_SHR_U, // 0x8D
  PWASM_OP_I16X8_ADD, // 0x8E
  PWASM_OP_I16X8_ADD_SATURATE_S, // 0x8F
  PWASM_OP_I16X8_ADD_SATURATE_U, // 0x90
  PWASM_OP_I16X8_SUB, // 0x91
  PWASM_OP_I16X8_SUB_SATURATE_S, // 0x92
  PWASM_OP_I16X8_SUB_SATURATE_U, // 0x93
  PWASM_OP_LAST, // 0x94
  PWASM_OP_I16X8_MUL, // 0x95
  PWASM_OP_I16X8_MIN_S, // 0x96
  PWASM_OP_I16X8_MIN_U, // 0x97
  PWASM_OP_I16X8_MAX_S, // 0x98
  PWASM_OP_I16X8_MAX_U, // 0x99
  PWASM_OP_LAST, // 0x9A
  PWASM_OP_I16X8_AVGR_U, // 0x9B
  PWASM_OP_LAST, // 0x9C
  PWASM_OP_LAST, // 0x9D
  PWASM_OP_LAST, // 0x9E
  PWASM_OP_LAST, // 0x9F
  PWASM_OP_I32X4_ABS, // 0xA0
  PWASM_OP_I32X4_NEG, // 0xA1
  PWASM_OP_I32X4_ANY_TRUE, // 0xA2
  PWASM_OP_I32X4_ALL_TRUE, // 0xA3
  PWASM_OP_LAST, // 0xA4
  PWASM_OP_LAST, // 0xA5
  PWASM_OP_LAST, // 0xA6
  PWASM_OP_I32X4_WIDEN_LOW_I16X8_S, // 0xA7
  PWASM_OP_I32X4_WIDEN_HIGH_I16X8_S, // 0xA8
  PWASM_OP_I32X4_WIDEN_LOW_I16X8_U, // 0xA9
  PWASM_OP_I32X4_WIDEN_HIGH_I16X8_U, // 0xAA
  PWASM_OP_I32X4_SHL, // 0xAB
  PWASM_OP_I32X4_SHR_S, // 0xAC
  PWASM_OP_I32X4_SHR_U, // 0xAD
  PWASM_OP_I32X4_ADD, // 0xAE
  PWASM_OP_LAST, // 0xAF
  PWASM_OP_LAST, // 0xB0
  PWASM_OP_I32X4_SUB, // 0xB1
  PWASM_OP_LAST, // 0xB2
  PWASM_OP_LAST, // 0xB3
  PWASM_OP_LAST, // 0xB4
  PWASM_OP_I32X4_MUL, // 0xB5
  PWASM_OP_I32X4_MIN_S, // 0xB6
  PWASM_OP_I32X4_MIN_U, // 0xB7
  PWASM_OP_I32X4_MAX_S, // 0xB8
  PWASM_OP_I32X4_MAX_U, // 0xB9
  PWASM_OP_LAST, // 0xBA
  PWASM_OP_LAST, // 0xBB
  PWASM_OP_LAST, // 0xBC
  PWASM_OP_LAST, // 0xBD
  PWASM_OP_LAST, // 0xBE
  PWASM_OP_LAST, // 0xBF
  PWASM_OP_LAST, // 0xC0
  PWASM_OP_I64X2_NEG, // 0xC1
  PWASM_OP_LAST, // 0xC2
  PWASM_OP_LAST, // 0xC3
  PWASM_OP_LAST, // 0xC4
  PWASM_OP_LAST, // 0xC5
  PWASM_OP_LAST, // 0xC6
  PWASM_OP_LAST, // 0xC7
  PWASM_OP_LAST, // 0xC8
  PWASM_OP_LAST, // 0xC9
  PWASM_OP_LAST, // 0xCA
  PWASM_OP_I64X2_SHL, // 0xCB
  PWASM_OP_I64X2_SHR_S, // 0xCC
  PWASM_OP_I64X2_SHR_U, // 0xCD
  PWASM_OP_I64X2_ADD, // 0xCE
  PWASM_OP_LAST, // 0xCF
  PWASM_OP_LAST, // 0xD0
  PWASM_OP_I64X2_SUB, // 0xD1
  PWASM_OP_LAST, // 0xD2
  PWASM_OP_LAST, // 0xD3
  PWASM_OP_LAST, // 0xD4
  PWASM_OP_I64X2_MUL, // 0xD5
  PWASM_OP_LAST, // 0xD6
  PWASM_OP_LAST, // 0xD7
  PWASM_OP_LAST, // 0xD8
  PWASM_OP_LAST, // 0xD9
  PWASM_OP_LAST, // 0xDA
  PWASM_OP_LAST, // 0xDB
  PWASM_OP_LAST, // 0xDC
  PWASM_OP_LAST, // 0xDD
  PWASM_OP_LAST, // 0xDE
  PWASM_OP_LAST, // 0xDF
  PWASM_OP_F32X4_ABS, // 0xE0
  PWASM_OP_F32X4_NEG, // 0xE1
  PWASM_OP_LAST, // 0xE2
  PWASM_OP_F32X4_SQRT, // 0xE3
  PWASM_OP_F32X4_ADD, // 0xE4
  PWASM_OP_F32X4_SUB, // 0xE5
  PWASM_OP_F32X4_MUL, // 0xE6
  PWASM_OP_F32X4_DIV, // 0xE7
  PWASM_OP_F32X4_MIN, // 0xE8
  PWASM_OP_F32X4_MAX, // 0xE9
  PWASM_OP_LAST, // 0xEA
  PWASM_OP_LAST, // 0xEB
  PWASM_OP_F64X2_ABS, // 0xEC
  PWASM_OP_F64X2_NEG, // 0xED
  PWASM_OP_LAST, // 0xEE
  PWASM_OP_F64X2_SQRT, // 0xEF
  PWASM_OP_F64X2_ADD, // 0xF0
  PWASM_OP_F64X2_SUB, // 0xF1
  PWASM_OP_F64X2_MUL, // 0xF2
  PWASM_OP_F64X2_DIV, // 0xF3
  PWASM_OP_F64X2_MIN, // 0xF4
  PWASM_OP_F64X2_MAX, // 0xF5
  PWASM_OP_LAST, // 0xF6
  PWASM_OP_LAST, // 0xF7
  PWASM_OP_I32X4_TRUNC_SAT_F32X4_S, // 0xF8
  PWASM_OP_I32X4_TRUNC_SAT_F32X4_U, // 0xF9
  PWASM_OP_F32X4_CONVERT_I32X4_S, // 0xFA
  PWASM_OP_F32X4_CONVERT_I32X4_U, // 0xFB
  PWASM_OP_LAST, // 0xFC
  PWASM_OP_LAST, // 0xFD
  PWASM_OP_LAST, // 0xFE
  PWASM_OP_LAST, // 0xFF
};

/**
 * Convert integer value to opcode enumeration value.
 *
 * Returns `PWASM_OP_LAST` if the set or value are invalid.
 */
static pwasm_op_t
pwasm_op_from_u32(
  const pwasm_ops_t set,
  const uint32_t val
) {
  const bool ok = pwasm_op_is_valid(set, val);
  const size_t ofs = (ok) ? (set * 256 + val) : 255;
  return PWASM_OP_MAP[ofs];
}

/**
 * Parse opcode into `dst` from buffer `src`.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_op(
  pwasm_op_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_inst_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // check source length
  if (curr.len < 1) {
    cbs->on_error("short instruction", cb_data);
    return 0;
  }

  // get initial byte
  const uint8_t byte = curr.ptr[0];

  // advance
  curr = pwasm_buf_step(curr, 1);
  num_bytes++;

  // switch on initial byte (check for op set prefix)
  switch (byte) {
  case 0xFC: // trunc_sat set
    {
      // check length
      if (!curr.len) {
        // log error, return failure
        cbs->on_error("missing trunc_sat opcode", cb_data);
        return 0;
      }

      // convert to opcode, check for error
      const pwasm_op_t op = pwasm_op_from_u32(PWASM_OPS_TRUNC_SAT, curr.ptr[0]);
      if (op == PWASM_OP_LAST) {
        // log error, return failure
        D("unknown trunc_sat opcode = 0x%02X", curr.ptr[0]);
        cbs->on_error("unknown trunc_sat opcode", cb_data);
        return 0;
      }

      // copy to destination, return number of bytes consumed
      *dst = op;
      return num_bytes + 1;
    }

    break;
  case 0xFD: // simd set
    {
      // decode leb128 value, check for error
      uint32_t val;
      const size_t len = pwasm_u32_decode(&val, curr);
      if (!len) {
        // log error, return failure
        cbs->on_error("invalid simd opcode", cb_data);
        return 0;
      }

      // convert to opcode, check for error
      const pwasm_op_t op = pwasm_op_from_u32(PWASM_OPS_SIMD, val);
      if (op == PWASM_OP_LAST) {
        // log error, return failure
        D("unknown simd opcode = 0x%08X", val);
        cbs->on_error("unknown simd opcode", cb_data);
        return 0;
      }

      // copy to destination, return number of bytes consumed
      *dst = op;
      return num_bytes + len;
    }

    break;
  default: // main
    {
      // convert to opcode, check for error
      const pwasm_op_t op = pwasm_op_from_u32(PWASM_OPS_MAIN, byte);
      if (op == PWASM_OP_LAST) {
        // log error, return failure
        D("unknown opcode = 0x%02X", byte);
        cbs->on_error("unknown opcode", cb_data);
        return 0;
      }

      // copy to destination, return number of bytes consumed
      *dst = op;
      return num_bytes;
    }
  }

  // return failure
  return 0;
}

/**
 * Parse inst into `dst` from buffer `src`.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_inst(
  pwasm_inst_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_inst_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // D("src.ptr = %p, src.len = %zu", (void*) src.ptr, src.len);

  pwasm_op_t op;
  {
    // parse op, check for error
    const size_t len = pwasm_parse_op(&op, src, cbs, cb_data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // build instruction
  pwasm_inst_t in = {
    .op = op,
  };

  // get op immediate
  switch (pwasm_op_get_imm(in.op)) {
  case PWASM_IMM_NONE:
    // do nothing
    break;
  case PWASM_IMM_BLOCK:
    {
      // parse block type, check for error
      int32_t block_type;
      const size_t len = pwasm_s32_decode(&block_type, curr);
      if (!len) {
        cbs->on_error("missing block type", cb_data);
        return 0;
      }

      // save block type
      in.v_block.block_type = block_type;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_BR_TABLE:
    {
      // build labels callbacks
      const pwasm_parse_labels_cbs_t labels_cbs = {
        .on_labels = cbs->on_labels,
        .on_error  = cbs->on_error,
      };

      // parse labels immediate, check for error
      pwasm_slice_t labels = { 0, 0 };
      const size_t len = pwasm_parse_labels(&labels, curr, &labels_cbs, cb_data);
      if (!len) {
        cbs->on_error("bad br_table labels immediate", cb_data);
        return 0;
      }

      // save labels buffer, increment length
      in.v_br_table = labels;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_INDEX:
  case PWASM_IMM_LANE_INDEX:
    {
      // get index, check for error
      uint32_t id = 0;
      const size_t len = pwasm_u32_decode(&id, curr);
      if (!len) {
        cbs->on_error("bad immediate index value", cb_data);
        return 0;
      }

      // save index
      in.v_index = id;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_CALL_INDIRECT:
    {
      {
        // get type index, check for error
        uint32_t id = 0;
        const size_t len = pwasm_u32_decode(&id, curr);
        if (!len) {
          cbs->on_error("bad call_indirect type index", cb_data);
          return 0;
        }

        // save index
        in.v_index = id;

        // advance
        curr = pwasm_buf_step(curr, len);
        num_bytes += len;
      }

      {
        // get table ID, check for error
        uint32_t table_id = 0;
        const size_t len = pwasm_u32_decode(&table_id, curr);
        if (!len) {
          cbs->on_error("call_indirect immediate: bad table index", cb_data);
          return 0;
        }

        // check table ID, check for error
        // FIXME: should this be handled in validation layer?
        if (table_id != 0) {
          cbs->on_error("call_indirect immediate: non-zero table index", cb_data);
          return 0;
        }

        // advance
        curr = pwasm_buf_step(curr, len);
        num_bytes += len;
      }
    }

    break;
  case PWASM_IMM_MEM:
    {
      // get align value, check for error
      uint32_t align = 0;
      {
        const size_t len = pwasm_u32_decode(&align, curr);
        if (!len) {
          cbs->on_error("bad align value", cb_data);
          return 0;
        }

        // advance
        curr = pwasm_buf_step(curr, len);
        num_bytes += len;
      }

      // get offset value, check for error
      uint32_t offset = 0;
      {
        const size_t len = pwasm_u32_decode(&offset, curr);
        if (!len) {
          cbs->on_error("bad offset value", cb_data);
          return 0;
        }

        // advance
        curr = pwasm_buf_step(curr, len);
        num_bytes += len;
      }

      // save alignment and offset
      in.v_mem.align = align;
      in.v_mem.offset = offset;
    }

    break;
  case PWASM_IMM_I32_CONST:
    {
      // get value, check for error
      uint32_t val = 0;
      const size_t len = pwasm_u32_decode(&val, curr);
      if (!len) {
        cbs->on_error("bad i32 value", cb_data);
        return 0;
      }

      // save value
      in.v_i32 = val;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_I64_CONST:
    {
      // get value, check for error
      uint64_t val = 0;
      const size_t len = pwasm_u64_decode(&val, curr);
      if (!len) {
        cbs->on_error("bad i64 value", cb_data);
        return 0;
      }

      // save value
      in.v_i64 = val;
      D("in.v_i64 = 0x%016lx", val);

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_F32_CONST:
    {
      // immediate size, in bytes
      const size_t len = sizeof(float);

      // check length
      if (curr.len < len) {
        cbs->on_error("incomplete f32", cb_data);
        return 0;
      }

      union {
        uint8_t u8[sizeof(float)];
        float f32;
      } u;
      memcpy(u.u8, curr.ptr, len);

      // save value, increment length
      in.v_f32 = u.f32;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_F64_CONST:
    {
      // immediate size, in bytes
      const size_t len = sizeof(double);

      // check length
      if (curr.len < len) {
        cbs->on_error("incomplete f64", cb_data);
        return 0;
      }

      union {
        uint8_t u8[sizeof(double)];
        double f64;
      } u;
      memcpy(u.u8, curr.ptr, len);

      // save value, increment length
      in.v_f64 = u.f64;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_V128_CONST:
    {
      // immediate size, in bytes
      const size_t len = sizeof(pwasm_v128_t);

      // check length
      if (curr.len < len) {
        cbs->on_error("incomplete v128", cb_data);
        return 0;
      }

      // copy immediate value, increment length
      memcpy(in.v_v128.i8, curr.ptr, len);

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  default:
    // never reached
    cbs->on_error("invalid immediate type", cb_data);
    return 0;
  }

  // save to result
  *dst = in;

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

typedef struct {
  // maximum depth of control stack
  size_t max_ctl_depth;

  // maximum depth of value stack
  size_t max_val_depth;
} pwasm_expr_stats_t;

typedef struct {
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);
  pwasm_slice_t (*on_stats)(const pwasm_expr_stats_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_expr_cbs_t;

/**
 * Parse expr into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_expr(
  pwasm_slice_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_expr_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // check source length
  if (src.len < 1) {
    cbs->on_error("invalid expr", cb_data);
    return 0;
  }

  // build instruction parser callbacks
  const pwasm_parse_inst_cbs_t in_cbs = {
    .on_labels  = cbs->on_labels,
    .on_error   = cbs->on_error,
  };

  pwasm_inst_t insts[PWASM_BATCH_SIZE];
  pwasm_slice_t in_slice = { 0, 0 };

  // TODO: track value stack depth too
  pwasm_depth_t ctl_depth = { 1, 1 };
  pwasm_depth_t val_depth = { 0, 0 };

  size_t ofs = 0, expr_ofs = 0;
  while ((ctl_depth.val > 0) && curr.len) {
    // parse instruction, check for error
    pwasm_inst_t in;
    const size_t len = pwasm_parse_inst(&in, curr, &in_cbs, cb_data);
    if (!len) {
      return 0;
    }

    if (pwasm_op_is_enter(in.op) || (in.op == PWASM_OP_END)) {
      D("ctl_depth = { val: %zu, max: %zu }", ctl_depth.val, ctl_depth.max);

      if (pwasm_op_is_enter(in.op) && !pwasm_depth_add(&ctl_depth, 1)) {
        cbs->on_error("control stack depth overflow", cb_data);
        return 0;
      } else if ((in.op == PWASM_OP_END) && !pwasm_depth_sub(&ctl_depth, 1)) {
        cbs->on_error("control stack depth underflow", cb_data);
        return 0;
      }
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;

    insts[ofs++] = in;
    expr_ofs++;

    if (ofs == LEN(insts)) {
      // clear offset
      ofs = 0;

      // flush batch, check for error
      const pwasm_slice_t slice = cbs->on_insts(insts, LEN(insts), cb_data);
      if (!slice.len) {
        return 0;
      }

      if (in_slice.len) {
        // keep offset, increment slice length (not first batch)
        in_slice.len += slice.len;
      } else {
        // set offset and length (first batch)
        in_slice = slice;
      }
    }
  }

  if (ofs > 0) {
    // flush batch, check for error
    const pwasm_slice_t slice = cbs->on_insts(insts, ofs, cb_data);
    if (!slice.len) {
      return 0;
    }

    if (in_slice.len) {
      // keep offset, increment slice length (not first batch)
      in_slice.len += slice.len;
    } else {
      // set offset and length (first batch)
      in_slice = slice;
    }
  }

  // check for control stack depth mismatch
  if (ctl_depth.val) {
    cbs->on_error("unbalanced expression", cb_data);
    return 0;
  }

  // TODO: check curr.len for functions
  // if (curr.len > 0) {
  //   cbs->on_error("unbalanced expression", cb_data);
  //   return 0;
  // }

  if (cbs->on_stats) {
    // emit expr stats
    cbs->on_stats((pwasm_expr_stats_t) {
      .max_ctl_depth = ctl_depth.max,
      .max_val_depth = val_depth.max,
    }, cb_data);
  }

  // save result to destination
  *dst = in_slice;

  // return number of bytes consumed
  return num_bytes;
}

/**
 * Parse constant expr into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_const_expr(
  pwasm_slice_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_expr_cbs_t * const cbs,
  void * const cb_data
) {
  // Note: parsing a constant expression is identical to parsing a
  // normal expression; the instruction limiting for constant
  // expressions is handled in the validation layer
  return pwasm_parse_expr(dst, src, cbs, cb_data);
}

/**
 * Parse global type into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_global_type(
  pwasm_global_type_t * const dst,
  const pwasm_buf_t src,
  void (*on_error)(const char *, void *),
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  on_error = on_error ? on_error : pwasm_null_on_error;

  // check source length
  if (src.len < 2) {
    on_error("incomplete global type", cb_data);
    return 0;
  }

  // parse value type, check for error
  pwasm_value_type_t type;
  {
    const size_t len = pwasm_u32_decode(&type, curr);
    if (!len) {
      on_error("bad global value type", cb_data);
      return 0;
    }

    // advance buffer
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // check value type
  if (!pwasm_is_valid_value_type(type)) {
    on_error("bad global value type", cb_data);
    return 0;
  }

  if (!curr.len) {
    on_error("missing global mutable flag", cb_data);
    return 0;
  }

  // get mutable flag, check for error
  const uint8_t mut = curr.ptr[0];
  if ((mut != 0) && (mut != 1)) {
    on_error("bad global mutable flag value", cb_data);
    return 0;
  }

  // advance buffer
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

  // write result
  *dst = (pwasm_global_type_t) {
    .type = type,
    .mutable = (mut == 1),
  };

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

/**
 * Parse global into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_global(
  pwasm_global_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_expr_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  D("parsing global, src.ptr = %p, src.len = %zu", (void*) src.ptr, src.len);

  // check source length
  if (src.len < 3) {
    cbs->on_error("incomplete global", cb_data);
    return 0;
  }

  D("parsing global type dst = %p", (void*) dst);
  pwasm_global_type_t type;
  {
    // parse type, check for error
    const size_t len = pwasm_parse_global_type(&type, curr, cbs->on_error, cb_data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  pwasm_slice_t expr;
  {
    D("parsing expr (before), dst = %p", (void*) dst);
    // parse expr, check for error
    const size_t len = pwasm_parse_const_expr(&expr, curr, cbs, cb_data);
    if (!len) {
      return 0;
    }
    D("parsing expr (after), dst = %p", (void*) dst);

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  *dst = (pwasm_global_t) {
    .type = type,
    .expr = expr,
  };

  // silence scanner
  (void) curr;

  // return total number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_slice_t (*on_bytes)(const uint8_t *, size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_export_cbs_t;

typedef struct {
  pwasm_slice_t (*on_bytes)(const uint8_t *, size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_import_cbs_t;

static inline size_t
pwasm_parse_import_func(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  (void) cbs;
  (void) cb_data;
  return pwasm_u32_decode(&(dst->func), src);
}

static inline size_t
pwasm_parse_import_table(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  void (*on_error)(const char *, void *) = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error;
  return pwasm_parse_table(&(dst->table), src, on_error, cb_data);
}

static inline size_t
pwasm_parse_import_mem(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  void (*on_error)(const char *, void *) = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error;
  return pwasm_parse_limits(&(dst->mem), src, on_error, cb_data);
}

static inline size_t
pwasm_parse_import_global(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  void (*on_error)(const char *, void *) = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error;
  return pwasm_parse_global_type(&(dst->global), src, on_error, cb_data);
}

static inline size_t
pwasm_parse_import_invalid(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  (void) dst;
  (void) src;

  if (cbs && cbs->on_error) {
    cbs->on_error("bad import type", cb_data);
  }

  return 0;
}

static inline size_t
pwasm_parse_import_data(
  const pwasm_import_type_t type,
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  D("type = %u:%s, src.ptr = %p, src.len = %zu", type, pwasm_import_type_get_name(type), (void*) src.ptr, src.len);
  switch (type) {
#define PWASM_IMPORT_TYPE(ID, TEXT, NAME) \
  case PWASM_IMPORT_TYPE_ ## ID: \
    return pwasm_parse_import_ ## NAME (dst, src, cbs, cb_data);
PWASM_IMPORT_TYPES
#undef PWASM_IMPORT_TYPE
  default:
    return pwasm_parse_import_invalid(dst, src, cbs, cb_data);
  }
}

static size_t
pwasm_parse_import(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  const pwasm_parse_buf_cbs_t buf_cbs = {
    .on_error = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error,
  };

  // get on_bytes callback
  pwasm_slice_t (*on_bytes)(const uint8_t *, size_t, void *) = (cbs && cbs->on_bytes) ? cbs->on_bytes : NULL;

  // parse module name, check for error
  pwasm_slice_t names[2] = { 0 };
  for (size_t i = 0; i < 2; i++) {
    pwasm_buf_t buf;
    const size_t len = pwasm_parse_buf(&buf, curr, &buf_cbs, cb_data);
    if (!len) {
      buf_cbs.on_error("invalid import module name length", cb_data);
      return 0;
    }

    if ((buf.len > 0) && on_bytes) {
      // add bytes, check for error
      names[i] = on_bytes(buf.ptr, buf.len, cb_data);
      if (!names[i].len) {
        buf_cbs.on_error("empty import module name", cb_data);
        return 0;
      }
    }

    // advance buffer, increment byte count
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;

    D("curr.ptr = %p, curr.len = %zu", (void*) curr.ptr, curr.len);
  }

  if (curr.len < 2) {
    buf_cbs.on_error("missing import type", cb_data);
    return 0;
  }

  // get import type
  const pwasm_import_type_t type = curr.ptr[0];

  // advance
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

  // build result
  pwasm_import_t tmp = {
    .module = names[0],
    .name = names[1],
    .type = type,
  };

  const size_t len = pwasm_parse_import_data(type, &tmp, curr, cbs, cb_data);
  if (!len) {
    buf_cbs.on_error("invalid import data", cb_data);
    return 0;
  }

  // add length to result
  curr = pwasm_buf_step(curr, len);
  num_bytes += len;

  D("dst = %p, num_bytes = %zu", (void*) dst, num_bytes);

  // save result to destination
  *dst = tmp;

  D("dst = %p, num_bytes = %zu (tmp written)", (void*) dst, num_bytes);

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

/**
 * Parse export into +dst+ from source buffer +src+, consuming a
 * maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_export(
  pwasm_export_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_export_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  pwasm_buf_t name_buf;
  {
    // parse name, check for error
    const size_t len = pwasm_parse_buf(&name_buf, curr, NULL, NULL);
    if (!len) {
      cbs->on_error("bad export name", cb_data);
      return 0;
    }
    D("len = %zu", len);

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // get name slice
  pwasm_slice_t name = cbs->on_bytes(name_buf.ptr, name_buf.len, cb_data);
  if (name.len != name_buf.len) {
    return 0;
  }

  // get export type
  pwasm_import_type_t type;
  {
    // parse type, check for error
    const size_t len = pwasm_parse_import_type(&type, curr);
    if (!len) {
      D("bad type = %u", curr.ptr[0]);
      cbs->on_error("bad export type", cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // parse id, check for error
  uint32_t id;
  {
    const size_t len = pwasm_u32_decode(&id, curr);
    if (!len) {
      cbs->on_error("bad export index", cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }
  D("type = %u:%s, id = %u", type, pwasm_import_type_get_name(type), id);

  *dst = (pwasm_export_t) {
    .name = name,
    .type = type,
    .id   = id,
  };

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_slice_t (*on_funcs)(const uint32_t *, const size_t, void *);
  // pwasm_slice_t (*on_labels)(const pwasm_inst_t *, const size_t, void *);
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_elem_cbs_t;

typedef struct {
  const pwasm_parse_elem_cbs_t * cbs;
  void *cb_data;
  pwasm_slice_t funcs;
  bool success;
} pwasm_parse_elem_t;

static void
pwasm_parse_elem_on_items(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_parse_elem_t *data = cb_data;

  if (!data->success) {
    return;
  }

  // add ids, check for error
  const pwasm_slice_t funcs = data->cbs->on_funcs(rows, num, data->cb_data);
  if (!funcs.len) {
    // mark failure and return
    data->success = false;
    return;
  }

  if (data->funcs.len) {
    // sucessive set, append to length
    data->funcs.len += funcs.len;
  } else {
    // initial slice, set offset and length
    data->funcs = funcs;
  }
}

/**
 * Parse element into +dst+ from source buffer +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_elem(
  pwasm_elem_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_elem_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  uint32_t table_id = 0;
  {
    // get table_id, check for error
    const size_t len = pwasm_u32_decode(&table_id, curr);
    if (!len) {
      cbs->on_error("bad element table id", cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  pwasm_slice_t expr = { 0, 0 };
  {
    // build parse expr callbacks
    const pwasm_parse_expr_cbs_t expr_cbs = {
      .on_insts = cbs->on_insts,
      .on_error = cbs->on_error,
    };

    // parse expr, check for error
    const size_t len = pwasm_parse_const_expr(&expr, curr, &expr_cbs, cb_data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // build parse func ids callback data
  pwasm_parse_elem_t data = {
    .cbs = cbs,
    .cb_data = cb_data,
    .success = true,
    .funcs = { 0, 0 },
  };

  {
    // build parse func ids callbacks
    const pwasm_parse_u32s_cbs_t u32s_cbs = {
      .on_items = pwasm_parse_elem_on_items,
      .on_error = cbs->on_error,
    };

    // parse function ids, check for error
    const size_t len = pwasm_parse_u32s(curr, &u32s_cbs, &data);
    if (!len || !data.success) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // save result
  *dst = (pwasm_elem_t) {
    .table_id = table_id,
    .expr     = expr,
    .funcs    = data.funcs,
  };

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_slice_t (*on_locals)(const pwasm_local_t *, size_t, void *);
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_code_cbs_t;

static size_t
pwasm_parse_code_locals_local(
  pwasm_local_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_code_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  if (curr.len < 2) {
    cbs->on_error("empty local", cb_data);
    return 0;
  }

  // get local num, check for error
  uint32_t num;
  {
    const size_t len = pwasm_u32_decode(&num, curr);
    if (!len) {
      cbs->on_error("invalid local num", cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  if (!curr.len) {
    cbs->on_error("missing local type", cb_data);
    return 0;
  }

  // get value type, check for error
  const pwasm_value_type_t type = curr.ptr[0];
  if (!pwasm_is_valid_value_type(type)) {
    cbs->on_error("invalid local type", cb_data);
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

  // populate result
  *dst = (pwasm_local_t) {
    .num  = num,
    .type = type,
  };

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

static size_t
pwasm_parse_code_locals(
  pwasm_slice_t * const dst,
  size_t * const dst_max_locals,
  const pwasm_buf_t src,
  const pwasm_parse_code_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;
  size_t max_locals = 0;
  pwasm_slice_t local = { 0, 0 };

  uint32_t count = 0;
  {
    // get local count, check for error
    const size_t len = pwasm_u32_decode(&count, curr);
    if (!len) {
      cbs->on_error("invalid locals count", cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }
  D("num locals = %u", count);

  // locals buffer
  pwasm_local_t locals[PWASM_BATCH_SIZE];

  size_t ofs = 0;
  for (size_t i = 0; i < count; i++) {
    // get local, check for error
    const size_t len = pwasm_parse_code_locals_local(locals + ofs, curr, cbs, cb_data);
    if (!len) {
      return 0;
    }

    // add to total number of locals for this function
    max_locals += locals[ofs].num;

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;

    // increment offset
    ofs++;
    if (ofs == LEN(locals)) {
      ofs = 0;

      // flush queue
      const pwasm_slice_t slice = cbs->on_locals(locals, LEN(locals), cb_data);

      if (local.len > 0) {
        // successive set, append to length
        local.len += slice.len;
      } else {
        // initial set, copy offset and length
        local = slice;
      }
    }
  }

  if (ofs) {
    // flush queue
    const pwasm_slice_t slice = cbs->on_locals(locals, ofs, cb_data);

    if (local.len > 0) {
      // successive set, append to length
      local.len += slice.len;
    } else {
      // initial set, copy offset and length
      local = slice;
    }
  }

  // copy results to destination
  *dst = local;
  *dst_max_locals = max_locals;

  // return number of bytes read
  return num_bytes;
}

typedef struct {
  const pwasm_parse_code_cbs_t * const cbs;
  void *cb_data;
  pwasm_slice_t insts;
  bool success;
} pwasm_parse_code_expr_t;

static size_t
pwasm_parse_code_expr(
  pwasm_slice_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_code_cbs_t * const src_cbs,
  void * const cb_data
) {
  // build callbacks
  const pwasm_parse_expr_cbs_t cbs = {
    .on_labels  = src_cbs->on_labels,
    .on_insts   = src_cbs->on_insts,
    .on_error   = src_cbs->on_error,
  };

  // parse expr, check for error
  pwasm_slice_t expr;
  const size_t len = pwasm_parse_expr(&expr, src, &cbs, cb_data);
  if (!len) {
    return 0;
  }

  // copy result to destination
  *dst = expr;

  // return number of bytes consumed
  return len;
}

static size_t
pwasm_parse_code(
  pwasm_func_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_code_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  uint32_t code_len;
  {
    // parse code length, check for error
    const size_t len = pwasm_u32_decode(&code_len, curr);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // FIXME: need to adjust curr here based on code_len

  pwasm_slice_t locals;
  size_t max_locals = 0;
  {
    // parse locals, check for error
    const size_t len = pwasm_parse_code_locals(&locals, &max_locals, curr, cbs, cb_data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  pwasm_slice_t expr;
  {
    // parse expr, check for error
    const size_t len = pwasm_parse_code_expr(&expr, curr, cbs, cb_data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  *dst = (pwasm_func_t) {
    .locals = locals,
    .max_locals = max_locals,
    .frame_size = 0, // populated elsewhere
    .expr = expr,
  };

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

/**
 * Data segment parser callbacks.
 */
typedef struct {
  /**
   * Called with bytes that should be cached.
   *
   * Callback should append bytes to internal byte buffer and then
   * return a slice indicating the offset and length within the byte
   * buffer.
   *
   */
  pwasm_slice_t (*on_bytes)(const uint8_t *, const size_t, void *);

  /**
   * Called with instructions that should be cached.
   *
   * Callback should append the instructions to an internal list of
   * instructions and then slice indicating the offset and length within
   * the internal instruction list.
   *
   */
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);

  /**
   * Called when a parse error occurs.
   */
  void (*on_error)(const char *, void *);
} pwasm_parse_segment_cbs_t;

static size_t
pwasm_parse_segment(
  pwasm_segment_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_segment_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  uint32_t mem_id;
  {
    // parse memory ID, check for error
    const size_t len = pwasm_u32_decode(&mem_id, curr);
    if (!len) {
      cbs->on_error("invalid memory id", cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  pwasm_slice_t expr;
  {
    // build expr parse cbs
    const pwasm_parse_expr_cbs_t expr_cbs = {
      .on_insts = cbs->on_insts,
      .on_error = cbs->on_error,
    };

    // parse expr, check for error
    const size_t len = pwasm_parse_expr(&expr, curr, &expr_cbs, cb_data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  pwasm_buf_t data_buf;
  {
    // build buf parse cbs
    const pwasm_parse_buf_cbs_t buf_cbs = {
      .on_error = cbs->on_error,
    };

    // parse buf, check for error
    const size_t len = pwasm_parse_buf(&data_buf, curr, &buf_cbs, cb_data);
    if (!len) {
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // append bytes, get slice, check for error
  pwasm_slice_t data = cbs->on_bytes(data_buf.ptr, data_buf.len, cb_data);
  if (data_buf.len != data.len) {
    return 0;
  }

  *dst = (pwasm_segment_t) {
    .mem_id = mem_id,
    .expr   = expr,
    .data   = data,
  };

  // silence scanner
  (void) curr;

  // return number of bytes consumed
  return num_bytes;
}

static const uint8_t PWASM_HEADER[] = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };

static inline size_t
pwasm_header_parse(
  pwasm_header_t * const dst,
  const pwasm_buf_t src
) {
  if (src.len < 2) {
    return 0;
  }

  // get section type
  const pwasm_section_type_t type = src.ptr[0];

  // advance past type, build length buffer
  const pwasm_buf_t buf = pwasm_buf_step(src, 1);

  // get section length, check for error
  uint32_t len;
  const size_t ofs = pwasm_u32_decode(&len, buf);
  if (!ofs) {
    return 0;
  }

  if (dst) {
    // build result, copy result to destination
    *dst = (pwasm_header_t) { type, len };
  }

  // return number of bytes consumed
  return ofs + 1;
}

static void *
pwasm_mem_ctx_init_defaults_on_realloc(
  void *ptr,
  const size_t len,
  void *cb_data
) {
  (void) cb_data;
  return realloc(ptr, len);
}

static const pwasm_mem_cbs_t
PWASM_MEM_CTX_INIT_DEFAULTS_CBS = {
  .on_realloc = pwasm_mem_ctx_init_defaults_on_realloc,
  .on_error   = pwasm_null_on_error,
};

pwasm_mem_ctx_t
pwasm_mem_ctx_init_defaults(
  void *cb_data
) {
  return (pwasm_mem_ctx_t) {
    .cbs      = &PWASM_MEM_CTX_INIT_DEFAULTS_CBS,
    .cb_data  = cb_data,
  };
}

void *
pwasm_realloc(
  pwasm_mem_ctx_t * const mem_ctx,
  void *ptr,
  const size_t len
) {
  return mem_ctx->cbs->on_realloc(ptr, len, mem_ctx->cb_data);
}

void
pwasm_fail(
  pwasm_mem_ctx_t * const mem_ctx,
  const char * const text
) {
  mem_ctx->cbs->on_error(text, mem_ctx->cb_data);
}

static inline size_t
pwasm_get_num_bytes(
  const size_t stride,
  const size_t num_items
) {
  const size_t page_size = sysconf(_SC_PAGESIZE);
  const size_t num_bytes = stride * num_items;
  const size_t num_pages = (
    (num_bytes / page_size) +
    ((num_bytes % page_size) ? 1 : 0)
  );

  return page_size * num_pages;
}

static bool
pwasm_vec_resize(
  pwasm_vec_t * const vec,
  const size_t new_capacity
) {
  const size_t num_bytes = pwasm_get_num_bytes(vec->stride, new_capacity);
  uint8_t * const ptr = pwasm_realloc(vec->mem_ctx, vec->rows, num_bytes);
  if (!ptr && (num_bytes > 0)) {
    // return failure
    return false;
  }

  // update vector pointer, capacity, and row count
  vec->rows = ptr;
  vec->max_rows = num_bytes / vec->stride;
  vec->num_rows = (vec->num_rows < vec->max_rows) ? vec->num_rows : vec->max_rows;

  // return success
  return true;
}

bool
pwasm_vec_init(
  pwasm_mem_ctx_t * const mem_ctx,
  pwasm_vec_t * const ret,
  const size_t stride
) {
  pwasm_vec_t vec = {
    .mem_ctx  = mem_ctx,
    .stride   = stride,
  };

  if (ret) {
    // copy result to destination
    memcpy(ret, &vec, sizeof(pwasm_vec_t));
  }

  // return success
  return true;
}

bool
pwasm_vec_fini(
  pwasm_vec_t * const vec
) {
  if (!vec->max_rows) {
    // return success
    return true;
  }

  return pwasm_vec_resize(vec, 0);
}

size_t
pwasm_vec_get_size(
  const pwasm_vec_t * const vec
) {
  return vec->num_rows;
}

size_t
pwasm_vec_get_stride(
  const pwasm_vec_t * const vec
) {
  return vec->stride;
}

const void *
pwasm_vec_get_data(
  const pwasm_vec_t * const vec
) {
  return vec->rows;
}

bool
pwasm_vec_push_uninitialized(
  pwasm_vec_t * const vec,
  const size_t num_rows,
  size_t * const ret_ofs
) {
  // calculate new length, check for overflow
  const size_t new_len = vec->num_rows + num_rows;
  if (new_len <= vec->num_rows) {
    // return failure
    return false;
  }

  // resize (if necessary), check for error
  if ((new_len >= vec->max_rows) && !pwasm_vec_resize(vec, new_len)) {
    // return failure
    return false;
  }

  if (ret_ofs) {
    // return offset
    *ret_ofs = vec->num_rows;
  }

  // save new length
  vec->num_rows = new_len;

  // return success
  return true;
}

bool
pwasm_vec_push(
  pwasm_vec_t * const vec,
  const size_t num_rows,
  const void * const src,
  size_t * const ret_ofs
) {
  size_t dst_ofs = 0;
  if (!pwasm_vec_push_uninitialized(vec, num_rows, &dst_ofs)) {
    return false;
  }

  if (ret_ofs) {
    // return offset
    *ret_ofs = dst_ofs;
  }

  if (src) {
    // copy data
    uint8_t * const dst = vec->rows + vec->stride * dst_ofs;
    memcpy(dst, src, num_rows * vec->stride);
  }

  // return success
  return true;
}

void
pwasm_vec_clear(
  pwasm_vec_t * const vec
) {
  vec->num_rows = 0;
}

bool
pwasm_vec_pop(
  pwasm_vec_t * const vec,
  void * const dst
) {
  // check row count
  if (!vec->num_rows) {
    // return error
    return false;
  }

  if (dst) {
    // copy tail to destination
    uint8_t * const src = vec->rows + vec->stride * (vec->num_rows - 1);
    memcpy(dst, src, vec->stride);
  }

  // decriment row count, return success
  vec->num_rows--;
  return true;
}

/**
 * Get a pointer to the Nth entry from the tail of the vector.
 *
 * Returns NULL if the vector is empty or if the offset is out of
 * bounds.
 */
const void *
pwasm_vec_peek_tail(
  const pwasm_vec_t * const vec,
  const size_t ofs
) {
  const bool ok = (vec->num_rows > 0) && (ofs < vec->num_rows);
  const size_t num_bytes = vec->stride * (vec->num_rows - 1 - ofs);
  return ok ? vec->rows + num_bytes : NULL;
}

/**
 * Truncate vector.
 *
 * Returns false if the new size is greater than the current size.
 */
static bool
pwasm_vec_shrink(
  pwasm_vec_t * const vec,
  const size_t new_size
) {
  const bool ok = (new_size <= vec->num_rows);
  vec->num_rows = ok ? new_size : vec->num_rows;
  return ok;
}

#define BUILDER_VECS \
  BUILDER_VEC(u32, uint32_t, dummy) \
  BUILDER_VEC(section, pwasm_header_t, u32) \
  BUILDER_VEC(custom_section, pwasm_custom_section_t, section) \
  BUILDER_VEC(type, pwasm_type_t, custom_section) \
  BUILDER_VEC(import, pwasm_import_t, type) \
  BUILDER_VEC(inst, pwasm_inst_t, import) \
  BUILDER_VEC(global, pwasm_global_t, inst) \
  BUILDER_VEC(func, uint32_t, global) \
  BUILDER_VEC(table, pwasm_table_t, func) \
  BUILDER_VEC(mem, pwasm_limits_t, table) \
  BUILDER_VEC(export, pwasm_export_t, mem) \
  BUILDER_VEC(local, pwasm_local_t, export) \
  BUILDER_VEC(code, pwasm_func_t, local) \
  BUILDER_VEC(elem, pwasm_elem_t, code) \
  BUILDER_VEC(segment, pwasm_segment_t, elem) \
  BUILDER_VEC(byte, uint8_t, segment) // note: keep at tail for alignment

bool
pwasm_builder_init(
  pwasm_mem_ctx_t * const mem_ctx,
  pwasm_builder_t * const ret
) {
  pwasm_builder_t b = {
    .mem_ctx = mem_ctx,
  };

#define BUILDER_VEC(NAME, TYPE, PREV) \
  if (!pwasm_vec_init(mem_ctx, &(b.NAME ## s), sizeof(TYPE))) { \
    return false; \
  }
BUILDER_VECS
#undef BUILDER_VEC

  // FIXME: double-check list above
  memcpy(ret, &b, sizeof(pwasm_builder_t));

  // return success
  return true;
}

void
pwasm_builder_fini(
  pwasm_builder_t * const builder
) {
#define BUILDER_VEC(name, type, prev) \
  if (!pwasm_vec_fini(&(builder->name ## s))) { \
    /* log error */ \
    pwasm_fail(builder->mem_ctx, "finalizing builder " #name " failed"); \
  }
BUILDER_VECS
#undef BUILDER_VEC
}

#define BUILDER_VEC(name, type, prev) \
  static pwasm_slice_t \
  pwasm_builder_push_ ## name ## s( \
    pwasm_builder_t * const builder, \
    const type * const src_ptr, \
    const size_t src_len \
  ) { \
    pwasm_vec_t * const vec = &(builder->name ## s); \
    size_t ofs; \
    if (pwasm_vec_push(vec, src_len, src_ptr, &ofs)) { \
      return (pwasm_slice_t) { ofs, src_len }; \
    } else { \
      /* log error */ \
      pwasm_fail(builder->mem_ctx, "builder " #name " push failed"); \
      return (pwasm_slice_t) { 0, 0 }; \
    } \
  }
BUILDER_VECS
#undef BUILDER_VEC

static size_t
pwasm_builder_get_size(
  const pwasm_builder_t * const builder
) {
  return (
  #define BUILDER_VEC(name, type, prev) \
    pwasm_vec_get_size(&(builder->name ## s)) * sizeof(type) +
  BUILDER_VECS
  #undef BUILDER_VEC
  0 /* sentinel */
  );
}

static bool
pwasm_builder_resolve_jumps(
  const pwasm_builder_t * const builder,
  pwasm_mod_t * const mod
) {
  pwasm_inst_t * const insts = (pwasm_inst_t*) mod->insts;

  // init offset stack
  pwasm_vec_t stack;
  if (!pwasm_vec_init(builder->mem_ctx, &stack, sizeof(size_t))) {
    pwasm_fail(builder->mem_ctx, "builder control stack init");
    return false;
  }

  for (size_t i = 0; i < mod->num_codes; i++) {
    const pwasm_func_t func = mod->codes[i];

    // clear stack
    pwasm_vec_clear(&stack);

    {
      // push control offset
      const size_t expr_ofs = 0;
      if (!pwasm_vec_push(&stack, 1, &expr_ofs, NULL)) {
        pwasm_fail(builder->mem_ctx, "builder control stack push");
        return false;
      }
    }

    for (size_t j = 0; j < func.expr.len; j++) {
      const pwasm_inst_t in = insts[func.expr.ofs + j];

      switch (in.op) {
      case PWASM_OP_IF:
      case PWASM_OP_BLOCK:
      case PWASM_OP_LOOP:
        // push control offset
        if (!pwasm_vec_push(&stack, 1, &j, NULL)) {
          pwasm_fail(builder->mem_ctx, "builder control stack push");
          return false;
        }

        // clear else/end offsets
        insts[func.expr.ofs + j].v_block.else_ofs = 0;
        insts[func.expr.ofs + j].v_block.end_ofs = 0;

        break;
      case PWASM_OP_ELSE:
        {
          // get top of stack
          const size_t * const ofs = pwasm_vec_peek_tail(&stack, 0);
          if (!ofs) {
            pwasm_fail(builder->mem_ctx, "builder control stack peek");
            return false;
          }

          // save else offset
          insts[func.expr.ofs + *ofs].v_block.else_ofs = j - *ofs;
        }

        break;
      case PWASM_OP_END:
        {
          // get top of stack
          size_t ofs;
          if (!pwasm_vec_pop(&stack, &ofs)) {
            pwasm_fail(builder->mem_ctx, "builder control stack pop");
            return false;
          }

          if (
            (insts[func.expr.ofs + ofs].op == PWASM_OP_IF) &&
            (insts[func.expr.ofs + ofs].v_block.else_ofs)
          ) {
            const size_t if_ofs = func.expr.ofs + ofs;
            const size_t else_ofs = if_ofs + insts[if_ofs].v_block.else_ofs;

            // cache end ofs for else inst
            insts[else_ofs].v_block.end_ofs = j - else_ofs;
          }

          // save end offset
          insts[func.expr.ofs + ofs].v_block.end_ofs = j - ofs;
        }

        break;
      default:
        // do nothing
        break;
      }
    }
  }

  // finalize offset stack
  pwasm_vec_fini(&stack);

  // return success
  return true;
}

static bool
pwasm_builder_build_mod(
  const pwasm_builder_t * const builder,
  pwasm_mod_t * const dst
) {
  const size_t total_num_bytes = pwasm_builder_get_size(builder);
  uint8_t * const ptr = pwasm_realloc(builder->mem_ctx, NULL, total_num_bytes);
  if (!ptr && (total_num_bytes > 0)) {
    // return failure
    return false;
  }

  // get item counts
  #define BUILDER_VEC(name, type, prev) \
    const size_t num_ ## name ## s = pwasm_vec_get_size(&(builder->name ## s));
  BUILDER_VECS
  #undef BUILDER_VEC

  // get output byte sizes
  #define BUILDER_VEC(name, type, prev) \
    const size_t name ## s_size = num_ ## name ## s * sizeof(type);
  BUILDER_VECS
  #undef BUILDER_VEC

  // get source pointers
  #define BUILDER_VEC(name, type, prev) \
    const uint8_t * const name ## s_src = pwasm_vec_get_data(&(builder->name ## s));
  BUILDER_VECS
  #undef BUILDER_VEC

  // dummy (used to expand macro below)
  uint8_t * const dummys_dst = ptr;
  const size_t dummys_size = 0;

  // get dest pointers
  #define BUILDER_VEC(name, type, prev) \
    uint8_t * const name ## s_dst = prev ## s_dst + prev ## s_size;
  BUILDER_VECS
  #undef BUILDER_VEC

  // populate result
  const pwasm_mod_t mod = {
    .mem_ctx = builder->mem_ctx,
    .mem = { ptr, total_num_bytes },

    .num_import_types = {
      builder->num_import_types[PWASM_IMPORT_TYPE_FUNC],
      builder->num_import_types[PWASM_IMPORT_TYPE_TABLE],
      builder->num_import_types[PWASM_IMPORT_TYPE_MEM],
      builder->num_import_types[PWASM_IMPORT_TYPE_GLOBAL],
    },

    .max_indices = {
      pwasm_vec_get_size(&(builder->funcs)) + builder->num_import_types[PWASM_IMPORT_TYPE_FUNC],
      pwasm_vec_get_size(&(builder->tables)) + builder->num_import_types[PWASM_IMPORT_TYPE_TABLE],
      pwasm_vec_get_size(&(builder->mems)) + builder->num_import_types[PWASM_IMPORT_TYPE_MEM],
      pwasm_vec_get_size(&(builder->globals)) + builder->num_import_types[PWASM_IMPORT_TYPE_GLOBAL],
    },

    .has_start = builder->has_start,
    .start = builder->start,

  #define BUILDER_VEC(name, type, prev) \
    .name ## s = memcpy(name ## s_dst, name ## s_src, name ## s_size), \
    .num_ ## name ## s = num_ ## name ## s,
  BUILDER_VECS
  #undef BUILDER_VEC
  };

  // copy result to output
  memcpy(dst, &mod, sizeof(pwasm_mod_t));

  // resolve else/end insts for if/loop/block
  if (!pwasm_builder_resolve_jumps(builder, dst)) {
    // return failure
    return false;
  }

  // return success
  return true;
}

DEF_VEC_PARSER(type, pwasm_type_t)
DEF_VEC_PARSER(import, pwasm_import_t)
DEF_VEC_PARSER(func, uint32_t)
DEF_VEC_PARSER(table, pwasm_table_t)
DEF_VEC_PARSER(mem, pwasm_limits_t)
DEF_VEC_PARSER(global, pwasm_global_t)
DEF_VEC_PARSER(export, pwasm_export_t)
DEF_VEC_PARSER(elem, pwasm_elem_t)
DEF_VEC_PARSER(code, pwasm_func_t)
DEF_VEC_PARSER(segment, pwasm_segment_t)

static size_t
pwasm_mod_parse_custom_section(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void *cb_data
) {
  const pwasm_parse_custom_section_cbs_t cbs = {
    .on_error           = src_cbs->on_error,
    .on_bytes           = src_cbs->on_bytes,
    .on_custom_section  = src_cbs->on_custom_section,
  };

  return pwasm_parse_custom_section(src, &cbs, cb_data);
}

static size_t
pwasm_mod_parse_type(
  pwasm_type_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void *cb_data
) {
  const pwasm_parse_type_cbs_t cbs = {
    .on_u32s  = src_cbs->on_u32s,
    .on_error = src_cbs->on_error,
  };

  return pwasm_parse_type(dst, src, &cbs, cb_data);
}

/**
 * Parse import into +dst_import+ from source buffer +src+, consuming a
 * maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_mod_parse_import(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void * const cb_data
) {
  const pwasm_parse_import_cbs_t cbs = {
    .on_bytes = src_cbs->on_bytes,
    .on_error = src_cbs->on_error,
  };

  return pwasm_parse_import(dst, src, &cbs, cb_data);
}

static size_t
pwasm_mod_parse_func(
  uint32_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void * const cb_data
) {
  const size_t len = pwasm_u32_decode(dst, src);
  if (!len) {
    cbs->on_error("invalid function id", cb_data);
  }

  return len;
}

static size_t
pwasm_mod_parse_table(
  pwasm_table_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void * const cb_data
) {
  return pwasm_parse_table(dst, src, cbs->on_error, cb_data);
}

static size_t
pwasm_mod_parse_mem(
  pwasm_limits_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void * const cb_data
) {
  return pwasm_parse_limits(dst, src, cbs->on_error, cb_data);
}

static size_t
pwasm_mod_parse_global(
  pwasm_global_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void * const cb_data
) {
  D("src_cbs = %p", (void*) src_cbs);

  const pwasm_parse_expr_cbs_t cbs = {
    .on_labels  = src_cbs->on_labels,
    .on_insts   = src_cbs->on_insts,
    .on_error   = src_cbs->on_error,
  };

  return pwasm_parse_global(dst, src, &cbs, cb_data);
}

static size_t
pwasm_mod_parse_export(
  pwasm_export_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void * const cb_data
) {
  const pwasm_parse_export_cbs_t cbs = {
    .on_bytes = src_cbs->on_bytes,
    .on_error = src_cbs->on_error,
  };
  return pwasm_parse_export(dst, src, &cbs, cb_data);
}

static size_t
pwasm_mod_parse_elem(
  pwasm_elem_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void * const cb_data
) {
  const pwasm_parse_elem_cbs_t cbs = {
    .on_funcs = src_cbs->on_u32s,
    .on_insts = src_cbs->on_insts,
    .on_error = src_cbs->on_error,
  };
  return pwasm_parse_elem(dst, src, &cbs, cb_data);
}

static size_t
pwasm_mod_parse_code(
  pwasm_func_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void * const cb_data
) {
  const pwasm_parse_code_cbs_t cbs = {
    .on_labels  = src_cbs->on_u32s,
    .on_locals  = src_cbs->on_locals,
    .on_insts   = src_cbs->on_insts,
    .on_error   = src_cbs->on_error,
  };
  return pwasm_parse_code(dst, src, &cbs, cb_data);
}

static size_t
pwasm_mod_parse_segment(
  pwasm_segment_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void * const cb_data
) {
  const pwasm_parse_segment_cbs_t cbs = {
    .on_bytes   = src_cbs->on_bytes,
    .on_insts   = src_cbs->on_insts,
    .on_error   = src_cbs->on_error,
  };
  return pwasm_parse_segment(dst, src, &cbs, cb_data);
}

#define DEF_VEC_SECTION(NAME) \
  static size_t pwasm_mod_parse_ ## NAME ## _section( \
    const pwasm_buf_t src, \
    const pwasm_mod_parse_cbs_t * const cbs, \
    void *cb_data \
  ) { \
    return pwasm_mod_parse_ ## NAME ## s(src, cbs, cb_data); \
  }

DEF_VEC_SECTION(type)
DEF_VEC_SECTION(import)
DEF_VEC_SECTION(func)
DEF_VEC_SECTION(table)
DEF_VEC_SECTION(mem)
DEF_VEC_SECTION(global)
DEF_VEC_SECTION(export)
DEF_VEC_SECTION(elem)
DEF_VEC_SECTION(code)
DEF_VEC_SECTION(segment)

static size_t
pwasm_mod_parse_start_section(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  uint32_t id = 0;

  const size_t len = pwasm_u32_decode(&id, src);
  if (len > 0) {
    cbs->on_start(id, cb_data);
  }

  return len;
}

static size_t
pwasm_mod_parse_invalid_section(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  (void) src;

  cbs->on_error("invalid section", cb_data);

  // return failure
  return 0;
}

static inline size_t
pwasm_mod_parse_section(
  const pwasm_section_type_t type,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  D("type = src.ptr = %p, type = %u:%s", (void*) src.ptr, type, pwasm_section_type_get_name(type));
  switch (type) {
  #define PWASM_SECTION_TYPE(a, b) \
    case PWASM_SECTION_TYPE_ ## a: \
      return pwasm_mod_parse_ ## b ## _section(src, cbs, cb_data);
  PWASM_SECTION_TYPES
  #undef PWASM_SECTION_TYPE
  default:
    return pwasm_mod_parse_invalid_section(src, cbs, cb_data);
  }
}

size_t
pwasm_mod_parse(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void *cb_data
) {
  D("src_cbs = %p", (void*) src_cbs);

  // cache callbacks locally
  const pwasm_mod_parse_cbs_t cbs = *src_cbs;
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // check source length
  if (src.len < 8) {
    cbs.on_error("source too small", cb_data);
    return 0;
  }

  // check magic and version
  if (memcmp(curr.ptr, PWASM_HEADER, sizeof(PWASM_HEADER))) {
    cbs.on_error("invalid module header", cb_data);
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, sizeof(PWASM_HEADER));
  num_bytes += sizeof(PWASM_HEADER);

  pwasm_section_type_t max_type = 0;
  while (curr.len > 0) {
    // get section header, check for error
    pwasm_header_t head;
    const size_t head_len = pwasm_header_parse(&head, curr);
    if (!head_len) {
      cbs.on_error("invalid section header", cb_data);
      return 0;
    }

    // advance
    curr = pwasm_buf_step(curr, head_len);
    num_bytes += head_len;

    // check section type
    if (head.type >= PWASM_SECTION_TYPE_LAST) {
      cbs.on_error("invalid section type", cb_data);
      return 0;
    }

    // check section order for non-custom sections
    if (head.type != PWASM_SECTION_TYPE_CUSTOM) {
      if (head.type <= max_type) {
        const char * const text = (head.type < max_type) ? "invalid section order" : "duplicate section";
        cbs.on_error(text, cb_data);
        return 0;
      }

      // update maximum section type
      max_type = head.type;
    }

    // invoke section header callback
    cbs.on_section(&head, cb_data);

    if (head.len > 0) {
      // build body buffer
      const pwasm_buf_t body = { curr.ptr, head.len };

      // parse section, check for error
      const size_t body_len = pwasm_mod_parse_section(head.type, body, &cbs, cb_data);
      if (!body_len) {
        // return failure
        return 0;
      }

      // advance
      curr = pwasm_buf_step(curr, body_len);
      num_bytes += body_len;
    }
  }

  // return number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_builder_t * const builder;
  bool success;
} pwasm_mod_init_unsafe_t;

static void
pwasm_mod_init_unsafe_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  if (data->success) {
    pwasm_fail(data->builder->mem_ctx, text);
    data->success = false;
  }
}

static pwasm_slice_t
pwasm_mod_init_unsafe_on_u32s(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  const pwasm_slice_t ret = pwasm_builder_push_u32s(data->builder, rows, num);
  if (!ret.len) {
    pwasm_mod_init_unsafe_on_error("push u32s failed", data);
  }

  return ret;
}

static pwasm_slice_t
pwasm_mod_init_unsafe_on_bytes(
  const uint8_t * const bytes,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  D("bytes = %p, num = %zu", (void*) bytes, num);

  const pwasm_slice_t ret = pwasm_builder_push_bytes(data->builder, bytes, num);
  if (ret.len != num) {
    pwasm_mod_init_unsafe_on_error("push bytes failed", data);
  }

  return ret;
}

static void
pwasm_mod_init_unsafe_on_section(
  const pwasm_header_t * const header,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_sections(data->builder, header, 1).len) {
    pwasm_mod_init_unsafe_on_error("push sections failed", data);
  }
}

static void
pwasm_mod_init_unsafe_on_custom_section(
  const pwasm_custom_section_t * const section,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_custom_sections(data->builder, section, 1).len) {
    pwasm_mod_init_unsafe_on_error("push custom sections failed", data);
  }
}

static void
pwasm_mod_init_unsafe_on_types(
  const pwasm_type_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_types(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push types failed", data);
  }
}

static void
pwasm_mod_init_unsafe_on_imports(
  const pwasm_import_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  if (!pwasm_builder_push_imports(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push imports failed", data);
    return;
  }

  for (size_t i = 0; i < num; i++) {
    switch (rows[i].type) {
    case PWASM_IMPORT_TYPE_FUNC:
      {
        if (!pwasm_builder_push_funcs(data->builder, &(rows[i].func), 1).len) {
          pwasm_mod_init_unsafe_on_error("push func import failed", data);
          return;
        }
      }

      break;
    case PWASM_IMPORT_TYPE_TABLE:
      {
        if (!pwasm_builder_push_tables(data->builder, &(rows[i].table), 1).len) {
          pwasm_mod_init_unsafe_on_error("push table import failed", data);
          return;
        }
      }

      break;
    case PWASM_IMPORT_TYPE_MEM:
      {
        if (!pwasm_builder_push_mems(data->builder, &(rows[i].mem), 1).len) {
          pwasm_mod_init_unsafe_on_error("push mem import failed", data);
          return;
        }
      }

      break;
    case PWASM_IMPORT_TYPE_GLOBAL:
      {
        const pwasm_global_t global = {
          .type = rows[i].global,
          .expr = { 0, 0 },
        };

        if (!pwasm_builder_push_globals(data->builder, &global, 1).len) {
          pwasm_mod_init_unsafe_on_error("push global import failed", data);
          return;
        }
      }

      break;
    default:
      pwasm_mod_init_unsafe_on_error("unknown import type", data);
      return;
    }

    data->builder->num_import_types[rows[i].type]++;
  }
}

static void
pwasm_mod_init_unsafe_on_funcs(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  for (size_t i = 0; i < num; i += PWASM_BATCH_SIZE) {
    // get number of elements
    const size_t len = MIN((num - i), PWASM_BATCH_SIZE);

    // push function IDs, check for error
    if (!pwasm_builder_push_funcs(data->builder, rows + i, len).len) {
      pwasm_mod_init_unsafe_on_error("push funcs failed", data);
    }
  }
}

static void
pwasm_mod_init_unsafe_on_tables(
  const pwasm_table_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_tables(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push tables failed", data);
  }
}

static pwasm_slice_t
pwasm_mod_init_unsafe_on_insts(
  const pwasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  const pwasm_slice_t ret = pwasm_builder_push_insts(data->builder, rows, num);
  if (!ret.len) {
    pwasm_mod_init_unsafe_on_error("push insts failed", data);
  }

  // for (size_t i = 0; i < num; i++) {
  //   D("inst[%zu].op = %u", i, rows[i].op);
  // }

  return ret;
}

static void
pwasm_mod_init_unsafe_on_mems(
  const pwasm_limits_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_mems(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push mems failed", data);
  }
}

static void
pwasm_mod_init_unsafe_on_globals(
  const pwasm_global_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_globals(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push globals failed", data);
  }
}

static void
pwasm_mod_init_unsafe_on_exports(
  const pwasm_export_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_exports(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push exports failed", data);
  }

  for (size_t i = 0; i < num; i++) {
    data->builder->num_export_types[rows[i].type]++;
  }
}

static void
pwasm_mod_init_unsafe_on_start(
  const uint32_t id,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  data->builder->has_start = true;
  data->builder->start = id;
}

static pwasm_slice_t
pwasm_mod_init_unsafe_on_locals(
  const pwasm_local_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  pwasm_slice_t ret = pwasm_builder_push_locals(data->builder, rows, num);
  if (!ret.len) {
    pwasm_mod_init_unsafe_on_error("push locals failed", data);
  }

  return ret;
}

static pwasm_slice_t
pwasm_mod_init_unsafe_on_labels(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;

  pwasm_slice_t ret = pwasm_builder_push_u32s(data->builder, rows, num);
  if (!ret.len) {
    pwasm_mod_init_unsafe_on_error("push labels failed", data);
  }

  return ret;
}

static void
pwasm_mod_init_unsafe_on_codes(
  const pwasm_func_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  const size_t codes_ofs = pwasm_vec_get_size(&(data->builder->codes));

  // get types and types length
  const pwasm_type_t * const types = pwasm_vec_get_data(&(data->builder->types));
  const size_t num_types = pwasm_vec_get_size(&(data->builder->types));

  // get funcs and funcs length
  const uint32_t * const funcs = pwasm_vec_get_data(&(data->builder->funcs));
  const size_t num_funcs = pwasm_vec_get_size(&(data->builder->funcs));

  // batch of funcs, used to calculate frame_size before pushing rows
  // (see longer description below)
  pwasm_func_t tmp[PWASM_BATCH_SIZE];

  //
  // this is kind of a screwy looking loop.  here's what we're doing for
  // each code entry:
  //
  // * look up the function type from builder->funcs (checking for overlow)
  // * get parameter count from builder->types (checking for overflow)
  // * code.frame_size = type.params.len + code.max_locals
  // * build batch of funcs, then emit them in batches
  //
  // we are doing this here for the following reasons:
  // * so we have access to the frame_size in the mod_check_code()
  //   functions to check local.{get,set,tee} index immediates
  // * to slightly speed up function invocation in the interpreter by
  //   skipping the frame size calculation
  //
  for (size_t i = 0; i < num; i += LEN(tmp)) {
    const size_t num_rows = MIN(num - i, LEN(tmp));
    const size_t num_bytes = sizeof(pwasm_func_t) * num_rows;
    const size_t funcs_ofs = codes_ofs + i;
    memcpy(tmp, rows + i, num_bytes);

    // check maximum offset for this batch, check for error
    if (funcs_ofs + num_rows > num_funcs) {
      pwasm_mod_init_unsafe_on_error("push codes failed: funcs overflow", data);
    }

    // calculate frame sizes
    for (size_t j = 0; j < num_rows; j++) {
      // get type index, check for error
      const uint32_t type_id = funcs[funcs_ofs + j];
      if (type_id >= num_types) {
        pwasm_mod_init_unsafe_on_error("push codes failed: types overflow", data);
      }

      // calculate frame size (num params + num locals)
      tmp[j].frame_size = types[type_id].params.len + tmp[j].max_locals;
      tmp[j].type_id = type_id;
    }

    // flush rows, check for error
    if (!pwasm_builder_push_codes(data->builder, tmp, num_rows).len) {
      pwasm_mod_init_unsafe_on_error("push codes failed", data);
    }
  }
}

static void
pwasm_mod_init_unsafe_on_elems(
  const pwasm_elem_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_elems(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push elems failed", data);
  }
}

static void
pwasm_mod_init_unsafe_on_segments(
  const pwasm_segment_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_init_unsafe_t * const data = cb_data;
  if (!pwasm_builder_push_segments(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push segments failed", data);
  }
}

static const pwasm_mod_parse_cbs_t
PWASM_MOD_INIT_UNSAFE_PARSE_CBS = {
  .on_error           = pwasm_mod_init_unsafe_on_error,

  .on_u32s            = pwasm_mod_init_unsafe_on_u32s,
  .on_bytes           = pwasm_mod_init_unsafe_on_bytes,
  .on_insts           = pwasm_mod_init_unsafe_on_insts,

  .on_section         = pwasm_mod_init_unsafe_on_section,
  .on_custom_section  = pwasm_mod_init_unsafe_on_custom_section,
  .on_types           = pwasm_mod_init_unsafe_on_types,
  .on_imports         = pwasm_mod_init_unsafe_on_imports,
  .on_funcs           = pwasm_mod_init_unsafe_on_funcs,
  .on_tables          = pwasm_mod_init_unsafe_on_tables,
  .on_mems            = pwasm_mod_init_unsafe_on_mems,
  .on_globals         = pwasm_mod_init_unsafe_on_globals,
  .on_exports         = pwasm_mod_init_unsafe_on_exports,
  .on_start           = pwasm_mod_init_unsafe_on_start,
  .on_locals          = pwasm_mod_init_unsafe_on_locals,
  .on_labels          = pwasm_mod_init_unsafe_on_labels,
  .on_codes           = pwasm_mod_init_unsafe_on_codes,
  .on_elems           = pwasm_mod_init_unsafe_on_elems,
  .on_segments        = pwasm_mod_init_unsafe_on_segments,
};

size_t
pwasm_mod_init_unsafe(
  pwasm_mem_ctx_t * const mem_ctx,
  pwasm_mod_t * const mod,
  pwasm_buf_t src
) {
  // unconditionally zero out backing memory to prevent a segfault if
  // someone tries to pwasm_mod_fini() on a mod that isn't initialized
  // because pwasm_mod_init() fails (e.g., me)
  memset(&(mod->mem), 0, sizeof(pwasm_buf_t));

  // init builder, check for error
  pwasm_builder_t builder;
  if (!pwasm_builder_init(mem_ctx, &builder)) {
    return 0;
  }

  // build mod init context
  pwasm_mod_init_unsafe_t ctx = {
    .builder = &builder,
    .success = true,
  };

  // parse mod into builder, check for error
  const size_t len = pwasm_mod_parse(src, &PWASM_MOD_INIT_UNSAFE_PARSE_CBS, &ctx);
  if (!len) {
    return 0;
  }

  // build mod, check for error
  if (!pwasm_builder_build_mod(&builder, mod)) {
    return 0;
  }

  // finalize builder
  pwasm_builder_fini(&builder);

  // return number of bytes consumed
  return len;
}

size_t
pwasm_mod_init(
  pwasm_mem_ctx_t * const mem_ctx,
  pwasm_mod_t * const mod,
  pwasm_buf_t src
) {
  // init mod
  const size_t len = pwasm_mod_init_unsafe(mem_ctx, mod, src);
  if (!len) {
    // return failure
    return 0;
  }

  const pwasm_mod_check_cbs_t cbs = {
    // TODO: .on_warning = mem_ctx->cbs->on_error,
    .on_error = mem_ctx->cbs->on_error,
  };

  // check module
  if (!pwasm_mod_check(mod, &cbs, mem_ctx->cb_data)) {
    // return failure
    return 0;
  }

  // return success
  return len;
}

/**
 * Finalize a parsed mod and free all memory associated with it.
 */
void
pwasm_mod_fini(
  pwasm_mod_t * const mod
) {
  if (mod->mem.ptr && (mod->mem.len > 0)) {
    pwasm_realloc(mod->mem_ctx, (void*) mod->mem.ptr, 0);
    mod->mem.ptr = 0;
    mod->mem.len = 0;
  }
}

/**
 * Convert a slice in a parse module to a buffer.
 */
static pwasm_buf_t
pwasm_mod_get_buf(
  const pwasm_mod_t * const mod,
  const pwasm_slice_t slice
) {
  return (pwasm_buf_t) {
    .ptr = mod->bytes + slice.ofs,
    .len = slice.len,
  };
}

/**
 * Returns true if the given slice of bytes is valid UTF-8, and false
 * otherwise.
 */
static bool
pwasm_mod_check_is_valid_utf8(
  const pwasm_mod_t * const mod,
  const pwasm_slice_t slice
) {
  return pwasm_utf8_is_valid(pwasm_mod_get_buf(mod, slice));
}

/**
 * Get the maximum valid index for the given import type.
 */
static inline size_t
pwasm_mod_get_max_index(
  const pwasm_mod_t * const mod,
  const pwasm_import_type_t type
) {
  return mod->max_indices[MIN(type, PWASM_IMPORT_TYPE_LAST)];
}

/**
 * Returns true if the given ID is a valid function index, and false
 * otherwise.
 *
 * Note: this function accounts for imported functions too.
 */
static inline bool
pwasm_mod_is_valid_index(
  const pwasm_mod_t * const mod,
  const pwasm_import_type_t type,
  const uint32_t id
) {
  return (
    (type < PWASM_IMPORT_TYPE_LAST) &&
    (id < pwasm_mod_get_max_index(mod, type))
  );
}

/**
 * Get a pointer to the type for the given function index.
 *
 * Returns NULL when given an invalid function index.
 */
static const pwasm_type_t *
pwasm_mod_get_func_type(
  const pwasm_mod_t * const mod,
  const uint32_t func_id
) {
  const size_t num_func_imports = mod->num_import_types[PWASM_IMPORT_TYPE_FUNC];

  if (!pwasm_mod_is_valid_index(mod, PWASM_IMPORT_TYPE_FUNC, func_id)) {
    return NULL;
  }

  if (func_id < num_func_imports) {
    for (size_t i = 0, j = 0; i < mod->num_imports; i++) {
      const pwasm_import_t import = mod->imports[i];

      if (import.type == PWASM_IMPORT_TYPE_FUNC) {
        if (func_id == j) {
          return mod->types + import.func;
        }

        j++;
      }
    }

    // return failure (shouldn't happen)
    return NULL;
  } else {
    // this is safe because of the is_valid_index() call above
    return mod->types + (func_id - num_func_imports);
  }
}

/**
 * Map of code checker type to string name and result type.
 *
 * Used to convert result types to checker types and vice versa.
 */
#define PWASM_CHECKER_TYPES \
  PWASM_CHECKER_TYPE(I32, "i32", I32) \
  PWASM_CHECKER_TYPE(I64, "i64", I64) \
  PWASM_CHECKER_TYPE(F32, "f32", F32) \
  PWASM_CHECKER_TYPE(F64, "f64", F64) \
  PWASM_CHECKER_TYPE(V128, "v128", V128) \
  PWASM_CHECKER_TYPE(UNKNOWN, "unknown", LAST)

/**
 * Checker value types.
 *
 * Used as entries in code checker type stack.
 */
typedef enum {
  #define PWASM_CHECKER_TYPE(a, b, c) PWASM_CHECKER_TYPE_ ## a,
  PWASM_CHECKER_TYPES
  #undef PWASM_CHECKER_TYPE
  PWASM_CHECKER_TYPE_LAST,
} pwasm_checker_type_t;

#ifdef PWASM_DEBUG
static const char *
pwasm_checker_type_get_name(
  const pwasm_checker_type_t type
) {
  switch (type) {
  #define PWASM_CHECKER_TYPE(a, b, c) \
    case PWASM_CHECKER_TYPE_ ## a: return (b);
  PWASM_CHECKER_TYPES
  #undef PWASM_CHECKER_TYPE
  default:
    return "invalid";
  }
}
#endif /* PWASM_DEBUG */

/**
 * Convert a value type to a checker type.
 *
 * Returns PWASM_CHECKER_TYPE_LAST if the result type is invalid.
 */
static pwasm_checker_type_t
pwasm_value_type_to_checker_type(
  const pwasm_value_type_t type
) {
  switch (type) {
  #define PWASM_CHECKER_TYPE(a, b, c) \
    case PWASM_VALUE_TYPE_ ## c: \
      return PWASM_CHECKER_TYPE_ ## a;
  PWASM_CHECKER_TYPES
  #undef PWASM_CHECKER_TYPE
  default:
    return PWASM_CHECKER_TYPE_LAST;
  }
}

/**
 * Control frame.
 *
 * Used as entries in code checker control stack.
 */
typedef struct {
  // control operator
  pwasm_op_t op;

  // block type
  int32_t block_type;

  // value stack height at entry
  size_t size;

  // unreachable flag
  bool unreachable;
} pwasm_checker_ctrl_t;

/**
 * Code checker data.
 */
typedef struct {
  const pwasm_mod_t *mod; // mod
  const pwasm_mod_check_cbs_t cbs; // callbacks
  void *cb_data; // user data
  pwasm_vec_t types; // vec(pwasm_checker_type_t)
  pwasm_vec_t ctrls; // vec(pwasm_checker_ctrl_t)
} pwasm_checker_t;

/**
 * Init code validator.
 */
static bool
pwasm_checker_init(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_cbs_t * const src_cbs,
  void *cb_data
) {
  const size_t type_size = sizeof(pwasm_checker_type_t);
  const size_t ctrl_size = sizeof(pwasm_checker_ctrl_t);

  const pwasm_mod_check_cbs_t cbs = {
    .on_warning = (src_cbs && src_cbs->on_warning) ? src_cbs->on_warning : pwasm_null_on_warning,
    .on_error = (src_cbs && src_cbs->on_error) ? src_cbs->on_error : pwasm_null_on_error,
  };

  // init type stack, check for error
  pwasm_vec_t types;
  if (!pwasm_vec_init(mod->mem_ctx, &types, type_size)) {
    cbs.on_error("checker type stack init failed", cb_data);
    return false;
  }

  // init ctrl stack, check for error
  pwasm_vec_t ctrls;
  if (!pwasm_vec_init(mod->mem_ctx, &ctrls, ctrl_size)) {
    cbs.on_error("checker type stack init failed", cb_data);
    return false;
  }

  // populate result
  const pwasm_checker_t tmp = {
    .mod = mod,
    .cbs = cbs,
    .cb_data = cb_data,
    .types = types,
    .ctrls = ctrls,
  };

  // copy result to destination
  memcpy(checker, &tmp, sizeof(pwasm_checker_t));

  // return success
  return true;
}

/**
 * Finalize code checker.
 */
static void
pwasm_checker_fini(
  pwasm_checker_t * const checker
) {
  pwasm_vec_fini(&(checker->types));
  pwasm_vec_fini(&(checker->ctrls));
}

/**
 * Clear code checker stacks.
 */
static void
pwasm_checker_clear(
  pwasm_checker_t * const checker
) {
  pwasm_vec_clear(&(checker->types));
  pwasm_vec_clear(&(checker->ctrls));
}

/**
 * Log error message to checker `on_error` callback.
 */
static void
pwasm_checker_fail(
  pwasm_checker_t * const checker,
  const char * const text
) {
  checker->cbs.on_error(text, checker->cb_data);
}

static void
pwasm_checker_dump_ctrls(
  const pwasm_checker_t * const checker
) {
#ifdef PWASM_DEBUG
  const pwasm_vec_t * const vec = &(checker->ctrls);
  const pwasm_checker_ctrl_t * const rows = pwasm_vec_get_data(vec);
  const size_t num_rows = pwasm_vec_get_size(vec);

  D("checker.types.len = %zu", num_rows);
  for (size_t i = 0; i < num_rows; i++) {
    const pwasm_checker_ctrl_t ctrl = rows[i];

    // get control frame block type
    char buf[512];
    if (!pwasm_block_type_to_buf(checker->mod, ctrl.block_type, buf, sizeof(buf))) {
      memcpy(buf, "<invalid block type>", 21);
    }

    D("checker.ctrls[%zu] = { op = %s, block_type = %s, size = %zu, unreachable = %s }",
      i,
      pwasm_op_get_name(ctrl.op),
      buf,
      ctrl.size,
      ctrl.unreachable ? "true" : "false"
    );
  }
#else
  (void) checker;
#endif /* PWASM_DEBUG */
}

static void
pwasm_checker_dump_types(
  const pwasm_checker_t * const checker
) {
#ifdef PWASM_DEBUG
  const pwasm_vec_t * const vec = &(checker->types);
  const pwasm_checker_type_t * const rows = pwasm_vec_get_data(vec);
  const size_t num_rows = pwasm_vec_get_size(vec);

  D("checker.types.len = %zu", num_rows);
  for (size_t i = 0; i < num_rows; i++) {
    D("checker.types[%zu] = %s", i, pwasm_checker_type_get_name(rows[i]));
  }
#else
  (void) checker;
#endif /* PWASM_DEBUG */
}

static void
pwasm_checker_dump(
  const pwasm_checker_t * const checker
) {
  pwasm_checker_dump_ctrls(checker);
  pwasm_checker_dump_types(checker);
}

// forward references
static const pwasm_checker_ctrl_t *pwasm_checker_ctrl_peek(const pwasm_checker_t *, const size_t);

/**
 * Count number of entries in type stack.
 */
static size_t
pwasm_checker_type_get_size(
  const pwasm_checker_t * const checker
) {
  return pwasm_vec_get_size(&(checker->types));
}

/**
 * Push entry to type stack.
 */
static bool
pwasm_checker_type_push(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t type
) {
  // push entry, check for error.
  if (!pwasm_vec_push(&(checker->types), 1, &type, NULL)) {
    pwasm_checker_fail(checker, "checker type stack push failed");
    return false;
  }

  // return success
  return true;
}

/**
 * Pop entry from type stack.
 *
 * Note: This function implements the additional checks specified in the
 * WebAssembly validation section, in order to handle type stack
 * underflow and a polymorphic stack.
 */
static bool
pwasm_checker_type_pop(
  pwasm_checker_t * const checker,
  pwasm_checker_type_t * const ret_type
) {
  const size_t types_size = pwasm_checker_type_get_size(checker);

  // get control frame, check for error
  const pwasm_checker_ctrl_t *tmp = pwasm_checker_ctrl_peek(checker, 0);
  const pwasm_checker_ctrl_t ctrl = tmp ? *tmp : (pwasm_checker_ctrl_t) {
    .size = 0,
  };

  // check type stack against control frame height
  if (types_size > ctrl.size) {
    // pop entry, check for error
    if (!pwasm_vec_pop(&(checker->types), ret_type)) {
      pwasm_checker_fail(checker, "checker type stack pop failed");
      return false;
    }
  } else if (ctrl.unreachable) {
    if (ret_type) {
      // return unknown
      *ret_type = PWASM_CHECKER_TYPE_UNKNOWN;
    }
  } else {
    // log error, return failure
    D("types_size = %zu, ctrl.size = %zu", types_size, ctrl.size);
    pwasm_checker_fail(checker, "checker type stack underflow");
    return false;
  }

  // return success
  return true;
}

/**
 * Pop entry from type stack with an expected type.
 *
 * Returns the type popped in `ret_type`, which may be
 * `PWASM_CHECKER_TYPE_UNKNOWN` rather than the expected type in certain
 * situations.
 */
static bool
pwasm_checker_type_pop_expected(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t exp_type,
  pwasm_checker_type_t * const ret_type
) {
  // pop actual type, check for error
  pwasm_checker_type_t got_type;
  if (!pwasm_checker_type_pop(checker, &got_type)) {
    return false;
  }

  if (got_type == PWASM_CHECKER_TYPE_UNKNOWN) {
    // return expected type
    if (ret_type) {
      *ret_type = exp_type;
    }
  } else if (exp_type == PWASM_CHECKER_TYPE_UNKNOWN) {
    // return actual type
    if (ret_type) {
      *ret_type = got_type;
    }
  } else if (got_type != exp_type) {
    D("type stack pop: type mismatch: got %s (%u), expected %s (%u)",
      pwasm_checker_type_get_name(got_type), got_type,
      pwasm_checker_type_get_name(exp_type), exp_type
    );

    // log error, return failure
    pwasm_checker_fail(checker, "type stack pop: type mismatch");
    return false;
  } else {
    // return actual type
    if (ret_type) {
      *ret_type = got_type;
    }
  }

  // return success
  return true;
}

/**
 * Shrink type stack to desired size.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_type_shrink(
  pwasm_checker_t * const checker,
  const size_t new_size
) {
  const size_t old_size = pwasm_vec_get_size(&(checker->types));
  (void) old_size;

  if (!pwasm_vec_shrink(&(checker->types), new_size)) {
    D("sizes: old = %zu, new = %zu", old_size, new_size);
    pwasm_checker_fail(checker, "shrink type stack failed");
    return false;
  }

  // return success
  return true;
}

static bool
pwasm_checker_type_check_params(
  pwasm_checker_t * const checker,
  const int32_t block_type
) {
  // get block.params.size, check for error
  size_t num_params;
  if (!pwasm_block_type_params_get_size(checker->mod, block_type, &num_params)) {
    pwasm_checker_fail(checker, "get block type parameter count failed");
    return false;
  }

  // pop expected parameters in reverse order
  for (size_t i = 0; i < num_params; i++) {
    // calc offset
    const size_t ofs = (num_params - 1 - i);

    // get value type
    pwasm_value_type_t val_type;
    if (!pwasm_block_type_params_get_nth(checker->mod, block_type, ofs, &val_type)) {
      pwasm_checker_fail(checker, "pop: get Nth block type param failed");
      return false;
    }

    // convert value type to checker type
    const pwasm_checker_type_t exp_type = pwasm_value_type_to_checker_type(val_type);

    // pop result, check for error
    pwasm_checker_type_t got_type;
    if (!pwasm_checker_type_pop_expected(checker, exp_type, &got_type)) {
      return false;
    }
  }

  // push expected parameters back to type stack
  for (size_t i = 0; i < num_params; i++) {
    // get value type
    pwasm_value_type_t val_type;
    if (!pwasm_block_type_params_get_nth(checker->mod, block_type, i, &val_type)) {
      pwasm_checker_fail(checker, "push: get Nth block type param failed");
      return false;
    }

    // convert value type to checker type
    const pwasm_checker_type_t exp_type = pwasm_value_type_to_checker_type(val_type);

    // push type to type stack
    if (!pwasm_checker_type_push(checker, exp_type)) {
      pwasm_checker_fail(checker, "push: push block type param failed");
      return false;
    }
  }

  // return success
  return true;
}

/**
 * Push entry to control stack.
 */
static bool
pwasm_checker_ctrl_push(
  pwasm_checker_t * const checker,
  const pwasm_checker_ctrl_t ctrl
) {
  // push entry, check for error.
  if (!pwasm_vec_push(&(checker->ctrls), 1, &ctrl, NULL)) {
    pwasm_checker_fail(checker, "checker control stack push failed");
    return false;
  }

  // return success
  return true;
}

/**
 * Pop entry from control stack.
 */
static bool
pwasm_checker_ctrl_pop(
  pwasm_checker_t * const checker,
  pwasm_checker_ctrl_t * const ret_ctrl
) {
  // peek at top control frame, check for error
  const pwasm_checker_ctrl_t * const ctrl = pwasm_checker_ctrl_peek(checker, 0);
  if (!ctrl) {
    pwasm_checker_fail(checker, "empty checker control stack");
    return false;
  }

  D("types.size = %zu", pwasm_checker_type_get_size(checker));

  // get number of results
  size_t num_results;
  if (!pwasm_block_type_results_get_size(checker->mod, ctrl->block_type, &num_results)) {
    pwasm_checker_fail(checker, "checker: couldn't get block type result size");
    return false;
  }

  for (size_t i = 0; i < num_results; i++) {
    // get offset
    const size_t ofs = num_results - 1 - i;

    // get value type, check for error
    pwasm_value_type_t val_type;
    if (!pwasm_block_type_results_get_nth(checker->mod, ctrl->block_type, ofs, &val_type)) {
      pwasm_checker_fail(checker, "checker: couldn't get block type result");
      return false;
    }

    // convert value type to checker type
    const pwasm_checker_type_t exp_type = pwasm_value_type_to_checker_type(val_type);

    // pop result, check for error
    pwasm_checker_type_t got_type;
    if (!pwasm_checker_type_pop_expected(checker, exp_type, &got_type)) {
      return false;
    }
  }

  // check type stack size
  if (pwasm_checker_type_get_size(checker) != ctrl->size) {
    D("sizes: types = %zu, ctrl->size = %zu", pwasm_checker_type_get_size(checker), ctrl->size);
    pwasm_checker_fail(checker, "incorrect type stack height");
    return false;
  }

  // pop control frame, return result
  return pwasm_vec_pop(&(checker->ctrls), ret_ctrl);
}

/**
 * Count number of entries in control stack.
 */
static size_t
pwasm_checker_ctrl_get_size(
  const pwasm_checker_t * const checker
) {
  return pwasm_vec_get_size(&(checker->ctrls));
}

/**
 * Return pointer to last control stack entry, or NULL if control stack
 * is empty.
 */
static const pwasm_checker_ctrl_t *
pwasm_checker_ctrl_peek(
  const pwasm_checker_t * const checker,
  const size_t ofs
) {
  return pwasm_vec_peek_tail(&(checker->ctrls), ofs);
}

static bool
pwasm_checker_ctrl_mark_unreachable(
  pwasm_checker_t * const checker
) {
  // pop control frame, check for error
  pwasm_checker_ctrl_t *ctrl = (pwasm_checker_ctrl_t*) pwasm_checker_ctrl_peek(checker, 0);
  if (!ctrl) {
    pwasm_checker_fail(checker, "no block to mark as unreachable");
    return false;
  }

  // shrink type stack, check for error
  if (!pwasm_checker_type_shrink(checker, ctrl->size)) {
    return false;
  }

  // mark frame as unreachable
  ctrl->unreachable = true;

  // return success
  return true;
}

/**
 * Get type of local variable.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_get_local_type(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const pwasm_func_t func,
  const uint32_t id,
  pwasm_checker_type_t * const ret_type
) {
  // check local index
  if (id >= func.frame_size) {
    pwasm_checker_fail(checker, "local index out of bounds");
    return false;
  }

  D("func.type_id = %lu", func.type_id);

  for (size_t i = 0; i < mod->num_funcs; i++) {
    D("mod->funcs[%zu] = %u", i, mod->funcs[i]);
  }

  const pwasm_type_t func_type = mod->types[func.type_id];
  D("id = %u", id);

  pwasm_value_type_t val_type = PWASM_VALUE_TYPE_LAST;
  if (id < func_type.params.len) {
    #ifdef PWASM_DEBUG
    for (size_t i = 0; i < func_type.params.len; i++) {
      const uint32_t val = mod->u32s[func_type.params.ofs + i];
      D("params[%zu] = %s (0x%02x)", i, pwasm_value_type_get_name(val), val);
    }
    #endif /* PWASM_DEBUG */

    // get parameter type
    val_type = mod->u32s[func_type.params.ofs + id];
    D("val_type = %s (%u)", pwasm_value_type_get_name(val_type), val_type);
  } else {
    const pwasm_local_t * const locals = mod->locals + func.locals.ofs;
    const uint32_t local_id = id - func_type.params.len;

    // walk locals, find match
    size_t sum = 0;
    for (size_t i = 0; i < func.locals.len; i++) {
      if ((local_id >= sum) && (local_id < sum + locals[i].num)) {
        // found match, save type
        val_type = locals[i].type;
      }

      // increment sum
      sum += locals[i].num;
    }
  }

  // check value type
  if (val_type == PWASM_VALUE_TYPE_LAST) {
    // should never be reached
    pwasm_checker_fail(checker, "invalid local value type");
    return false;
  }

  // convert value type to checker type
  const pwasm_checker_type_t type = pwasm_value_type_to_checker_type(val_type);
  if (ret_type) {
    *ret_type = type;
  }

  // return success
  return true;
}

/**
 * Get type of global.
 *
 * Returns `true` on success or `false` on error.
 *
 * @TODO use pwasm_mod_get_global_type()
 */
static bool
pwasm_checker_get_global_type(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const uint32_t id,
  pwasm_checker_type_t * const ret_type,
  bool * const ret_mut
) {
  // check global index
  if (id >= mod->max_indices[PWASM_IMPORT_TYPE_GLOBAL]) {
    pwasm_checker_fail(checker, "global index out of bound");
    return false;
  }

  const pwasm_global_type_t *global_type = NULL;
  if (id < mod->num_import_types[PWASM_IMPORT_TYPE_GLOBAL]) {
    // get imported global type
    for (size_t i = 0, curr_global_id = 0; i < mod->num_imports; i++) {
      if (mod->imports[i].type == PWASM_IMPORT_TYPE_GLOBAL) {
        if (id == curr_global_id) {
          global_type = &(mod->imports[i].global);
        }
        curr_global_id++;
      }
    }
  } else {
    // get internal module global type
    const uint32_t global_id = id - mod->num_import_types[PWASM_IMPORT_TYPE_GLOBAL];
    global_type = &(mod->globals[global_id].type);
  }

  // check value type
  if (!global_type) {
    // should never be reached
    pwasm_checker_fail(checker, "invalid global value type");
    return false;
  }

  // convert value type to checker type
  const pwasm_checker_type_t type = pwasm_value_type_to_checker_type(global_type->type);
  if (ret_type) {
    *ret_type = type;
  }

  if (ret_mut) {
    // copy mutable to destination
    *ret_mut = global_type->mutable;
  }

  // return success
  return true;
}

static pwasm_checker_type_t
pwasm_checker_check_const_get_type(
  const pwasm_op_t op
) {
  switch (op) {
  case PWASM_OP_I32_CONST:
    return PWASM_CHECKER_TYPE_I32;
  case PWASM_OP_I64_CONST:
    return PWASM_CHECKER_TYPE_I64;
  case PWASM_OP_F32_CONST:
    return PWASM_CHECKER_TYPE_F32;
  case PWASM_OP_F64_CONST:
    return PWASM_CHECKER_TYPE_F64;
  case PWASM_OP_V128_CONST:
    return PWASM_CHECKER_TYPE_V128;
  default:
    return PWASM_CHECKER_TYPE_LAST;
  }
}

static bool
pwasm_checker_check_const(
  pwasm_checker_t * const checker,
  const pwasm_op_t op
) {
  const pwasm_checker_type_t type = pwasm_checker_check_const_get_type(op);
  return pwasm_checker_type_push(checker, type);
}

/**
 * Verify that at least one memory is attached to this module.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_mem(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod
) {
  // check memory
  if (!mod->max_indices[PWASM_IMPORT_TYPE_MEM]) {
    pwasm_checker_fail(checker, "invalid memory op: no memory attached");
    return false;
  }

  // return success
  return true;
}

/**
 * Verify values in memory immediate.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_mem_imm(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const pwasm_inst_t in
) {
  // check memory
  if (!pwasm_checker_check_mem(checker, mod)) {
    return false;
  }

  // check alignment immediate
  if (in.v_mem.align > 31) {
    pwasm_checker_fail(checker, "memory alignment too large");
    return false;
  }

  // get number of bits for instruction
  const uint32_t num_bytes = pwasm_op_get_num_bytes(in.op);

  // check alignment
  // reference:
  // https://webassembly.github.io/spec/core/valid/instructions.html#memory-instructions
  if ((1U << in.v_mem.align) > (num_bytes)) {
    pwasm_checker_fail(checker, "invalid memory alignment");
    return false;
  }

  // return succes
  return true;
}

static pwasm_checker_type_t
pwasm_checker_check_mem_get_type(
  const pwasm_op_t op
) {
  switch (op) {
  case PWASM_OP_I32_LOAD:
  case PWASM_OP_I32_LOAD8_S:
  case PWASM_OP_I32_LOAD8_U:
  case PWASM_OP_I32_LOAD16_S:
  case PWASM_OP_I32_LOAD16_U:
  case PWASM_OP_I32_STORE:
  case PWASM_OP_I32_STORE8:
  case PWASM_OP_I32_STORE16:
    return PWASM_CHECKER_TYPE_I32;
  case PWASM_OP_I64_LOAD:
  case PWASM_OP_I64_LOAD8_S:
  case PWASM_OP_I64_LOAD8_U:
  case PWASM_OP_I64_LOAD16_S:
  case PWASM_OP_I64_LOAD16_U:
  case PWASM_OP_I64_LOAD32_S:
  case PWASM_OP_I64_LOAD32_U:
  case PWASM_OP_I64_STORE:
  case PWASM_OP_I64_STORE8:
  case PWASM_OP_I64_STORE16:
  case PWASM_OP_I64_STORE32:
    return PWASM_CHECKER_TYPE_I64;
  case PWASM_OP_F32_LOAD:
  case PWASM_OP_F32_STORE:
    return PWASM_CHECKER_TYPE_F32;
  case PWASM_OP_F64_LOAD:
  case PWASM_OP_F64_STORE:
    return PWASM_CHECKER_TYPE_F64;
  case PWASM_OP_V128_LOAD:
  case PWASM_OP_V128_STORE:
  case PWASM_OP_I16X8_LOAD8X8_S:
  case PWASM_OP_I16X8_LOAD8X8_U:
  case PWASM_OP_I32X4_LOAD16X4_S:
  case PWASM_OP_I32X4_LOAD16X4_U:
  case PWASM_OP_I64X2_LOAD32X2_S:
  case PWASM_OP_I64X2_LOAD32X2_U:
  case PWASM_OP_V8X16_LOAD_SPLAT:
  case PWASM_OP_V16X8_LOAD_SPLAT:
  case PWASM_OP_V32X4_LOAD_SPLAT:
  case PWASM_OP_V64X2_LOAD_SPLAT:
    return PWASM_CHECKER_TYPE_V128;
  default:
    return PWASM_CHECKER_TYPE_UNKNOWN;
  }
}

/**
 * Verify memory load op.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_load(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const pwasm_inst_t in
) {
  // check memory and immediate
  if (!pwasm_checker_check_mem_imm(checker, mod, in)) {
    return false;
  }

  // pop offset operand
  if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
    return false;
  }

  // push type
  const pwasm_checker_type_t type = pwasm_checker_check_mem_get_type(in.op);
  if (!pwasm_checker_type_push(checker, type)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify memory store op.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_store(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const pwasm_inst_t in
) {
  // check memory and immediate
  if (!pwasm_checker_check_mem_imm(checker, mod, in)) {
    return false;
  }

  // get expected type
  const pwasm_checker_type_t exp_type = pwasm_checker_check_mem_get_type(in.op);

  // pop value operand
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // pop offset operand
  if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Check branch instruction.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_branch(
  pwasm_checker_t * const checker,
  const uint32_t id
) {
  // check branch target
  if (id > pwasm_checker_ctrl_get_size(checker)) {
    pwasm_checker_fail(checker, "label out of bounds");
    return false;
  }

  // get target control frame, check for error
  const pwasm_checker_ctrl_t *ctrl = pwasm_checker_ctrl_peek(checker, id);
  if (!ctrl) {
    pwasm_checker_fail(checker, "null control entry");
    return false;
  }

  if (ctrl->op != PWASM_OP_LOOP) {
    // get number of results
    size_t num_results;
    if (!pwasm_block_type_results_get_size(checker->mod, ctrl->block_type, &num_results)) {
      pwasm_checker_fail(checker, "checker: couldn't get block type result size");
      return false;
    }

    #ifdef PWASM_DEBUG
    {
      char buf[512];
      if (!pwasm_block_type_to_buf(checker->mod, ctrl->block_type, buf, sizeof(buf))) {
        pwasm_checker_fail(checker, "checker: block_type_to_buf failed");
        return false;
      }
      D("block_type = %s", buf);
    }
    #endif /* PWASM_DEBUG */

    // pop results from stack
    for (size_t i = 0; i < num_results; i++) {
      // get offset
      const size_t ofs = num_results - 1 - i;

      // get value type, check for error
      pwasm_value_type_t val_type;
      if (!pwasm_block_type_results_get_nth(checker->mod, ctrl->block_type, ofs, &val_type)) {
        pwasm_checker_fail(checker, "checker: couldn't get block type result");
        return false;
      }

      // convert value type to checker type
      const pwasm_checker_type_t exp_type = pwasm_value_type_to_checker_type(val_type);

      // pop expected value type from type stack, check for error
      pwasm_checker_type_t got_type;
      if (!pwasm_checker_type_pop_expected(checker, exp_type, &got_type)) {
        return false;
      }
    }
  }

  // return success
  return true;
}

/**
 * Check call.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_call(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const uint32_t type_id
) {
  // get function type, params, and results
  const pwasm_type_t func_type = mod->types[type_id];
  const uint32_t * const params = mod->u32s + func_type.params.ofs;
  const uint32_t * const results = mod->u32s + func_type.results.ofs;

  // pop/check function parameter types in reverse order
  for (size_t i = 0; i < func_type.params.len; i++) {
    const pwasm_value_type_t tmp = params[func_type.params.len - 1 - i];
    const pwasm_checker_type_t type = pwasm_value_type_to_checker_type(tmp);

    if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
      return false;
    }
  }

  // push function results to type stack
  for (size_t i = 0; i < func_type.results.len; i++) {
    const pwasm_value_type_t tmp = results[i];
    const pwasm_checker_type_t type = pwasm_value_type_to_checker_type(tmp);

    if (!pwasm_checker_type_push(checker, type)) {
      return false;
    }
  }

  // return success
  return true;
}

/**
 * Verify lane index immediate.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_lane(
  pwasm_checker_t * const checker,
  const pwasm_inst_t in
) {
  switch (in.op) {
  case PWASM_OP_V8X16_SHUFFLE:
    // check lanes
    for (size_t i = 0; i < 16; i++) {
      if (in.v_v128.i8[i] > 31) {
        // log error, return failure
        D("lane %zu = %u", i, in.v_v128.i8[i]);
        pwasm_checker_fail(checker, "v8x16.shuffle: invalid lane index (>31)");
        return false;
      }
    }

    // return success
    return true;
  case PWASM_OP_I8X16_EXTRACT_LANE_S:
  case PWASM_OP_I8X16_EXTRACT_LANE_U:
  case PWASM_OP_I16X8_EXTRACT_LANE_S:
  case PWASM_OP_I16X8_EXTRACT_LANE_U:
  case PWASM_OP_I32X4_EXTRACT_LANE:
  case PWASM_OP_I64X2_EXTRACT_LANE:
  case PWASM_OP_F32X4_EXTRACT_LANE:
  case PWASM_OP_F64X2_EXTRACT_LANE:
  case PWASM_OP_I8X16_REPLACE_LANE:
  case PWASM_OP_I16X8_REPLACE_LANE:
  case PWASM_OP_I32X4_REPLACE_LANE:
  case PWASM_OP_I64X2_REPLACE_LANE:
  case PWASM_OP_F32X4_REPLACE_LANE:
  case PWASM_OP_F64X2_REPLACE_LANE:
    if (in.v_index >= PWASM_OPS[in.op].num_lanes) {
      // log error, return failure
      D("lane index = %u", in.v_index);
      pwasm_checker_fail(checker, "invalid lane index");
      return false;
    }

    // return success
    return true;
  default:
    // log error, return failure
    D("opcode = %s(%u)", pwasm_op_get_name(in.op), in.op);
    pwasm_checker_fail(checker, "unknown opcode");
    return false;
  }
}

/**
 * Verify unary operation (`unop`).
 *
 * A `unop` takes a value of the given type and returns a value of the
 * given type.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_unop(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t exp_type
) {
  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // push result
  if (!pwasm_checker_type_push(checker, exp_type)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify test operation (`testop`).
 *
 * A `testop` takes a value of the given type and returns an `i32`.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_testop(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t exp_type
) {
  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // push result
  if (!pwasm_checker_type_push(checker, PWASM_CHECKER_TYPE_I32)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify relative operation (`relop`).
 *
 * A `relop` takes two values of the given type and returns an `i32`.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_relop(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t exp_type
) {
  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // push result
  if (!pwasm_checker_type_push(checker, PWASM_CHECKER_TYPE_I32)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify binary operation (`binop`).
 *
 * A `binop` takes two values of the given type and returns a value of
 * the given type.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_binop(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t exp_type
) {
  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // push result
  if (!pwasm_checker_type_push(checker, exp_type)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify trinary operation (`triop`).
 *
 * A `triop` takes three values of the given type and returns a value of
 * the given type.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_triop(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t exp_type
) {
  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // pop operand type
  if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
    return false;
  }

  // push result
  if (!pwasm_checker_type_push(checker, exp_type)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify convert operation (`cvtop`).
 *
 * A `cvtop` takes two types, it pops the source type and then pushes
 * the destination type.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_cvtop(
  pwasm_checker_t * const checker,
  const pwasm_checker_type_t dst_type,
  const pwasm_checker_type_t src_type
) {
  // pop src type
  if (!pwasm_checker_type_pop_expected(checker, src_type, NULL)) {
    return false;
  }

  // push dst type
  if (!pwasm_checker_type_push(checker, dst_type)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Get source value type for splat op.
 *
 * Returns `PWASM_CHECKER_TYPE_LAST` if the opcode not a v128 splat
 * opcode.
 */
static pwasm_checker_type_t
pwasm_checker_check_splat_get_type(
  const pwasm_op_t op
) {
  switch (op) {
  case PWASM_OP_I8X16_SPLAT:
  case PWASM_OP_I16X8_SPLAT:
  case PWASM_OP_I32X4_SPLAT:
    return PWASM_CHECKER_TYPE_I32;
  case PWASM_OP_I64X2_SPLAT:
    return PWASM_CHECKER_TYPE_I64;
  case PWASM_OP_F32X4_SPLAT:
    return PWASM_CHECKER_TYPE_F32;
  case PWASM_OP_F64X2_SPLAT:
    return PWASM_CHECKER_TYPE_F64;
  default:
    // return failure
    return PWASM_CHECKER_TYPE_LAST;
  }
}

/**
 * Verify splat operation.
 *
 * A splat pops a scalar value and then pushes a v128 with all lanes
 * populated with the source scalar value.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_splat(
  pwasm_checker_t * const checker,
  const pwasm_op_t op
) {
  // get value type
  const pwasm_checker_type_t type = pwasm_checker_check_splat_get_type(op);

  // pop value type, check for error
  if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
    return false;
  }

  // push v128
  if (!pwasm_checker_type_push(checker, PWASM_CHECKER_TYPE_V128)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Get destination value type for extract op.
 *
 * Returns `PWASM_CHECKER_TYPE_LAST` if the opcode not a v128 extract
 * opcode.
 */
static pwasm_checker_type_t
pwasm_checker_check_extract_get_type(
  const pwasm_op_t op
) {
  switch (op) {
  case PWASM_OP_I8X16_EXTRACT_LANE_S:
  case PWASM_OP_I8X16_EXTRACT_LANE_U:
  case PWASM_OP_I16X8_EXTRACT_LANE_S:
  case PWASM_OP_I16X8_EXTRACT_LANE_U:
  case PWASM_OP_I32X4_EXTRACT_LANE:
    return PWASM_CHECKER_TYPE_I32;
  case PWASM_OP_I64X2_EXTRACT_LANE:
    return PWASM_CHECKER_TYPE_I64;
  case PWASM_OP_F32X4_EXTRACT_LANE:
    return PWASM_CHECKER_TYPE_F32;
  case PWASM_OP_F64X2_EXTRACT_LANE:
    return PWASM_CHECKER_TYPE_F64;
  default:
    // return failure
    return PWASM_CHECKER_TYPE_LAST;
  }
}

/**
 * Verify v128 extract operation.
 *
 * An extract pops a vector value, extracts a lane from the vector based
 * on the lane index immediate, and then pushes the extracted scalar
 * value to the stack.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_extract(
  pwasm_checker_t * const checker,
  const pwasm_inst_t in
) {
  // get value type
  const pwasm_checker_type_t type = pwasm_checker_check_extract_get_type(in.op);

  // check lane
  if (!pwasm_checker_check_lane(checker, in)) {
    // return failure
    return false;
  }

  // pop v128 type, check for error
  if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_V128, NULL)) {
    return false;
  }

  // push value type, check for error
  if (!pwasm_checker_type_push(checker, type)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Get destination value type for replace lane op.
 *
 * Returns `PWASM_CHECKER_TYPE_LAST` if the opcode not a v128 replace
 * lane opcode.
 */
static pwasm_checker_type_t
pwasm_checker_check_replace_get_type(
  const pwasm_op_t op
) {
  switch (op) {
  case PWASM_OP_I8X16_REPLACE_LANE:
  case PWASM_OP_I16X8_REPLACE_LANE:
  case PWASM_OP_I32X4_REPLACE_LANE:
    return PWASM_CHECKER_TYPE_I32;
  case PWASM_OP_I64X2_REPLACE_LANE:
    return PWASM_CHECKER_TYPE_I64;
  case PWASM_OP_F32X4_REPLACE_LANE:
    return PWASM_CHECKER_TYPE_F32;
  case PWASM_OP_F64X2_REPLACE_LANE:
    return PWASM_CHECKER_TYPE_F64;
  default:
    // return failure
    return PWASM_CHECKER_TYPE_LAST;
  }
}

/**
 * Verify v128 replace operation.
 *
 * Pops a vector value and a scalar value, replace a lane in the vector
 * and then pushes the updated vector.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_replace(
  pwasm_checker_t * const checker,
  const pwasm_inst_t in
) {
  // get value type
  const pwasm_checker_type_t type = pwasm_checker_check_replace_get_type(in.op);

  // check lane
  if (!pwasm_checker_check_lane(checker, in)) {
    // return failure
    return false;
  }

  // pop type, check for error
  if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
    return false;
  }

  // pop v128 type, check for error
  if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_V128, NULL)) {
    return false;
  }

  // push v128 type, check for error
  if (!pwasm_checker_type_push(checker, PWASM_CHECKER_TYPE_V128)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify v128 shift operation.
 *
 * Pops a vector value and an i32 scalar value, shift each lane in the
 * vector value by the scalar value, then pushes the updated vector.
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_checker_check_shift(
  pwasm_checker_t * const checker
) {
  // pop i32 type, check for error
  if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
    return false;
  }

  // pop v128 type, check for error
  if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_V128, NULL)) {
    return false;
  }

  // push v128 type, check for error
  if (!pwasm_checker_type_push(checker, PWASM_CHECKER_TYPE_V128)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Check function code.
 *
 * Returns `true` on success, or `false` on error.
 */
static bool
pwasm_checker_check(
  pwasm_checker_t * const checker,
  const pwasm_mod_t * const mod,
  const pwasm_func_t func
) {
  // temporarily disabled
  // return true;

  // get function instructions
  const pwasm_inst_t * const insts = mod->insts + func.expr.ofs;

  if (!func.expr.len) {
    pwasm_checker_fail(checker, "empty expression");
    return false;
  } else if (insts[func.expr.len - 1].op != PWASM_OP_END) {
    pwasm_checker_fail(checker, "unterminated expression");
    return false;
  }

  // clear checker
  pwasm_checker_clear(checker);

  {
    // build control frame for outer block
    const pwasm_checker_ctrl_t ctrl = {
      .op   = PWASM_OP_BLOCK,
      .block_type = func.type_id,
      .size = pwasm_checker_type_get_size(checker),
    };

    // push control frame, check for error
    if (!pwasm_checker_ctrl_push(checker, ctrl)) {
      // return failure
      D("%s: ctrl_push failed", "func");
      return false;
    }
  }

  for (size_t i = 0; i < func.expr.len; i++) {
    // get instruction and index immediate
    const pwasm_inst_t in = insts[i];
    const uint32_t id = in.v_index;
    D("in.op = %s", pwasm_op_get_name(in.op));
    pwasm_checker_dump(checker);

    switch (in.op) {
    case PWASM_OP_UNREACHABLE:
      // mark control frame as unreachable
      if (!pwasm_checker_ctrl_mark_unreachable(checker)) {
        // return failure
        D("%s: mark_unreachable failed", pwasm_op_get_name(in.op));
        return false;
      }

      break;
    case PWASM_OP_BLOCK:
    case PWASM_OP_LOOP:
      {
        // get block type
        const int32_t block_type = in.v_block.block_type;

        // check params
        if (!pwasm_checker_type_check_params(checker, block_type)) {
          // return failure
          return false;
        }

        // get block.params.size
        size_t num_params;
        if (!pwasm_block_type_params_get_size(checker->mod, block_type, &num_params)) {
          // log error, return failure
          pwasm_checker_fail(checker, "get block params");
          return false;
        }

        // build control frame
        const pwasm_checker_ctrl_t ctrl = {
          .op   = in.op,
          .block_type = in.v_block.block_type,
          .size = pwasm_checker_type_get_size(checker) - num_params,
        };

        // push control frame, check for error
        if (!pwasm_checker_ctrl_push(checker, ctrl)) {
          // return failure
          D("%s: ctrl_push failed", pwasm_op_get_name(in.op));
          return false;
        }
      }

      break;
    case PWASM_OP_IF:
      {
        // pop type stack, check for error
        if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
          // return failure
          D("%s: pop_expected failed", pwasm_op_get_name(in.op));
          return false;
        }

        // get block type
        const int32_t block_type = in.v_block.block_type;

        // check params
        if (!pwasm_checker_type_check_params(checker, block_type)) {
          // return failure
          return false;
        }

        // get block.params.size
        size_t num_params;
        if (!pwasm_block_type_params_get_size(checker->mod, block_type, &num_params)) {
          // log error, return failure
          pwasm_checker_fail(checker, "get block params");
          return false;
        }

        // build control frame
        const pwasm_checker_ctrl_t ctrl = {
          .op   = in.op,
          .block_type = in.v_block.block_type,
          .size = pwasm_checker_type_get_size(checker) - num_params,
        };

        // push control frame, check for error
        if (!pwasm_checker_ctrl_push(checker, ctrl)) {
          // return failure
          D("%s: ctrl_push failed", pwasm_op_get_name(in.op));
          return false;
        }
      }

      break;
    case PWASM_OP_ELSE:
      {
        // pop top of control stack, check for error
        pwasm_checker_ctrl_t ctrl;
        if (!pwasm_checker_ctrl_pop(checker, &ctrl)) {
          // return failure
          D("%s: ctrl_pop failed", pwasm_op_get_name(in.op));
          return false;
        }

        // check for "if" op
        if (ctrl.op != PWASM_OP_IF) {
          // return failure
          D("%s: op check failed", pwasm_op_get_name(in.op));
          pwasm_checker_fail(checker, "else: missing if");
          return false;
        }

        // update control frame
        ctrl.op = PWASM_OP_ELSE;

        // push control frame, check for error
        if (!pwasm_checker_ctrl_push(checker, ctrl)) {
          // return failure
          D("%s: ctrl_push failed", pwasm_op_get_name(in.op));
          return false;
        }
      }

      break;
    case PWASM_OP_END:
      {
        // pop top of control stack, check for error
        pwasm_checker_ctrl_t ctrl;
        if (!pwasm_checker_ctrl_pop(checker, &ctrl)) {
          // return failure
          D("%s: ctrl_pop failed", pwasm_op_get_name(in.op));
          return false;
        }

        // get number of results
        size_t num_results;
        if (!pwasm_block_type_results_get_size(checker->mod, ctrl.block_type, &num_results)) {
          pwasm_checker_fail(checker, "checker: end: couldn't get block type result size");
          return false;
        }

        // push result types to type stack
        for (size_t j = 0; j < num_results; j++) {
          // get value type, check for error
          pwasm_value_type_t val_type;
          if (!pwasm_block_type_results_get_nth(checker->mod, ctrl.block_type, j, &val_type)) {
            pwasm_checker_fail(checker, "checker: end: couldn't get block type result");
            return false;
          }

          // convert value type to checker type
          const pwasm_checker_type_t type = pwasm_value_type_to_checker_type(val_type);

          // push type, check for error
          if (!pwasm_checker_type_push(checker, type)) {
            // return failure
            D("%s: type_push failed", pwasm_op_get_name(in.op));
            return false;
          }
        }
      }

      break;
    case PWASM_OP_BR:
      // check branch
      if (!pwasm_checker_check_branch(checker, id)) {
        return false;
      }

      // mark control frame as unreachable
      if (!pwasm_checker_ctrl_mark_unreachable(checker)) {
        D("%s: mark_unreachable failed", pwasm_op_get_name(in.op));
        return false;
      }

      break;
    case PWASM_OP_BR_IF:
      // pop i32 from type stack, check for error
      if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
        return false;
      }

      // check branch
      if (!pwasm_checker_check_branch(checker, id)) {
        return false;
      }

      break;
    case PWASM_OP_BR_TABLE:
      {
        const uint32_t max_label = pwasm_checker_ctrl_get_size(checker);
        const pwasm_slice_t slice = in.v_br_table;
        const uint32_t * const labels = mod->u32s + slice.ofs;
        const uint32_t num_labels = slice.len;
        const uint32_t last_label = labels[num_labels - 1];

        // pop i32 from type stack, check for error
        if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
          D("%s: type_pop_expected failed (i32)", pwasm_op_get_name(in.op));
          return false;
        }

        // check default branch target
        if (last_label >= max_label) {
          pwasm_checker_fail(checker, "br_table: default label out of bounds");
          return false;
        }

        // get control frame for default branch target, check for error
        const pwasm_checker_ctrl_t * const last_ctrl = pwasm_checker_ctrl_peek(checker, last_label);
        if (!last_ctrl) {
          // should never happen
          pwasm_checker_fail(checker, "br_table: null control frame for default label");
          return false;
        }

        const int32_t last_block_type = (last_ctrl->op == PWASM_OP_LOOP) ? PWASM_BLOCK_TYPE_VOID : last_ctrl->block_type;

        for (uint32_t i = 0; i < num_labels - 1; i++) {
          // get branch target, check bounds
          const uint32_t label = labels[i];
          if (label >= max_label) {
            pwasm_checker_fail(checker, "br_table: label out of bounds");
            return false;
          }

          // get target control frame, check for error
          const pwasm_checker_ctrl_t *ctrl = pwasm_checker_ctrl_peek(checker, label);
          if (!ctrl) {
            // should never happen
            pwasm_checker_fail(checker, "br_table: null control frame for label");
            return false;
          }

          // get block type of target control frame
          const int32_t block_type = (ctrl->op == PWASM_OP_LOOP) ? PWASM_BLOCK_TYPE_VOID : ctrl->block_type;

          // compare target/last block type results, check for error
          bool same;
          if (!pwasm_block_type_compare_results(checker->mod, block_type, last_block_type, &same)) {
            pwasm_checker_fail(checker, "br_table: couldn't compare label block types");
            return false;
          }

          // check compare result
          if (!same) {
            // log error, return failure
            pwasm_checker_fail(checker, "br_table: invalid label block type results");
            return false;
          }
        }

        // get number of results, check for error
        size_t num_results;
        if (!pwasm_block_type_results_get_size(checker->mod, last_block_type, &num_results)) {
          pwasm_checker_fail(checker, "br_table: couldn't get block type num results");
          return false;
        }

        // pop results
        for (size_t j = 0; j < num_results; j++) {
          // calculate offset
          const size_t ofs = (num_results - 1 - j);

          // get value type, check for error
          uint32_t val_type;
          if (!pwasm_block_type_results_get_nth(checker->mod, last_block_type, ofs, &val_type)) {
          }

          // convert value type to checker type
          const pwasm_checker_type_t type = pwasm_value_type_to_checker_type(val_type);

          // pop expected type, check for error
          if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
            D("%s: type_pop_expected failed (val)", pwasm_op_get_name(in.op));
            return false;
          }
        }

        // mark control frame as unreachable
        if (!pwasm_checker_ctrl_mark_unreachable(checker)) {
          D("%s: ctrl_mark_unreachable failed", pwasm_op_get_name(in.op));
          return false;
        }
      }

      break;
    case PWASM_OP_RETURN:
      {
        const pwasm_slice_t results = mod->types[func.type_id].results;
        const uint32_t * const val_types = mod->u32s + results.ofs;

        // check results
        if (results.len >= pwasm_checker_type_get_size(checker)) {
          pwasm_checker_fail(checker, "return: result count greater than stack size");
          return false;
        }

        // pop/check function result types
        for (size_t i = 0; i < results.len; i++) {
          const pwasm_checker_type_t exp_type = pwasm_value_type_to_checker_type(val_types[results.len - 1 - i]);
          if (!pwasm_checker_type_pop_expected(checker, exp_type, NULL)) {
            return false;
          }
        }

        // mark control frame as unreachable
        // FIXME: is this correct?
        if (!pwasm_checker_ctrl_mark_unreachable(checker)) {
          return false;
        }
      }

      break;
    case PWASM_OP_CALL:
      // check call
      if (!pwasm_checker_check_call(checker, mod, mod->funcs[id])) {
        return false;
      }

      break;
    case PWASM_OP_CALL_INDIRECT:
      // pop indirect operand
      if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
        return false;
      }

      // check call
      if (!pwasm_checker_check_call(checker, mod, id)) {
        return false;
      }

      break;
    case PWASM_OP_DROP:
      if (!pwasm_checker_type_pop(checker, NULL)) {
        return false;
      }

      break;
    case PWASM_OP_SELECT:
      {
        // pop conditional
        if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
          return false;
        }

        // pop first type
        pwasm_checker_type_t type;
        if (!pwasm_checker_type_pop(checker, &type)) {
          return false;
        }

        // pop/check second type
        if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
          return false;
        }

        // push type
        if (!pwasm_checker_type_push(checker, type)) {
          return false;
        }
      }

      break;
    case PWASM_OP_LOCAL_GET:
      {
        // get checker type of local ID, check for error
        pwasm_checker_type_t type;
        if (!pwasm_checker_get_local_type(checker, mod, func, id, &type)) {
          return false;
        }
        D("type = %s", pwasm_checker_type_get_name(type));

        // push type
        if (!pwasm_checker_type_push(checker, type)) {
          return false;
        }
      }

      break;
    case PWASM_OP_LOCAL_SET:
      {
        // get checker type of local ID, check for error
        pwasm_checker_type_t type;
        if (!pwasm_checker_get_local_type(checker, mod, func, id, &type)) {
          return false;
        }

        // pop type
        if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
          return false;
        }
      }

      break;
    case PWASM_OP_LOCAL_TEE:
      {
        // get checker type of local ID, check for error
        pwasm_checker_type_t type;
        if (!pwasm_checker_get_local_type(checker, mod, func, id, &type)) {
          return false;
        }

        // pop type
        if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
          return false;
        }

        // push type
        if (!pwasm_checker_type_push(checker, type)) {
          return false;
        }
      }

      break;
    case PWASM_OP_GLOBAL_GET:
      {
        // get checker type of global, check for error
        pwasm_checker_type_t type;
        if (!pwasm_checker_get_global_type(checker, mod, id, &type, NULL)) {
          return false;
        }

        // push type
        if (!pwasm_checker_type_push(checker, type)) {
          return false;
        }
      }

      break;
    case PWASM_OP_GLOBAL_SET:
      {
        // get checker type of global, check for error
        bool mut = false;
        pwasm_checker_type_t type;
        if (!pwasm_checker_get_global_type(checker, mod, id, &type, &mut)) {
          return false;
        }

        if (!mut) {
          pwasm_checker_fail(checker, "global.set: immutable global");
          return false;
        }

        // pop type
        if (!pwasm_checker_type_pop_expected(checker, type, NULL)) {
          return false;
        }
      }

      break;
    case PWASM_OP_I32_LOAD:
    case PWASM_OP_I32_LOAD8_S:
    case PWASM_OP_I32_LOAD8_U:
    case PWASM_OP_I32_LOAD16_S:
    case PWASM_OP_I32_LOAD16_U:
    case PWASM_OP_I64_LOAD:
    case PWASM_OP_I64_LOAD8_S:
    case PWASM_OP_I64_LOAD8_U:
    case PWASM_OP_I64_LOAD16_S:
    case PWASM_OP_I64_LOAD16_U:
    case PWASM_OP_I64_LOAD32_S:
    case PWASM_OP_I64_LOAD32_U:
    case PWASM_OP_F32_LOAD:
    case PWASM_OP_F64_LOAD:
    case PWASM_OP_V128_LOAD:
    case PWASM_OP_I16X8_LOAD8X8_S:
    case PWASM_OP_I16X8_LOAD8X8_U:
    case PWASM_OP_I32X4_LOAD16X4_S:
    case PWASM_OP_I32X4_LOAD16X4_U:
    case PWASM_OP_I64X2_LOAD32X2_S:
    case PWASM_OP_I64X2_LOAD32X2_U:
    case PWASM_OP_V8X16_LOAD_SPLAT:
    case PWASM_OP_V16X8_LOAD_SPLAT:
    case PWASM_OP_V32X4_LOAD_SPLAT:
    case PWASM_OP_V64X2_LOAD_SPLAT:
    return PWASM_CHECKER_TYPE_V128;
      if (!pwasm_checker_check_load(checker, mod, in)) {
        return false;
      }

      break;
    case PWASM_OP_I32_STORE:
    case PWASM_OP_I32_STORE8:
    case PWASM_OP_I32_STORE16:
    case PWASM_OP_I64_STORE:
    case PWASM_OP_I64_STORE8:
    case PWASM_OP_I64_STORE16:
    case PWASM_OP_I64_STORE32:
    case PWASM_OP_F32_STORE:
    case PWASM_OP_F64_STORE:
    case PWASM_OP_V128_STORE:
      if (!pwasm_checker_check_store(checker, mod, in)) {
        return false;
      }

      break;
    case PWASM_OP_MEMORY_SIZE:
      {
        // check memory
        if (!pwasm_checker_check_mem(checker, mod)) {
          return false;
        }

        // check memory index (must be zero for current wasm)
        if (id != 0) {
          pwasm_checker_fail(checker, "memory.size: non-zero memory index");
          return false;
        }

        // push size
        if (!pwasm_checker_type_push(checker, PWASM_CHECKER_TYPE_I32)) {
          return false;
        }
      }

      break;
    case PWASM_OP_MEMORY_GROW:
      {
        // check memory
        if (!pwasm_checker_check_mem(checker, mod)) {
          return false;
        }

        // check memory index (must be zero for current wasm)
        if (id != 0) {
          pwasm_checker_fail(checker, "memory.grow: non-zero memory index");
          return false;
        }

        // pop size
        if (!pwasm_checker_type_pop_expected(checker, PWASM_CHECKER_TYPE_I32, NULL)) {
          return false;
        }

        // push result
        if (!pwasm_checker_type_push(checker, PWASM_CHECKER_TYPE_I32)) {
          return false;
        }
      }

      break;
    case PWASM_OP_I32_CONST:
    case PWASM_OP_I64_CONST:
    case PWASM_OP_F32_CONST:
    case PWASM_OP_F64_CONST:
    case PWASM_OP_V128_CONST:
      if (!pwasm_checker_check_const(checker, in.op)) {
        return false;
      }

      break;
    case PWASM_OP_I32_EQZ:
      if (!pwasm_checker_check_testop(checker, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_I32_EQ:
    case PWASM_OP_I32_NE:
    case PWASM_OP_I32_LT_S:
    case PWASM_OP_I32_LT_U:
    case PWASM_OP_I32_GT_S:
    case PWASM_OP_I32_GT_U:
    case PWASM_OP_I32_LE_S:
    case PWASM_OP_I32_LE_U:
    case PWASM_OP_I32_GE_S:
    case PWASM_OP_I32_GE_U:
      if (!pwasm_checker_check_relop(checker, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_I64_EQZ:
      if (!pwasm_checker_check_testop(checker, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_I64_EQ:
    case PWASM_OP_I64_NE:
    case PWASM_OP_I64_LT_S:
    case PWASM_OP_I64_LT_U:
    case PWASM_OP_I64_GT_S:
    case PWASM_OP_I64_GT_U:
    case PWASM_OP_I64_LE_S:
    case PWASM_OP_I64_LE_U:
    case PWASM_OP_I64_GE_S:
    case PWASM_OP_I64_GE_U:
      if (!pwasm_checker_check_relop(checker, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_F32_EQ:
    case PWASM_OP_F32_NE:
    case PWASM_OP_F32_LT:
    case PWASM_OP_F32_GT:
    case PWASM_OP_F32_LE:
    case PWASM_OP_F32_GE:
      if (!pwasm_checker_check_relop(checker, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_F64_EQ:
    case PWASM_OP_F64_NE:
    case PWASM_OP_F64_LT:
    case PWASM_OP_F64_GT:
    case PWASM_OP_F64_LE:
    case PWASM_OP_F64_GE:
      if (!pwasm_checker_check_relop(checker, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_I32_CLZ:
    case PWASM_OP_I32_CTZ:
    case PWASM_OP_I32_POPCNT:
      if (!pwasm_checker_check_unop(checker, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_I32_ADD:
    case PWASM_OP_I32_SUB:
    case PWASM_OP_I32_MUL:
    case PWASM_OP_I32_DIV_S:
    case PWASM_OP_I32_DIV_U:
    case PWASM_OP_I32_REM_S:
    case PWASM_OP_I32_REM_U:
    case PWASM_OP_I32_AND:
    case PWASM_OP_I32_OR:
    case PWASM_OP_I32_XOR:
    case PWASM_OP_I32_SHL:
    case PWASM_OP_I32_SHR_S:
    case PWASM_OP_I32_SHR_U:
    case PWASM_OP_I32_ROTL:
    case PWASM_OP_I32_ROTR:
      if (!pwasm_checker_check_binop(checker, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_I64_ADD:
    case PWASM_OP_I64_SUB:
    case PWASM_OP_I64_MUL:
    case PWASM_OP_I64_DIV_S:
    case PWASM_OP_I64_DIV_U:
    case PWASM_OP_I64_REM_S:
    case PWASM_OP_I64_REM_U:
    case PWASM_OP_I64_AND:
    case PWASM_OP_I64_OR:
    case PWASM_OP_I64_XOR:
    case PWASM_OP_I64_SHL:
    case PWASM_OP_I64_SHR_S:
    case PWASM_OP_I64_SHR_U:
    case PWASM_OP_I64_ROTL:
    case PWASM_OP_I64_ROTR:
      if (!pwasm_checker_check_binop(checker, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_F32_ABS:
    case PWASM_OP_F32_NEG:
    case PWASM_OP_F32_CEIL:
    case PWASM_OP_F32_FLOOR:
    case PWASM_OP_F32_TRUNC:
    case PWASM_OP_F32_NEAREST:
    case PWASM_OP_F32_SQRT:
      if (!pwasm_checker_check_unop(checker, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_F32_ADD:
    case PWASM_OP_F32_SUB:
    case PWASM_OP_F32_MUL:
    case PWASM_OP_F32_DIV:
    case PWASM_OP_F32_MIN:
    case PWASM_OP_F32_MAX:
    case PWASM_OP_F32_COPYSIGN:
      if (!pwasm_checker_check_binop(checker, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_F64_ABS:
    case PWASM_OP_F64_NEG:
    case PWASM_OP_F64_CEIL:
    case PWASM_OP_F64_FLOOR:
    case PWASM_OP_F64_TRUNC:
    case PWASM_OP_F64_NEAREST:
    case PWASM_OP_F64_SQRT:
      if (!pwasm_checker_check_unop(checker, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_F64_ADD:
    case PWASM_OP_F64_SUB:
    case PWASM_OP_F64_MUL:
    case PWASM_OP_F64_DIV:
    case PWASM_OP_F64_MIN:
    case PWASM_OP_F64_MAX:
    case PWASM_OP_F64_COPYSIGN:
      if (!pwasm_checker_check_binop(checker, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_I32_WRAP_I64:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I32, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_I32_TRUNC_F32_S:
    case PWASM_OP_I32_TRUNC_F32_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I32, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_I32_TRUNC_F64_S:
    case PWASM_OP_I32_TRUNC_F64_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I32, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_I64_EXTEND_I32_S:
    case PWASM_OP_I64_EXTEND_I32_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I64, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_I64_TRUNC_F32_S:
    case PWASM_OP_I64_TRUNC_F32_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I64, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_I64_TRUNC_F64_S:
    case PWASM_OP_I64_TRUNC_F64_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I64, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_F32_CONVERT_I32_S:
    case PWASM_OP_F32_CONVERT_I32_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F32, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_F32_CONVERT_I64_S:
    case PWASM_OP_F32_CONVERT_I64_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F32, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_F32_DEMOTE_F64:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F32, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_F64_CONVERT_I32_S:
    case PWASM_OP_F64_CONVERT_I32_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F64, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_F64_CONVERT_I64_S:
    case PWASM_OP_F64_CONVERT_I64_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F64, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_F64_PROMOTE_F32:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F64, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_I32_REINTERPRET_F32:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I32, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_I64_REINTERPRET_F64:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I64, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_F32_REINTERPRET_I32:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F32, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_F64_REINTERPRET_I64:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_F64, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_I32_EXTEND8_S:
    case PWASM_OP_I32_EXTEND16_S:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I32, PWASM_CHECKER_TYPE_I32)) {
        return false;
      }

      break;
    case PWASM_OP_I64_EXTEND8_S:
    case PWASM_OP_I64_EXTEND16_S:
    case PWASM_OP_I64_EXTEND32_S:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I64, PWASM_CHECKER_TYPE_I64)) {
        return false;
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F32_S:
    case PWASM_OP_I32_TRUNC_SAT_F32_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I32, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F64_S:
    case PWASM_OP_I32_TRUNC_SAT_F64_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I32, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F32_S:
    case PWASM_OP_I64_TRUNC_SAT_F32_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I64, PWASM_CHECKER_TYPE_F32)) {
        return false;
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F64_S:
    case PWASM_OP_I64_TRUNC_SAT_F64_U:
      if (!pwasm_checker_check_cvtop(checker, PWASM_CHECKER_TYPE_I64, PWASM_CHECKER_TYPE_F64)) {
        return false;
      }

      break;
    case PWASM_OP_V8X16_SHUFFLE:
      // check lane immediate
      if (!pwasm_checker_check_lane(checker, in)) {
        return false;
      }

      if (!pwasm_checker_check_binop(checker, PWASM_CHECKER_TYPE_V128)) {
        return false;
      }

      break;
    case PWASM_OP_I8X16_ANY_TRUE:
    case PWASM_OP_I8X16_ALL_TRUE:
    case PWASM_OP_I16X8_ANY_TRUE:
    case PWASM_OP_I16X8_ALL_TRUE:
    case PWASM_OP_I32X4_ANY_TRUE:
    case PWASM_OP_I32X4_ALL_TRUE:
      if (!pwasm_checker_check_testop(checker, PWASM_CHECKER_TYPE_V128)) {
        return false;
      }

      break;
    case PWASM_OP_I8X16_NEG:
    case PWASM_OP_I16X8_NEG:
    case PWASM_OP_I32X4_NEG:
    case PWASM_OP_I64X2_NEG:
    case PWASM_OP_F32X4_ABS:
    case PWASM_OP_F32X4_NEG:
    case PWASM_OP_F32X4_SQRT:
    case PWASM_OP_F64X2_ABS:
    case PWASM_OP_F64X2_NEG:
    case PWASM_OP_F64X2_SQRT:
    case PWASM_OP_I32X4_TRUNC_SAT_F32X4_S:
    case PWASM_OP_I32X4_TRUNC_SAT_F32X4_U:
    case PWASM_OP_F32X4_CONVERT_I32X4_S:
    case PWASM_OP_F32X4_CONVERT_I32X4_U:
    case PWASM_OP_I16X8_WIDEN_LOW_I8X16_S:
    case PWASM_OP_I16X8_WIDEN_HIGH_I8X16_S:
    case PWASM_OP_I16X8_WIDEN_LOW_I8X16_U:
    case PWASM_OP_I16X8_WIDEN_HIGH_I8X16_U:
    case PWASM_OP_I32X4_WIDEN_LOW_I16X8_S:
    case PWASM_OP_I32X4_WIDEN_HIGH_I16X8_S:
    case PWASM_OP_I32X4_WIDEN_LOW_I16X8_U:
    case PWASM_OP_I32X4_WIDEN_HIGH_I16X8_U:
    case PWASM_OP_I8X16_ABS:
    case PWASM_OP_I16X8_ABS:
    case PWASM_OP_I32X4_ABS:
    case PWASM_OP_V128_NOT:
      if (!pwasm_checker_check_unop(checker, PWASM_CHECKER_TYPE_V128)) {
        return false;
      }

      break;
    case PWASM_OP_V8X16_SWIZZLE:
    case PWASM_OP_I8X16_EQ:
    case PWASM_OP_I8X16_NE:
    case PWASM_OP_I8X16_LT_S:
    case PWASM_OP_I8X16_LT_U:
    case PWASM_OP_I8X16_GT_S:
    case PWASM_OP_I8X16_GT_U:
    case PWASM_OP_I8X16_LE_S:
    case PWASM_OP_I8X16_LE_U:
    case PWASM_OP_I8X16_GE_S:
    case PWASM_OP_I8X16_GE_U:
    case PWASM_OP_I16X8_EQ:
    case PWASM_OP_I16X8_NE:
    case PWASM_OP_I16X8_LT_S:
    case PWASM_OP_I16X8_LT_U:
    case PWASM_OP_I16X8_GT_S:
    case PWASM_OP_I16X8_GT_U:
    case PWASM_OP_I16X8_LE_S:
    case PWASM_OP_I16X8_LE_U:
    case PWASM_OP_I16X8_GE_S:
    case PWASM_OP_I16X8_GE_U:
    case PWASM_OP_I32X4_EQ:
    case PWASM_OP_I32X4_NE:
    case PWASM_OP_I32X4_LT_S:
    case PWASM_OP_I32X4_LT_U:
    case PWASM_OP_I32X4_GT_S:
    case PWASM_OP_I32X4_GT_U:
    case PWASM_OP_I32X4_LE_S:
    case PWASM_OP_I32X4_LE_U:
    case PWASM_OP_I32X4_GE_S:
    case PWASM_OP_I32X4_GE_U:
    case PWASM_OP_F32X4_EQ:
    case PWASM_OP_F32X4_NE:
    case PWASM_OP_F32X4_LT:
    case PWASM_OP_F32X4_GT:
    case PWASM_OP_F32X4_LE:
    case PWASM_OP_F32X4_GE:
    case PWASM_OP_F64X2_EQ:
    case PWASM_OP_F64X2_NE:
    case PWASM_OP_F64X2_LT:
    case PWASM_OP_F64X2_GT:
    case PWASM_OP_F64X2_LE:
    case PWASM_OP_F64X2_GE:
    case PWASM_OP_V128_AND:
    case PWASM_OP_V128_OR:
    case PWASM_OP_V128_XOR:
    case PWASM_OP_I8X16_ADD:
    case PWASM_OP_I8X16_ADD_SATURATE_S:
    case PWASM_OP_I8X16_ADD_SATURATE_U:
    case PWASM_OP_I8X16_SUB:
    case PWASM_OP_I8X16_SUB_SATURATE_S:
    case PWASM_OP_I8X16_SUB_SATURATE_U:
    case PWASM_OP_I8X16_MIN_S:
    case PWASM_OP_I8X16_MIN_U:
    case PWASM_OP_I8X16_MAX_S:
    case PWASM_OP_I8X16_MAX_U:
    case PWASM_OP_I16X8_ADD:
    case PWASM_OP_I16X8_ADD_SATURATE_S:
    case PWASM_OP_I16X8_ADD_SATURATE_U:
    case PWASM_OP_I16X8_SUB:
    case PWASM_OP_I16X8_SUB_SATURATE_S:
    case PWASM_OP_I16X8_SUB_SATURATE_U:
    case PWASM_OP_I16X8_MUL:
    case PWASM_OP_I16X8_MIN_S:
    case PWASM_OP_I16X8_MIN_U:
    case PWASM_OP_I16X8_MAX_S:
    case PWASM_OP_I16X8_MAX_U:
    case PWASM_OP_I32X4_ADD:
    case PWASM_OP_I32X4_SUB:
    case PWASM_OP_I32X4_MUL:
    case PWASM_OP_I32X4_MIN_S:
    case PWASM_OP_I32X4_MIN_U:
    case PWASM_OP_I32X4_MAX_S:
    case PWASM_OP_I32X4_MAX_U:
    case PWASM_OP_I64X2_ADD:
    case PWASM_OP_I64X2_SUB:
    case PWASM_OP_I64X2_MUL:
    case PWASM_OP_F32X4_ADD:
    case PWASM_OP_F32X4_SUB:
    case PWASM_OP_F32X4_MUL:
    case PWASM_OP_F32X4_DIV:
    case PWASM_OP_F32X4_MIN:
    case PWASM_OP_F32X4_MAX:
    case PWASM_OP_F64X2_ADD:
    case PWASM_OP_F64X2_SUB:
    case PWASM_OP_F64X2_MUL:
    case PWASM_OP_F64X2_DIV:
    case PWASM_OP_F64X2_MIN:
    case PWASM_OP_F64X2_MAX:
    case PWASM_OP_I8X16_NARROW_I16X8_S:
    case PWASM_OP_I8X16_NARROW_I16X8_U:
    case PWASM_OP_I16X8_NARROW_I32X4_S:
    case PWASM_OP_I16X8_NARROW_I32X4_U:
    case PWASM_OP_V128_ANDNOT:
    case PWASM_OP_I8X16_AVGR_U:
    case PWASM_OP_I16X8_AVGR_U:
      if (!pwasm_checker_check_binop(checker, PWASM_CHECKER_TYPE_V128)) {
        return false;
      }

      break;
    case PWASM_OP_V128_BITSELECT:
      if (!pwasm_checker_check_triop(checker, PWASM_CHECKER_TYPE_V128)) {
        return false;
      }

      break;
    case PWASM_OP_I8X16_SPLAT:
    case PWASM_OP_I16X8_SPLAT:
    case PWASM_OP_I32X4_SPLAT:
    case PWASM_OP_I64X2_SPLAT:
    case PWASM_OP_F32X4_SPLAT:
    case PWASM_OP_F64X2_SPLAT:
      // check splat
      if (!pwasm_checker_check_splat(checker, in.op)) {
        return false;
      }

      break;
    case PWASM_OP_I8X16_EXTRACT_LANE_S:
    case PWASM_OP_I8X16_EXTRACT_LANE_U:
    case PWASM_OP_I16X8_EXTRACT_LANE_S:
    case PWASM_OP_I16X8_EXTRACT_LANE_U:
    case PWASM_OP_I32X4_EXTRACT_LANE:
    case PWASM_OP_I64X2_EXTRACT_LANE:
    case PWASM_OP_F32X4_EXTRACT_LANE:
    case PWASM_OP_F64X2_EXTRACT_LANE:
      // check extract
      if (!pwasm_checker_check_extract(checker, in)) {
        return false;
      }

      break;
    case PWASM_OP_I8X16_REPLACE_LANE:
    case PWASM_OP_I16X8_REPLACE_LANE:
    case PWASM_OP_I32X4_REPLACE_LANE:
    case PWASM_OP_I64X2_REPLACE_LANE:
    case PWASM_OP_F32X4_REPLACE_LANE:
    case PWASM_OP_F64X2_REPLACE_LANE:
      // check replace
      if (!pwasm_checker_check_replace(checker, in)) {
        return false;
      }

      break;
    case PWASM_OP_I8X16_SHL:
    case PWASM_OP_I8X16_SHR_S:
    case PWASM_OP_I8X16_SHR_U:
    case PWASM_OP_I16X8_SHL:
    case PWASM_OP_I16X8_SHR_S:
    case PWASM_OP_I16X8_SHR_U:
    case PWASM_OP_I32X4_SHL:
    case PWASM_OP_I32X4_SHR_S:
    case PWASM_OP_I32X4_SHR_U:
    case PWASM_OP_I64X2_SHL:
    case PWASM_OP_I64X2_SHR_S:
    case PWASM_OP_I64X2_SHR_U:
      // check shift
      if (!pwasm_checker_check_shift(checker)) {
        return false;
      }

      break;
    default:
      D("ignoring op: 0x%08x", in.op);
      // ignore
      break;
    }
  }

  // return success
  return true;
}

/**
 * Internal data for pwasm_mod_check()
 */
typedef struct {
  const pwasm_mod_check_cbs_t cbs; /** callbacks */
  void *cb_data; /** user data */
  pwasm_checker_t checker; /** code checker */
} pwasm_mod_check_t;

/**
 * Initialize mod checker internal data.
 */
static bool
pwasm_mod_check_init(
  pwasm_mod_check_t * const check,
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_cbs_t * const cbs,
  void *cb_data
) {
  // init code checker, check for error
  pwasm_checker_t checker;
  if (!pwasm_checker_init(&checker, mod, cbs, cb_data)) {
    // return failure
    return false;
  }

  // populate result
  pwasm_mod_check_t tmp = {
    .cbs = {
      .on_warning = (cbs && cbs->on_warning) ? cbs->on_warning : pwasm_null_on_warning,
      .on_error = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error,
    },
    .cb_data = cb_data,

    .checker = checker,
  };

  // copy to destination
  memcpy(check, &tmp, sizeof(pwasm_mod_check_t));

  // return success
  return true;
}

/**
 * Finalize mod checker internal data.
 */
static void
pwasm_mod_check_fini(
  pwasm_mod_check_t * const check
) {
  pwasm_checker_fini(&(check->checker));
}

/**
 * Verify that a limits structure in a module is valid.
 *
 * Returns true if the limits structure validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 *
 * Reference:
 * https://webassembly.github.io/spec/core/valid/types.html#limits
 */
static bool
pwasm_mod_check_limits(
  const pwasm_limits_t limits,
  const pwasm_mod_check_t * const check
) {
  if (limits.has_max && (limits.max < limits.min)) {
    check->cbs.on_error("limits.max < limits.min", check->cb_data);
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that a function type index in a module is valid.
 *
 * Returns true if the function type index is valid and false otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static inline bool
pwasm_mod_check_type_id(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const uint32_t id
) {
  // check type index
  if (id >= mod->num_types) {
    check->cbs.on_error("type index out of bounds", check->cb_data);
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that an instruction in a constant expression is valid.
 *
 * This function checks the following conditions:
 *
 * * The instruction is a global.get or a *.const instruction.
 * * If the instruction is a global.get instruction, then the
 *   index refers to an imported global.
 *
 * Returns true if the instruction validates successfully and
 * false otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 *
 * Reference:
 * https://webassembly.github.io/spec/core/valid/instructions.html#constant-expressions
 */
static bool
pwasm_mod_check_const_expr_inst(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_inst_t in
) {
  // number of imported globals in this module
  const size_t num_global_imports = mod->num_import_types[PWASM_IMPORT_TYPE_GLOBAL];

  // instruction index immediate
  const uint32_t id = in.v_index;

  // is this a valid instruction for a constant expr?
  if (
    !pwasm_op_is_const(in.op) &&
    (in.op != PWASM_OP_GLOBAL_GET) &&
    (in.op != PWASM_OP_END)
  ) {
    check->cbs.on_error("non-constant instruction found in constant expression", check->cb_data);
    return false;
  }

  // is this a global.get inst that references a non-imported global?
  if ((in.op == PWASM_OP_GLOBAL_GET) && id >= num_global_imports) {
    check->cbs.on_error("constant expressions cannot reference non-imported globals", check->cb_data);
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that the instructions in a constant expression are valid.
 *
 * Returns true if the constant expression validates successfully and
 * false otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 *
 * Reference:
 * https://webassembly.github.io/spec/core/valid/instructions.html#constant-expressions
 */
static bool
pwasm_mod_check_const_expr(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_slice_t expr
) {
  const pwasm_inst_t * const insts = mod->insts + expr.ofs;

  // check instructions
  for (size_t i = 0; i < expr.len; i++) {
    if (!pwasm_mod_check_const_expr_inst(mod, check, insts[i])) {
      return false;
    }
  }

  // return success
  return true;
}

/**
 * Verify that a custom section in a parsed module is valid.
 *
 * Returns true if the custom section validates successfully and
 * false otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_custom_section(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_custom_section_t section
) {
  // is the custom section name valid utf8?
  if (!pwasm_mod_check_is_valid_utf8(mod, section.name)) {
    // Note: this is a warning rather than an error because the wasm
    // spec says that errors in custom sections should be non-fatal.
    //
    // TODO: verify that this is correct
    check->cbs.on_warning("bad UTF-8 in custom section name", check->cb_data);
  }

  // return success
  return true;
}

static bool
pwasm_mod_check_type_vals(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_slice_t slice
) {
  // get values
  const uint32_t * const vals = mod->u32s + slice.ofs;

  // check values
  for (size_t i = 0; i < slice.len; i++) {
    if (!pwasm_is_valid_value_type(vals[i])) {
      check->cbs.on_error("invalid value type in function type", check->cb_data);
      return false;
    }
  }

  // return success
  return true;
}

/**
 * Verify that a function type in a parsed module is valid.
 *
 * Returns true if the function type validates successfully and
 * false otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_type(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_type_t type
) {
  // check parameters
  if (!pwasm_mod_check_type_vals(mod, check, type.params)) {
    return false;
  }

  // check results
  if (!pwasm_mod_check_type_vals(mod, check, type.results)) {
    return false;
  }

  // check result count
  // NOTE: disabled, because blocks can have multiple results
  if (type.results.len > 1) {
    // check->cbs.on_warning("function type result count > 1", check->cb_data);
  }

  // return success
  return true;
}

/**
 * Verify that a function entry in a module is valid.
 *
 * Returns true if the function entry is valid and false otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static inline bool
pwasm_mod_check_func(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const uint32_t id
) {
  return pwasm_mod_check_type_id(mod, check, id);
}

/**
 * Verify that a global in a module are valid.
 *
 * Returns true if the global validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_global(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_global_t global
) {
  // check init expr
  if (!pwasm_mod_check_const_expr(mod, check, global.expr)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that a table element in a module is valid.
 *
 * Returns true if the table element validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_elem(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_elem_t elem
) {
  // get the maximum table ID
  const size_t max_tables = mod->max_indices[PWASM_IMPORT_TYPE_TABLE];

  // check table index
  if (elem.table_id >= max_tables) {
    check->cbs.on_error("invalid table index in element", check->cb_data);
    return false;
  }

  // check constant expression
  if (!pwasm_mod_check_const_expr(mod, check, elem.expr)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that a data segment in a module is valid.
 *
 * Returns true if the data segment validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_segment(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_segment_t segment
) {
  const size_t max_mems = mod->max_indices[PWASM_IMPORT_TYPE_MEM];

  // check memory index
  if (segment.mem_id >= max_mems) {
    check->cbs.on_error("invalid memory index in segment", check->cb_data);
    return false;
  }

  // check init expr
  if (!pwasm_mod_check_const_expr(mod, check, segment.expr)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that a memory entry in a module is valid.
 *
 * Returns true if the memory entry validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 *
 * Reference:
 * https://webassembly.github.io/spec/core/valid/types.html#memory-types
 */
static bool
pwasm_mod_check_mem(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_limits_t mem
) {
  (void) mod;

  // check limits
  if (!pwasm_mod_check_limits(mem, check)) {
    return false;
  }

  // mem.min bounds check
  if (mem.min > 0x10000) {
    check->cbs.on_error("mem.min > 0x10000", check->cb_data);
    return false;
  }

  // mem.max bound check
  if (mem.has_max && (mem.max > 0x10000)) {
    check->cbs.on_error("mem.max > 0x10000", check->cb_data);
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that a table in a module is valid.
 *
 * Returns true if the table validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 *
 * Reference:
 * https://webassembly.github.io/spec/core/binary/types.html#table-types
 */
static bool
pwasm_mod_check_table(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_table_t table
) {
  (void) mod;

  // check element type
  // (note: redundant, since this is checked during parsing)
  if (table.elem_type != 0x70) {
    check->cbs.on_error("invalid table element type", check->cb_data);
    return false;
  }

  // check limits
  if (!pwasm_mod_check_limits(table.limits, check)) {
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that an import in a module is valid.
 *
 * Returns true if the import validate successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_import(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_import_t import
) {
  (void) mod;

  // is the import module name valid utf8?
  if (!pwasm_mod_check_is_valid_utf8(mod, import.module)) {
    check->cbs.on_error("import module name is not UTF-8", check->cb_data);
    return false;
  }

  // is the import entry name valid utf8?
  if (!pwasm_mod_check_is_valid_utf8(mod, import.name)) {
    check->cbs.on_error("import entry name is not UTF-8", check->cb_data);
    return false;
  }

  // check import data
  switch (import.type) {
  case PWASM_IMPORT_TYPE_FUNC:
    return pwasm_mod_check_type_id(mod, check, import.func);
  case PWASM_IMPORT_TYPE_TABLE:
    return pwasm_mod_check_table(mod, check, import.table);
  case PWASM_IMPORT_TYPE_GLOBAL:
    // nothing to check
    return true;
  case PWASM_IMPORT_TYPE_MEM:
    return pwasm_mod_check_mem(mod, check, import.mem);
  default:
    check->cbs.on_error("invalid import type", check->cb_data);
    return false;
  }
}

/**
 * Returns true if the given index is a valid index for the given export
 * type in the given module, or false otherwise.
 */
static inline bool
pwasm_mod_check_export_id(
  const pwasm_mod_t * const mod,
  const pwasm_import_type_t type,
  const uint32_t id
) {
  switch (type) {
  case PWASM_IMPORT_TYPE_FUNC:
    return id < (mod->num_funcs + mod->num_import_types[PWASM_IMPORT_TYPE_FUNC]);
  case PWASM_IMPORT_TYPE_TABLE:
    return id < (mod->num_tables + mod->num_import_types[PWASM_IMPORT_TYPE_TABLE]);
  case PWASM_IMPORT_TYPE_MEM:
    return id < (mod->num_mems + mod->num_import_types[PWASM_IMPORT_TYPE_MEM]);
  case PWASM_IMPORT_TYPE_GLOBAL:
    return id < (mod->num_globals + mod->num_import_types[PWASM_IMPORT_TYPE_GLOBAL]);
  default:
    return false;
  }
}

/**
 * Verify that an export in a module is valid.
 *
 * Returns true if the export validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_export(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check,
  const pwasm_export_t export
) {
  // is the export name valid utf8?
  if (!pwasm_mod_check_is_valid_utf8(mod, export.name)) {
    check->cbs.on_error("export name is not UTF-8", check->cb_data);
    return false;
  }

  // is the export type valid?
  // (note: redundant, caught in parsing)
  if (export.type >= PWASM_IMPORT_TYPE_LAST) {
    check->cbs.on_error("invalid export type", check->cb_data);
    return false;
  }

  // check export index
  // (TODO: should be caught in parsing)
  if (!pwasm_mod_check_export_id(mod, export.type, export.id)) {
    check->cbs.on_error("invalid export index", check->cb_data);
    return false;
  }

  // return success
  return true;
}

/**
 * Verify that a function code entry in a module is valid.
 *
 * Returns true if the function validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_code(
  const pwasm_mod_t * const mod,
  pwasm_mod_check_t * const check,
  const pwasm_func_t func
) {
  return pwasm_checker_check(&(check->checker), mod, func);
}

/**
 * Verify that the start function in a module is valid.
 *
 * Returns true if the start function validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 */
static bool
pwasm_mod_check_start(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_t * const check
) {
  if (!mod->has_start) {
    // return success
    return true;
  }

  // check index
  if (!pwasm_mod_is_valid_index(mod, PWASM_IMPORT_TYPE_FUNC, mod->start)) {
    check->cbs.on_error("invalid start function index", check->cb_data);
    return false;
  }

  // get function prototype, check for error
  const pwasm_type_t * const type = pwasm_mod_get_func_type(mod, mod->start);
  if (!type) {
    check->cbs.on_error("invalid start function type", check->cb_data);
    return false;
  }

  // check parameter count
  if (type->params.len > 0) {
    check->cbs.on_error("start function has non-zero parameter count", check->cb_data);
    return false;
  }

  // check result count
  if (type->results.len > 0) {
    check->cbs.on_error("start function has non-zero result count", check->cb_data);
    return false;
  }

  // return success
  return true;
}

/**
 * Mod checks.
 *
 * Note: The start function check is handled separately in
 * pwasm_mod_check().
 */
#define MOD_CHECKS \
  MOD_CHECK(custom_section) \
  MOD_CHECK(type) \
  MOD_CHECK(import) \
  MOD_CHECK(func) \
  MOD_CHECK(global) \
  MOD_CHECK(segment) \
  MOD_CHECK(mem) \
  MOD_CHECK(elem) \
  MOD_CHECK(table) \
  MOD_CHECK(export) \
  MOD_CHECK(code)

#define MOD_CHECK(NAME) \
  static bool pwasm_mod_check_ ## NAME ## s( \
    const pwasm_mod_t * const mod, \
    pwasm_mod_check_t * const check \
  ) { \
    for (size_t i = 0; i < mod->num_ ## NAME ## s; i++) { \
      if (!pwasm_mod_check_ ## NAME (mod, check, mod->NAME ## s[i])) { \
        return false; \
      } \
    } \
  \
    /* return success */ \
    return true; \
  }
MOD_CHECKS
#undef MOD_CHECK

/**
 * Verify that a parsed module is valid.
 *
 * Returns true if the module validates successfully and false
 * otherwise.
 *
 * If a validation error occurs, and the +cbs+ parameter and
 * +cbs->on_error+ are both non-NULL, then +cbs->on_error+ will be
 * called with an error message describing the validation error.
 *
 * Note: this function is called by `pwasm_mod_init()`, so you only
 * need to call `pwasm_mod_check()` if you parsed the module with
 * `pwasm_mod_init_unsafe()`.
 */
bool
pwasm_mod_check(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_cbs_t * const cbs,
  void *cb_data
) {
  // init mod check context
  pwasm_mod_check_t check;
  if (!pwasm_mod_check_init(&check, mod, cbs, cb_data)) {
    return false;
  }

  // check start function
  if (!pwasm_mod_check_start(mod, &check)) {
    // return failure
    return false;
  }

  #define MOD_CHECK(NAME) \
    if (!pwasm_mod_check_ ## NAME ## s(mod, &check)) { \
      return false; \
    }
  MOD_CHECKS
  #undef MOD_CHECK

  // fini mod check context
  pwasm_mod_check_fini(&check);

  // return success
  return true;
}

bool
pwasm_jit_compile(
  pwasm_jit_t *jit, // JIT compiler
  pwasm_buf_t *dst, // destination buffer
  pwasm_env_t *env, // env
  const uint32_t mod_id, // module instance handle
  const size_t func_ofs // function offset
) {
  if (!jit || !jit->cbs || !jit->cbs->compile) {
    // return failure
    return false;
  }

  return jit->cbs->compile(jit, dst, env, mod_id, func_ofs);
}

void
pwasm_jit_fini(
  pwasm_jit_t *jit // JIT compiler
) {
  if (jit && jit->cbs && jit->cbs->fini) {
    jit->cbs->fini(jit);
  }
}

bool
pwasm_env_init(
  pwasm_env_t * env,
  pwasm_mem_ctx_t * mem_ctx,
  const pwasm_env_cbs_t * cbs,
  pwasm_stack_t * stack,
  void *user_data
) {
  pwasm_env_t tmp = {
    .mem_ctx    = mem_ctx,
    .cbs        = cbs,
    .stack      = stack,
    .user_data  = user_data,
  };
  memcpy(env, &tmp, sizeof(pwasm_env_t));

  return (cbs && cbs->init) ? cbs->init(env) : true;
}

void
pwasm_env_fini(
  pwasm_env_t * const env
) {
  if (env->cbs && env->cbs->fini) {
    env->cbs->fini(env);
  }
}

void *
pwasm_env_get_data(
  const pwasm_env_t * const env
) {
  return env->user_data;
}

uint32_t
pwasm_env_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  return (cbs && cbs->add_mod) ? cbs->add_mod(env, name, mod) : 0;
}

uint32_t
pwasm_env_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  return (cbs && cbs->add_native) ? cbs->add_native(env, name, mod) : 0;
}

bool
pwasm_env_get_global(
  pwasm_env_t * const env,
  const uint32_t id,
  pwasm_val_t * const ret_val
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  D("in get global, id = %u", id);
  return (cbs && cbs->get_global) ? cbs->get_global(env, id, ret_val) : false;
}

bool
pwasm_env_set_global(
  pwasm_env_t * const env,
  const uint32_t id,
  const pwasm_val_t val
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  return (cbs && cbs->set_global) ? cbs->set_global(env, id, val) : false;
}

const pwasm_mod_t *
pwasm_env_get_mod(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  return (cbs && cbs->get_mod) ? cbs->get_mod(env, mod_id) : NULL;
}

const pwasm_buf_t *
pwasm_env_get_mod_name(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  return (cbs && cbs->get_mod_name) ? cbs->get_mod_name(env, mod_id) : NULL;
}

uint32_t
pwasm_env_find_mod(
  pwasm_env_t * const env,
  const pwasm_buf_t name
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, name = \"%s\"", env, name);
  return (cbs && cbs->find_mod) ? cbs->find_mod(env, name) : 0;
}

uint32_t
pwasm_env_find_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, mod_id = %u, name = \"%s\"", env, mod_id, name);
  return (cbs && cbs->find_func) ? cbs->find_func(env, mod_id, name) : 0;
}

uint32_t
pwasm_env_find_mem(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, mod_id = %u, name = \"%s\"", env, mod_id, name);
  return (cbs && cbs->find_mem) ? cbs->find_mem(env, mod_id, name) : 0;
}

pwasm_env_mem_t *
pwasm_env_get_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, mod_id = %u, name = \"%s\"", env, mod_id, name);
  return (cbs && cbs->get_mem) ? cbs->get_mem(env, mem_id) : 0;
}

uint32_t
pwasm_env_find_global(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, mod_id = %u, name = \"%s\"", env, mod_id, name);
  return (cbs && cbs->find_global) ? cbs->find_global(env, mod_id, name) : 0;
}

uint32_t
pwasm_env_find_table(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, mod_id = %u, name = \"%s\"", env, mod_id, name);
  return (cbs && cbs->find_table) ? cbs->find_table(env, mod_id, name) : 0;
}

bool
pwasm_env_call(
  pwasm_env_t * const env,
  const uint32_t func_id
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, func_id = %u", (void*) env, func_id);
  return (cbs && cbs->call) ? cbs->call(env, func_id) : false;
}

bool
pwasm_env_call_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const uint32_t func_ofs
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, func_id = %u", (void*) env, func_id);
  return (cbs && cbs->call_func) ? cbs->call_func(env, mod_id, func_ofs) : false;
}

uint32_t
pwasm_env_get_table_index(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const uint32_t table_ofs
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  // D("env = %p, func_id = %u", (void*) env, func_id);
  return (cbs && cbs->get_table_index) ? cbs->get_table_index(env, mod_id, table_ofs) : false;
}

bool
pwasm_env_mem_load(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t ofs,
  pwasm_val_t * const ret_val
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  const bool have_cb = (cbs && cbs->mem_load);
  return have_cb ? cbs->mem_load(env, mem_id, in, ofs, ret_val) : false;
}

bool
pwasm_env_mem_store(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t ofs,
  const pwasm_val_t val
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  const bool have_cb = (cbs && cbs->mem_store);
  return have_cb ? cbs->mem_store(env, mem_id, in, ofs, val) : false;
}

bool
pwasm_env_mem_size(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  uint32_t * const ret_val
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  const bool have_cb = (cbs && cbs->mem_size);
  return have_cb ? cbs->mem_size(env, mem_id, ret_val) : false;
}

bool
pwasm_env_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  const bool have_cb = (cbs && cbs->mem_grow);
  return have_cb ? cbs->mem_grow(env, mem_id, grow, ret_val) : false;
}

bool
pwasm_env_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_id
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  const bool have_cb = (cbs && cbs->get_elem);
  return have_cb ? cbs->get_elem(env, table_id, elem_ofs, ret_id) : false;
}

uint32_t
pwasm_env_find_import(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  const bool have_cb = (cbs && cbs->find_import);
  return have_cb ? cbs->find_import(env, mod_id, type, name) : 0;
}

static void
pwasm_env_fail(
  pwasm_env_t * const env,
  const char * const text
) {
  env->mem_ctx->cbs->on_error(text, env->mem_ctx->cb_data);
}

/*
 * Friendly wrapper around pwasm_env_find_mod() which takes a string
 * pointer instead of a buffer.
 */
uint32_t
pwasm_find_mod(
  pwasm_env_t * const env,
  const char * const mod_name
) {
  return pwasm_env_find_mod(env, pwasm_buf_str(mod_name));
}

/*
 * Friendly wrapper around pwasm_env_find_func() which takes a string
 * pointer module and function name instead of module handle and a
 * function name buffer.
 */
uint32_t
pwasm_find_func(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name
) {
  const uint32_t mod_id = pwasm_find_mod(env, mod);
  return pwasm_env_find_func(env, mod_id, pwasm_buf_str(name));
}

/*
 * Friendly wrapper around pwasm_env_get_mem() which takes a string
 * pointer module and memory name instead of a buffer.
 */
pwasm_env_mem_t *
pwasm_get_mem(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name
) {
  const uint32_t mod_id = pwasm_find_mod(env, mod);
  const uint32_t mem_id = pwasm_env_find_mem(env, mod_id, pwasm_buf_str(name));
  return pwasm_env_get_mem(env, mem_id);
}

/*
 * Find global in environment by module name and global name and return
 * a handle to the global instance.
 *
 * Note: This is a convenience wrapper around pwasm_env_get_global().
 *
 * Returns 0 if an error occurred.
 */
uint32_t pwasm_find_global(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name
) {
  const uint32_t mod_id = pwasm_find_mod(env, mod);
  return pwasm_env_find_global(env, mod_id, pwasm_buf_str(name));
}

/*
 * Find global in environment by module name and global name and return
 * a pointer to the global instance.
 *
 * Note: This is a convenience wrapper around pwasm_env_get_global().
 *
 * Returns NULL if an error occurred.
 */
bool pwasm_get_global(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name,
  pwasm_val_t * const ret
) {
  const uint32_t id = pwasm_find_global(env, mod, name);
  return pwasm_env_get_global(env, id, ret);
}

bool
pwasm_set_global(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name,
  const pwasm_val_t val
) {
  const uint32_t id = pwasm_find_global(env, mod, name);
  return pwasm_env_set_global(env, id, val);
}

bool
pwasm_call(
  pwasm_env_t * const env,
  const char * const mod_name,
  const char * const func_name
) {
  // D("env = %p, mod = %s, func = %s", (void*) env, mod_name, func_name);
  return pwasm_env_call(env, pwasm_find_func(env, mod_name, func_name));
}

//
// control stack: used by new interpreter to manage control frames
//

#define CTRL_TYPES \
  CTRL_TYPE(BLOCK) \
  CTRL_TYPE(LOOP) \
  CTRL_TYPE(IF) \
  CTRL_TYPE(ELSE)

typedef enum {
#define CTRL_TYPE(a) CTRL_ ## a,
CTRL_TYPES
#undef CTRL_TYPE
  CTRL_LAST,
} pwasm_ctrl_stack_entry_type_t;

typedef struct {
  pwasm_ctrl_stack_entry_type_t type; // stack entry type
  size_t depth; // value stack depth
  size_t ofs; // inst ofs
  uint32_t func_type;
} pwasm_ctrl_stack_entry_t;

typedef struct {
  pwasm_vec_t stack;
} pwasm_ctrl_stack_t;

static bool
pwasm_ctrl_stack_init(
  pwasm_ctrl_stack_t * const stack,
  pwasm_mem_ctx_t * const mem_ctx
) {
  const size_t stride = sizeof(pwasm_ctrl_stack_entry_t);
  return pwasm_vec_init(mem_ctx, &(stack->stack), stride);
}

static void
pwasm_ctrl_stack_fini(
  pwasm_ctrl_stack_t * const stack
) {
  pwasm_vec_fini(&(stack->stack));
}

#if 0
static size_t
pwasm_ctrl_stack_get_size(
  const pwasm_ctrl_stack_t * const stack
) {
  return pwasm_vec_get_size(&(stack->stack));
}
#endif /* 0 */

static const void *
pwasm_ctrl_stack_peek_tail(
  const pwasm_ctrl_stack_t * const stack,
  const size_t ofs
) {
  return pwasm_vec_peek_tail(&(stack->stack), ofs);
}

static bool
pwasm_ctrl_stack_pop(
  pwasm_ctrl_stack_t * const stack,
  void * const dst
) {
  // pop final entry, return result
  return pwasm_vec_pop(&(stack->stack), dst);
}

static bool
pwasm_ctrl_stack_popn(
  pwasm_ctrl_stack_t * const stack,
  const size_t num
) {
  // pop entries
  for (size_t i = 0; i < num; i++) {
    if (!pwasm_vec_pop(&(stack->stack), NULL)) {
      return false;
    }
  }

  // return success
  return true;
}

static bool
pwasm_ctrl_stack_push(
  pwasm_ctrl_stack_t * const stack,
  const pwasm_ctrl_stack_entry_t entry
) {
  return pwasm_vec_push(&(stack->stack), 1, &entry, NULL);
}

//
// new interpreter
//

typedef enum {
  PWASM_NEW_INTERP_MOD_TYPE_MOD,
  PWASM_NEW_INTERP_MOD_TYPE_NATIVE,
  PWASM_NEW_INTERP_MOD_TYPE_LAST,
} pwasm_new_interp_mod_type_t;

typedef struct {
  // module name
  pwasm_buf_t name;

  // module type (internal or native)
  pwasm_new_interp_mod_type_t type;

  // references to the u32s vector in the parent interpreter
  pwasm_slice_t funcs;
  pwasm_slice_t globals;
  pwasm_slice_t mems;
  pwasm_slice_t tables;

  union {
    const pwasm_native_t * const native;
    const pwasm_mod_t * const mod;
  };
} pwasm_new_interp_mod_t;

typedef struct {
  // mod offset in parent interpreter
  uint32_t mod_ofs;

  // func offset in parent mod
  uint32_t func_ofs;
} pwasm_new_interp_func_t;

typedef struct {
  pwasm_env_mem_t * const mem; // memory pointer
  const size_t ofs; // absolute offset, in bytes
  const size_t size; // size, in bytes
} pwasm_new_interp_mem_chunk_t;

/*
 * static void
 * pwasm_new_interp_dump_mem_chunk(
 *   const pwasm_new_interp_mem_chunk_t chunk
 * ) {
 *   D("{ .mem = %p, .ofs = %zu, .size = %zu }", (void*) chunk.mem, chunk.ofs, chunk.size);
 * }
 */

typedef struct {
  // mod offset in parent interpreter
  uint32_t mod_ofs;

  // table offset in parent mod
  uint32_t table_ofs;

  // limits from initial mod
  const pwasm_limits_t limits;

  // vec of u32 values
  uint32_t *vals;

  // array of u64s indicating set elements
  uint64_t *masks;

  // maximum number of elements
  size_t max_vals;
} pwasm_new_interp_table_t;

static pwasm_new_interp_table_t
pwasm_new_interp_table_init(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const uint32_t table_ofs,
  pwasm_limits_t limits
) {
  (void) env;
  return (pwasm_new_interp_table_t) {
    .mod_ofs    = mod_ofs,
    .table_ofs  = table_ofs,
    .limits     = limits,
    .vals       = NULL,
    .masks      = NULL,
    .max_vals   = 0,
  };
}

static void
pwasm_new_interp_table_fini(
  pwasm_env_t * const env,
  pwasm_new_interp_table_t * const table
) {
  if (!table->max_vals) {
    return;
  }

  if (table->max_vals > 0) {
    // free memory and masks
    pwasm_realloc(env->mem_ctx, table->vals, 0);
    pwasm_realloc(env->mem_ctx, table->masks, 0);
    table->vals = NULL;
    table->masks = NULL;
    table->max_vals = 0;
  }
}

static bool
pwasm_new_interp_table_grow(
  pwasm_env_t * const env,
  pwasm_new_interp_table_t * const table,
  const size_t src_new_len
) {
  // check existing capacity
  if (src_new_len <= table->max_vals) {
    return true;
  }

  // clamp to minimum size
  const size_t new_len = (table->limits.min > src_new_len) ? table->limits.min : src_new_len;

  // check table maximum limit
  if (table->limits.has_max && src_new_len > table->limits.max) {
    D("src_new_len = %zu, max = %u", src_new_len, table->limits.max);
    pwasm_env_fail(env, "length greater than table limit");
    return false;
  }

  // reallocate vals, check for error
  uint32_t *tmp_vals = pwasm_realloc(env->mem_ctx, table->vals, new_len * sizeof(uint32_t));
  if (!tmp_vals && new_len > 0) {
    pwasm_env_fail(env, "vals pwasm_realloc() failed");
    return false;
  }

  // reallocate masks, check for error
  const size_t new_num_masks = (new_len / 64) + ((new_len & 0x3F) ? 1 : 0);
  uint64_t *tmp_masks = pwasm_realloc(env->mem_ctx, table->masks, new_num_masks * sizeof(uint64_t));
  if (!tmp_vals && new_len > 0) {
    pwasm_env_fail(env, "masks pwasm_realloc() failed");
    return false;
  }

  // clear masks
  for (size_t i = table->max_vals; i < new_len; i++) {
    tmp_masks[i >> 6] &= ~(((uint64_t) 1) << (i & 0x3F));
  }

  // update vals, masks, and max_len
  table->vals = tmp_vals;
  table->masks = tmp_masks;
  table->max_vals = new_len;

  // return success
  return true;
}

static bool
pwasm_new_interp_table_set(
  pwasm_env_t * const env,
  pwasm_new_interp_table_t * const table,
  const size_t ofs,
  const uint32_t * const vals,
  const size_t num_vals
) {
  if (!pwasm_new_interp_table_grow(env, table, ofs + num_vals)) {
    // return failure
    return false;
  }

  // copy values
  memcpy(table->vals + ofs, vals, num_vals * sizeof(uint32_t));

  // set masks
  for (size_t i = ofs; i < ofs + num_vals; i++) {
    table->masks[i >> 6] |= ((uint64_t) 1) << (i & 0x3F);
  }

  // return success
  return true;
}

/**
 * Get element (u32) from table.
 *
 * At the moment the only supported types are function references, so
 * this returns a u32 offset into the interpreters funcs table.
 */
static bool
pwasm_new_interp_table_get_elem(
  pwasm_env_t * const env,
  pwasm_new_interp_table_t * const table,
  const size_t ofs,
  uint32_t * const ret_val
) {
  // check element offset
  if (ofs >= table->max_vals) {
    D("ofs = %zu, table->max_vals = %zu", ofs, table->max_vals);
    // log error, return failure
    pwasm_env_fail(env, "table element offset out of bounds");
    return false;
  }

  // check element mask
  const bool set = table->masks[ofs >> 6] & ((uint64_t) 1) << (ofs & 0x3F);
  if (!set) {
    // log error, return failure
    pwasm_env_fail(env, "table element is not set");
    return false;
  }

  if (ret_val) {
    // write value to destination
    *ret_val = table->vals[ofs];
  }

  // return success
  return true;
}

#define PWASM_NEW_INTERP_VECS \
  PWASM_NEW_INTERP_VEC(u32s, uint32_t) \
  PWASM_NEW_INTERP_VEC(mods, pwasm_new_interp_mod_t) \
  PWASM_NEW_INTERP_VEC(funcs, pwasm_new_interp_func_t) \
  PWASM_NEW_INTERP_VEC(globals, pwasm_env_global_t) \
  PWASM_NEW_INTERP_VEC(mems, pwasm_env_mem_t) \
  PWASM_NEW_INTERP_VEC(tables, pwasm_new_interp_table_t)

typedef struct {
  #define PWASM_NEW_INTERP_VEC(NAME, TYPE) pwasm_vec_t NAME;
  PWASM_NEW_INTERP_VECS
  #undef PWASM_NEW_INTERP_VEC
  pwasm_ctrl_stack_t ctrl_stack;
} pwasm_new_interp_t;

typedef struct {
  pwasm_env_t * const env;
  pwasm_new_interp_mod_t * const mod;

  // memory for this frame
  const uint32_t mem_id;

  // function parameters
  const pwasm_slice_t params;

  // offset and length of locals on the stack
  // NOTE: the offset and length include function parameters
  pwasm_slice_t locals;
} pwasm_new_interp_frame_t;

static bool
pwasm_new_interp_init(
  pwasm_env_t * const env
) {
  const size_t interp_size = sizeof(pwasm_new_interp_t);
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // allocate interpreter data store
  pwasm_new_interp_t *interp = pwasm_realloc(mem_ctx, NULL, interp_size);
  if (!interp) {
    // log error, return failure
    D("pwasm_realloc() failed (size = %zu)", interp_size);
    pwasm_env_fail(env, "interpreter memory allocation failed");
    return false;
  }

  #define PWASM_NEW_INTERP_VEC(NAME, TYPE) \
    /* allocate vector, check for error */ \
    if (!pwasm_vec_init(mem_ctx, &(interp->NAME), sizeof(TYPE))) { \
      /* log error, return failure */ \
      D("pwasm_vec_init() failed (stride = %zu)", sizeof(TYPE)); \
      pwasm_env_fail(env, "interpreter " #NAME " vector init failed"); \
      return false; \
    }
  PWASM_NEW_INTERP_VECS
  #undef PWASM_NEW_INTERP_VEC

  // init control stack, check for error
  if (!pwasm_ctrl_stack_init(&(interp->ctrl_stack), mem_ctx)) {
    // return failure
    return false;
  }

  // save interpreter, return success
  env->env_data = interp;
  return true;
}

static void
pwasm_new_interp_fini_tables(
  pwasm_env_t * const env
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const vec = &(interp->tables);
  pwasm_new_interp_table_t *rows = (pwasm_new_interp_table_t*) pwasm_vec_get_data(vec);
  const size_t num_rows = pwasm_vec_get_size(vec);

  for (size_t i = 0; i < num_rows; i++) {
    pwasm_new_interp_table_fini(env, rows + i);
  }
}

static void
pwasm_new_interp_fini(
  pwasm_env_t * const env
) {
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // get interpreter data
  pwasm_new_interp_t *data = env->env_data;
  if (!data) {
    return;
  }

  // finalize tables
  pwasm_new_interp_fini_tables(env);

  // fini control stack, check for error
  pwasm_ctrl_stack_fini(&(data->ctrl_stack));

  // free vectors
  #define PWASM_NEW_INTERP_VEC(NAME, TYPE) pwasm_vec_fini(&(data->NAME));
  PWASM_NEW_INTERP_VECS
  #undef PWASM_NEW_INTERP_VEC

  // free backing data
  pwasm_realloc(mem_ctx, data, 0);
  env->env_data = NULL;
}

/**
 * Given an environment and a table handle, return a pointer to the
 * table instance.
 *
 * Returns `NULL` if the table handle is invalid.
 */
static pwasm_new_interp_table_t *
pwasm_new_interp_get_table(
  pwasm_env_t * const env,
  const uint32_t table_id
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const vec = &(interp->tables);
  const pwasm_new_interp_table_t * const rows = pwasm_vec_get_data(vec);
  const size_t num_rows = pwasm_vec_get_size(vec);

  // check that table_id is in bounds
  if (!table_id || table_id > num_rows) {
    // log error, return failure
    D("bad table_id: %u, num_rows = %zu", table_id, num_rows);
    pwasm_env_fail(env, "interpreter table index out of bounds");
    return NULL;
  }

  // return pointer to table
  return (pwasm_new_interp_table_t*) rows + (table_id - 1);
}

static bool
pwasm_new_interp_push_u32s(
  pwasm_env_t * const env,
  const size_t ofs,
  const size_t len
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const u32s = &(interp->u32s);
  const pwasm_slice_t slice = { ofs, len };

  uint32_t ids[PWASM_BATCH_SIZE];
  size_t num = 0;

  for (size_t i = 0; i < slice.len; i++) {
    ids[num++] = slice.ofs + i;

    if (num == LEN(ids)) {
      // clear count
      num = 0;

      // append results, check for error
      if (!pwasm_vec_push(u32s, LEN(ids), ids, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append offsets failed");
        return false;
      }
    }
  }

  if (num > 0) {
    // append results, check for error
    if (!pwasm_vec_push(u32s, num, ids, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining offsets failed");
      return false;
    }
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_add_native_funcs(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_native_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->funcs);
  const size_t dst_ofs = pwasm_vec_get_size(dst);
  const size_t u32s_ofs = pwasm_vec_get_size(&(interp->u32s));

  pwasm_new_interp_func_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_funcs; i++) {
    tmp[tmp_ofs++] = (pwasm_new_interp_func_t) {
      .mod_ofs  = mod_ofs,
      .func_ofs = i,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append native functions failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native functions failed");
      return false;
    }
  }

  // add IDs
  if (!pwasm_new_interp_push_u32s(env, u32s_ofs, mod->num_funcs)) {
    // return failure
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = dst_ofs,
    .len = mod->num_funcs,
  };

  // return success
  return true;
}

static bool
pwasm_new_interp_add_native_globals(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_native_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->globals);
  const size_t dst_ofs = pwasm_vec_get_size(dst);
  const size_t u32s_ofs = pwasm_vec_get_size(&(interp->u32s));
  (void) mod_ofs;

  pwasm_env_global_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_globals; i++) {
    tmp[tmp_ofs++] = (pwasm_env_global_t) {
      .type = mod->globals[i].type,
      .val  = mod->globals[i].val,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append native globals failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native globals failed");
      return false;
    }
  }

  // add IDs
  if (!pwasm_new_interp_push_u32s(env, u32s_ofs, mod->num_globals)) {
    // return failure
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = dst_ofs,
    .len = mod->num_globals,
  };

  // return success
  return true;
}

static bool
pwasm_new_interp_add_native_mems(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_native_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->mems);
  const size_t dst_ofs = pwasm_vec_get_size(dst);
  (void) mod_ofs;

  pwasm_env_mem_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_mems; i++) {
    tmp[tmp_ofs++] = (pwasm_env_mem_t) {
      .buf    = mod->mems[i].buf,
      .limits = mod->mems[i].limits,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append native mems failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native mems failed");
      return false;
    }
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = dst_ofs,
    .len = mod->num_mems,
  };

  // return success
  return true;
}

static uint32_t
pwasm_new_interp_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const size_t mod_ofs = pwasm_vec_get_size(&(interp->mods));

  // add native functions, check for error
  pwasm_slice_t funcs;
  if (!pwasm_new_interp_add_native_funcs(env, mod_ofs, mod, &funcs)) {
    // return failure
    return 0;
  }

  // add native globals, check for error
  pwasm_slice_t globals;
  if (!pwasm_new_interp_add_native_globals(env, mod_ofs, mod, &globals)) {
    // return failure
    return 0;
  }

  // add native mems, check for error
  pwasm_slice_t mems;
  if (!pwasm_new_interp_add_native_mems(env, mod_ofs, mod, &mems)) {
    // return failure
    return 0;
  }

  // build row
  const pwasm_new_interp_mod_t interp_mod = {
    .type     = PWASM_NEW_INTERP_MOD_TYPE_NATIVE,
    .name     = pwasm_buf_str(name),
    .native   = mod,

    .funcs    = funcs,
    .globals  = globals,
    .mems     = mems,
  };

  // append native mod, check for error
  if (!pwasm_vec_push(&(interp->mods), 1, &interp_mod, NULL)) {
    // log error, return failure
    pwasm_env_fail(env, "append native mod failed");
    return 0;
  }

  // convert offset to ID by adding 1
  return mod_ofs + 1;
}

/*
 * Resolve imports of a given type.
 *
 * On success, a slice of import IDs is stored in +ret+, and this
 * function returns true.
 *
 * Returns false on error.
 */
static bool
pwasm_new_interp_add_mod_imports(
  pwasm_env_t * const env,
  const pwasm_mod_t * const mod,
  const pwasm_import_type_t type,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const u32s = &(interp->u32s);
  const size_t ret_ofs = pwasm_vec_get_size(u32s);

  uint32_t ids[PWASM_BATCH_SIZE];
  size_t num_ids = 0;

  // loop over imports and resolve each one
  for (size_t i = 0; i < mod->num_imports; i++) {
    // get import, check type
    const pwasm_import_t import = mod->imports[i];
    if (import.type != type) {
      continue;
    }

    const pwasm_buf_t mod_buf = {
      .ptr = mod->bytes + import.module.ofs,
      .len = import.module.len,
    };

    // find mod, check for error
    const uint32_t mod_id = pwasm_env_find_mod(env, mod_buf);
    if (!mod_id) {
      // return failure
      return false;
    }

    const pwasm_buf_t name_buf = {
      .ptr = mod->bytes + import.name.ofs,
      .len = import.name.len,
    };

    // find import ID, check for error
    const uint32_t id = pwasm_env_find_import(env, mod_id, import.type, name_buf);
    if (!id) {
      // return failure
      return false;
    }

    // add item to results, increment count
    ids[num_ids++] = id;

    if (num_ids == LEN(ids)) {
      // clear count
      num_ids = 0;

      // append results, check for error
      if (!pwasm_vec_push(u32s, LEN(ids), ids, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append import ids failed");
        return false;
      }
    }
  }

  if (num_ids > 0) {
    // append remaining results
    if (!pwasm_vec_push(u32s, num_ids, ids, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native imports failed");
      return false;
    }
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = ret_ofs,
    .len = pwasm_vec_get_size(u32s) - ret_ofs,
  };

  // return success
  return true;
}

static bool
pwasm_new_interp_add_mod_funcs(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->funcs);
  const size_t funcs_ofs = pwasm_vec_get_size(dst);

  // add imported functions, check for error
  pwasm_slice_t imports;
  if (!pwasm_new_interp_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_FUNC, &imports)) {
    return false;
  }

  pwasm_new_interp_func_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_funcs; i++) {
    tmp[tmp_ofs++] = (pwasm_new_interp_func_t) {
      .mod_ofs  = mod_ofs,
      .func_ofs = i,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append functions failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining functions failed");
      return false;
    }
  }

  if (!pwasm_new_interp_push_u32s(env, funcs_ofs, mod->num_funcs)) {
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_funcs,
  };

  // return success
  return true;
}

static bool
pwasm_new_interp_add_mod_globals(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->globals);
  const size_t globals_ofs = pwasm_vec_get_size(dst);
  (void) mod_ofs;

  // add imported globals, check for error
  pwasm_slice_t imports;
  if (!pwasm_new_interp_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_GLOBAL, &imports)) {
    return false;
  }

  pwasm_env_global_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_globals; i++) {
    tmp[tmp_ofs++] = (pwasm_env_global_t) {
      .type  = mod->globals[i].type,
      // .val = // FIXME: uninitialized
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append globals failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining globals failed");
      return false;
    }
  }

  // add IDs
  if (!pwasm_new_interp_push_u32s(env, globals_ofs, mod->num_globals)) {
    // return failure
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_globals,
  };

  // return success
  return true;
}

static bool
pwasm_new_interp_add_mod_mems(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->mems);
  const size_t mems_ofs = pwasm_vec_get_size(dst);
  (void) mod_ofs;

  // add imported mems, check for error
  pwasm_slice_t imports;
  if (!pwasm_new_interp_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_MEM, &imports)) {
    return false;
  }

  pwasm_env_mem_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_mems; i++) {
    // allocate buffer
    const size_t num_bytes = mod->mems[i].min * PWASM_PAGE_SIZE;
    uint8_t * const ptr = pwasm_realloc(env->mem_ctx, NULL, num_bytes);
    if (!ptr && num_bytes) {
      // log error, return failure
      pwasm_env_fail(env, "allocate memory buffer failed");
      return false;
    }

    tmp[tmp_ofs++] = (pwasm_env_mem_t) {
      .limits = mod->mems[i],
      .buf = (pwasm_buf_t) {
        .ptr = ptr,
        .len = num_bytes,
      },
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append mems failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining mems failed");
      return false;
    }
  }

  if (!pwasm_new_interp_push_u32s(env, mems_ofs, mod->num_mems)) {
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_mems,
  };

  // return success
  return true;
}

static bool
pwasm_new_interp_add_mod_tables(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->tables);
  pwasm_vec_t * const u32s = &(interp->u32s);

  // add imported tables, check for error
  pwasm_slice_t imports = { 0, 0 };
  if (!pwasm_new_interp_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_TABLE, &imports)) {
    return false;
  }

  uint32_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_tables; i++) {
    // init table
    pwasm_new_interp_table_t table = pwasm_new_interp_table_init(env, mod_ofs, i, mod->tables[i].limits);

    // append initialized table to interpreter, check for error
    size_t tmp_pos;
    if (!pwasm_vec_push(dst, 1, &table, &tmp_pos)) {
      // log error, return failure
      pwasm_env_fail(env, "append tables failed");
      return false;
    }

    // append to table offsets, increment count
    tmp[tmp_ofs] = tmp_pos;
    tmp_ofs++;

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append IDs, check for error
      if (!pwasm_vec_push(u32s, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append table ids failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // flush IDs, check for error
    if (!pwasm_vec_push(u32s, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining table IDs failed");
      return false;
    }
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_tables,
  };

  // return success
  return true;
}

// forward declarations
static bool pwasm_new_interp_eval_expr(
  pwasm_new_interp_frame_t frame,
  const pwasm_slice_t
);

static bool pwasm_new_interp_call(pwasm_env_t *, const uint32_t);

static bool
pwasm_new_interp_init_globals(
  pwasm_new_interp_frame_t frame
) {
  pwasm_new_interp_t * const interp = frame.env->env_data;
  pwasm_env_global_t *env_globals = (pwasm_env_global_t*) pwasm_vec_get_data(&(interp->globals));
  pwasm_stack_t * const stack = frame.env->stack;
  const pwasm_global_t * const mod_globals = frame.mod->mod->globals;
  const uint32_t * interp_u32s = (uint32_t*) pwasm_vec_get_data(&(interp->u32s)) + frame.mod->globals.ofs;
  const size_t num_globals = frame.mod->mod->num_globals;
  const pwasm_val_t zero = { .i64 = 0 };

  for (size_t i = 0; i < num_globals; i++) {
    // clear stack
    stack->pos = 0;

    // evaluate init
    if (!pwasm_new_interp_eval_expr(frame, mod_globals[i].expr)) {
      // return failure
      return false;
    }

    // get destination offset, save value to global
    const uint32_t ofs = interp_u32s[i];
    env_globals[ofs].val = stack->pos ? stack->ptr[0] : zero;
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_init_elem_funcs(
  pwasm_new_interp_frame_t frame,
  const uint32_t ofs,
  const pwasm_elem_t elem
) {
  // get table and mod to interpreter func ID map
  pwasm_new_interp_t * const interp = frame.env->env_data;
  const uint32_t *u32s = pwasm_vec_get_data(&(interp->u32s));
  const uint32_t table_ofs = u32s[frame.mod->tables.ofs + elem.table_id];
  D("frame.mod->tables.ofs = %zu, elem.table_id = %u, table_ofs = %u", frame.mod->tables.ofs, elem.table_id, table_ofs);
  const uint32_t * const funcs = u32s + frame.mod->funcs.ofs;
  pwasm_new_interp_table_t * const table = pwasm_new_interp_get_table(frame.env, table_ofs + 1);
  if (!table) {
    return false;
  }

  uint32_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < elem.funcs.len; i++) {
    // remap function id from module ID to interpreter ID
    const uint32_t func_id = frame.mod->mod->u32s[elem.funcs.ofs + i];
    tmp[tmp_ofs++] = funcs[func_id];

    if (tmp_ofs == LEN(tmp)) {
      // get destination offset
      const size_t dst_ofs = ofs + i - LEN(tmp);

      // set table elements, check for error
      if (!pwasm_new_interp_table_set(frame.env, table, dst_ofs, tmp, LEN(tmp))) {
        // return failure
        return false;
      }

      // reset offset
      tmp_ofs = 0;
    }
  }

  if (tmp_ofs > 0) {
    // get destination offset
    const size_t dst_ofs = ofs + elem.funcs.len - tmp_ofs;

    // flush table elements, check for error
    if (!pwasm_new_interp_table_set(frame.env, table, dst_ofs, tmp, tmp_ofs)) {
      // return failure
      return false;
    }
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_init_elems(
  pwasm_new_interp_frame_t frame
) {
  const pwasm_elem_t * const elems = frame.mod->mod->elems;
  const size_t num_elems = frame.mod->mod->num_elems;
  pwasm_stack_t * const stack = frame.env->stack;

  // init table elements
  for (size_t i = 0; i < num_elems; i++) {
    const pwasm_elem_t elem = elems[i];

    // evaluate offset expression, check for error
    stack->pos = 0;
    if (!pwasm_new_interp_eval_expr(frame, elem.expr)) {
      // return failure
      return false;
    }

    // check that const expr has at least one result
    // FIXME: should this be done in check()?
    if (!stack->pos) {
      pwasm_env_fail(frame.env, "constant expression must return table offset");
      return false;
    }

    // get destination offset
    const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

    // populate table elements, check for error
    if (!pwasm_new_interp_init_elem_funcs(frame, ofs, elem)) {
      // return failure
      return false;
    }
  }

  // return success
  return true;
}

// forward reference
static pwasm_env_mem_t *pwasm_new_interp_get_mem(pwasm_env_t * const, const uint32_t);

static bool
pwasm_new_interp_init_segments(
  pwasm_new_interp_frame_t frame
) {
  pwasm_new_interp_t * const interp = frame.env->env_data;
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));
  const pwasm_segment_t * const segments = frame.mod->mod->segments;
  const size_t num_segments = frame.mod->mod->num_segments;
  pwasm_stack_t * const stack = frame.env->stack;

  // init memory elements
  for (size_t i = 0; i < num_segments; i++) {
    const pwasm_segment_t segment = segments[i];

    // get interpreter memory ID
    const uint32_t mem_id = u32s[frame.mod->mems.ofs + segment.mem_id];

    // get memory, check for error
    pwasm_env_mem_t * const mem = pwasm_new_interp_get_mem(frame.env, mem_id);
    if (!mem) {
      // return failure
      return false;
    }

    // evaluate offset expression, check for error
    stack->pos = 0;
    if (!pwasm_new_interp_eval_expr(frame, segment.expr)) {
      // return failure
      return false;
    }

    // check that const expr has at least one result
    if (!stack->pos) {
      pwasm_env_fail(frame.env, "constant expression must return segment offset");
      return false;
    }

    // get destination offset
    const uint32_t dst_ofs = stack->ptr[stack->pos - 1].i32;

    // check destination length
    // FIXME: should we resize here?
    if (dst_ofs + segment.data.len > mem->buf.len) {
      pwasm_env_fail(frame.env, "segment destination out of bounds");
      return false;
    }

    // D("mem = %u, ofs = %u, len = %zu", mem_id, dst_ofs, segment.data.len);

    // get dst, src pointers
    uint8_t * const dst = (uint8_t*) mem->buf.ptr + dst_ofs;
    const uint8_t * const src = frame.mod->mod->bytes + segment.data.ofs;

    // copy data
    memcpy(dst, src, segment.data.len);
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_init_start(
  pwasm_new_interp_frame_t frame
) {
  (void) frame;

  // check for start function
  if (!frame.mod->mod->has_start) {
    // return success
    return true;
  }

  // get start function ID in mod, map to interpreter func ID
  pwasm_new_interp_t * const interp = frame.env->env_data;
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));
  const uint32_t start = frame.mod->mod->start;
  const uint32_t func_id = u32s[frame.mod->funcs.ofs + start] + 1;

  // call start function, return result
  return pwasm_new_interp_call(frame.env, func_id);
}

static uint32_t
pwasm_new_interp_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const size_t mod_ofs = pwasm_vec_get_size(&(interp->mods));

  // add mod funcs, check for error
  pwasm_slice_t funcs;
  if (!pwasm_new_interp_add_mod_funcs(env, mod_ofs, mod, &funcs)) {
    // return failure
    return 0;
  }

  // add mod globals, check for error
  pwasm_slice_t globals;
  if (!pwasm_new_interp_add_mod_globals(env, mod_ofs, mod, &globals)) {
    // return failure
    return 0;
  }

  // add mod mems, check for error
  pwasm_slice_t mems;
  if (!pwasm_new_interp_add_mod_mems(env, mod_ofs, mod, &mems)) {
    // return failure
    return 0;
  }

  // add mod tables, check for error
  pwasm_slice_t tables;
  if (!pwasm_new_interp_add_mod_tables(env, mod_ofs, mod, &tables)) {
    // return failure
    return 0;
  }

  // build mod instance
  pwasm_new_interp_mod_t interp_mod = {
    .type     = PWASM_NEW_INTERP_MOD_TYPE_MOD,
    .name     = pwasm_buf_str(name),
    .mod      = mod,

    .funcs    = funcs,
    .globals  = globals,
    .mems     = mems,
    .tables   = tables,
  };

  // append mod, check for error
  if (!pwasm_vec_push(&(interp->mods), 1, &interp_mod, NULL)) {
    // log error, return failure
    pwasm_env_fail(env, "append mod failed");
    return 0;
  }

  // set up a temporary frame to init globals, tables, and mems
  pwasm_new_interp_frame_t frame = {
    .env = env,
    .mod = &interp_mod,
  };

  // init globals, mems, tables, and start (in that order)
  // source: https://webassembly.github.io/spec/core/exec/modules.html#exec-instantiation)

  // init globals, check for error
  if (!pwasm_new_interp_init_globals(frame)) {
    // return failure
    return 0;
  }

  // init tables, check for error
  if (!pwasm_new_interp_init_elems(frame)) {
    // return failure
    return 0;
  }

  // init segments, check for error
  if (!pwasm_new_interp_init_segments(frame)) {
    // return failure
    return 0;
  }

  // call start func, check for error
  if (!pwasm_new_interp_init_start(frame)) {
    // return failure
    return 0;
  }

  // return success, convert offset to ID by adding 1
  return mod_ofs + 1;
}

static const pwasm_mod_t *
pwasm_new_interp_get_mod(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_new_interp_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  const bool ok = (mod_id && mod_id <= num_rows);
  return ok ? rows[mod_id - 1].mod : NULL;
}

static const pwasm_buf_t *
pwasm_new_interp_get_mod_name(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_new_interp_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  const bool ok = (mod_id && mod_id <= num_rows);
  return ok ? &(rows[mod_id - 1].name) : NULL;
}

static uint32_t
pwasm_new_interp_find_mod(
  pwasm_env_t * const env,
  const pwasm_buf_t name
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_new_interp_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  for (size_t i = 0; i < num_rows; i++) {
    if (
      (rows[i].name.len == name.len) &&
      !memcmp(name.ptr, rows[i].name.ptr, name.len)
    ) {
      // return offset + 1 (prevent zero IDs)
      return i + 1;
    }
  }

  // log error, return failure
  pwasm_env_fail(env, "module not found");
  return 0;
}

static uint32_t
pwasm_new_interp_find_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_vec_t * const vec = &(interp->mods);
  const pwasm_new_interp_mod_t * const mods = pwasm_vec_get_data(vec);
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));

  // check mod_id
  if (mod_id > pwasm_vec_get_size(&(interp->mods))) {
    pwasm_env_fail(env, "invalid mod ID");
    return 0;
  }

  // get mod
  const pwasm_new_interp_mod_t mod = mods[mod_id - 1];

  switch (mod.type) {
  case PWASM_NEW_INTERP_MOD_TYPE_MOD:
    for (size_t i = 0; i < mod.mod->num_exports; i++) {
      const pwasm_export_t row = mod.mod->exports[i];

      if (
        (row.type == PWASM_IMPORT_TYPE_FUNC) &&
        (row.name.len == name.len) &&
        !memcmp(mod.mod->bytes + row.name.ofs, name.ptr, name.len)
      ) {
        // return offset + 1 (prevent zero IDs)
        return u32s[mod.funcs.ofs + row.id] + 1;
      }
    }

    break;
  case PWASM_NEW_INTERP_MOD_TYPE_NATIVE:
    for (size_t i = 0; i < mod.native->num_funcs; i++) {
      const pwasm_native_func_t row = mod.native->funcs[i];
      const pwasm_buf_t row_buf = pwasm_buf_str(row.name);

      if (
        row_buf.ptr &&
        (row_buf.len == name.len) &&
        !memcmp(row_buf.ptr, name.ptr, name.len)
      ) {
        D("u32s = %p, mod->funcs.ofs = %zu, i = %zu", (void*) u32s, mod.funcs.ofs, i);
        // return offset + 1 (prevent zero IDs)
        return u32s[mod.funcs.ofs + i] + 1;
      }
    }

    break;
  default:
    // log error, return failure
    pwasm_env_fail(env, "unknown module type (bug?)");
    return 0;
  }

  // log error, return failure
  pwasm_env_fail(env, "function not found");
  return 0;
}

static uint32_t
pwasm_new_interp_find_mem(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_vec_t * const vec = &(interp->mods);
  const pwasm_new_interp_mod_t * const mods = pwasm_vec_get_data(vec);
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));

  // check mod_id
  if (mod_id > pwasm_vec_get_size(&(interp->mods))) {
    pwasm_env_fail(env, "invalid mod ID");
    return 0;
  }

  // get mod
  const pwasm_new_interp_mod_t * const mod = mods + (mod_id - 1);

  switch (mod->type) {
  case PWASM_NEW_INTERP_MOD_TYPE_MOD:
    for (size_t i = 0; i < mod->mod->num_exports; i++) {
      const pwasm_export_t row = mod->mod->exports[i];

      if (
        (row.type == PWASM_IMPORT_TYPE_MEM) &&
        (row.name.len == name.len) &&
        !memcmp(mod->mod->bytes + row.name.ofs, name.ptr, name.len)
      ) {
        // return offset + 1 (prevent zero IDs)
        return u32s[mod->mems.ofs + row.id] + 1;
      }
    }

    break;
  case PWASM_NEW_INTERP_MOD_TYPE_NATIVE:
    for (size_t i = 0; i < mod->native->num_funcs; i++) {
      const pwasm_native_mem_t row = mod->native->mems[i];
      const pwasm_buf_t row_buf = pwasm_buf_str(row.name);

      if (
        row_buf.ptr &&
        (row_buf.len == name.len) &&
        !memcmp(row_buf.ptr, name.ptr, name.len)
      ) {
        // return offset + 1 (prevent zero IDs)
        return u32s[mod->mems.ofs + i] + 1;
      }
    }

    break;
  default:
    // log error, return failure
    pwasm_env_fail(env, "unknown module type (bug?)");
    return 0;
  }

  // log error, return failure
  pwasm_env_fail(env, "memory not found");
  return 0;
}

static pwasm_env_mem_t *
pwasm_new_interp_get_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_env_mem_t *rows = pwasm_vec_get_data(&(interp->mems));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mems));
  D("num mems: %zu", num_rows);

  // check that mem_id is in bounds
  if (!mem_id || mem_id > num_rows) {
    // log error, return failure
    D("bad mem_id: %u", mem_id);
    pwasm_env_fail(env, "memory index out of bounds");
    return NULL;
  }

  // return pointer to memory
  return (pwasm_env_mem_t*) rows + (mem_id - 1);
}

/*
 * Get the absolute memory offset from the immediate offset and the
 * offset operand.
 */
static bool
pwasm_new_interp_get_mem_chunk(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_new_interp_mem_chunk_t * const ret
) {
  pwasm_env_mem_t * const mem = pwasm_new_interp_get_mem(env, mem_id);
  size_t ofs = in.v_mem.offset + arg_ofs;
  size_t size = pwasm_op_get_num_bytes(in.op);
  const bool ok = mem && size && (ofs + size < mem->buf.len);

  if (!ok) {
    // dissect error
    if (!mem) {
      D("mem_id = %u", mem_id);
      pwasm_env_fail(env, "invalid memory index");
    } else if (!size) {
      D("op = %s", pwasm_op_get_name(in.op));
      pwasm_env_fail(env, "invalid memory instruction");
    } else {
      D("ofs = %zu, size = %zu", ofs, size);
      pwasm_env_fail(env, "invalid memory address");
    }

    // return failure
    return false;
  }

  const pwasm_new_interp_mem_chunk_t tmp = {
    .mem  = mem,
    .ofs  = ofs,
    .size = size,
  };

  // populate result
  memcpy(ret, &tmp, sizeof(pwasm_new_interp_mem_chunk_t));

  // return success
  return true;
}

static bool
pwasm_new_interp_mem_load(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_val_t * const ret_val
) {
  // get memory chunk, check for error
  pwasm_new_interp_mem_chunk_t chunk;
  if (!pwasm_new_interp_get_mem_chunk(env, mem_id, in, arg_ofs, &chunk)) {
    return false;
  }

  // pwasm_new_interp_dump_mem_chunk(chunk);
  // D("load i32 = %u", ((pwasm_val_t*) (chunk.mem->buf.ptr + chunk.ofs))->i32);

  // clear temp val
  pwasm_val_t tmp;
  memset(&tmp, 0, sizeof(pwasm_val_t));

  // copy to temp
  memcpy(&tmp, chunk.mem->buf.ptr + chunk.ofs, chunk.size);

  // copy temp to destination
  memcpy(ret_val, &tmp, sizeof(pwasm_val_t));

  // return success
  return true;
}

static bool
pwasm_new_interp_mem_store(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  const pwasm_val_t val
) {
  // get memory chunk, check for error
  pwasm_new_interp_mem_chunk_t chunk;
  if (!pwasm_new_interp_get_mem_chunk(env, mem_id, in, arg_ofs, &chunk)) {
    return false;
  }

  // pwasm_new_interp_dump_mem_chunk(chunk);
  // D("chunk.size = %zu", chunk.size);

  // copy to result
  memcpy((uint8_t*) chunk.mem->buf.ptr + chunk.ofs, &val, chunk.size);

  // return success
  return true;
}

static bool
pwasm_new_interp_mem_size(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  uint32_t * const ret_val
) {
  // get memory, check for error
  pwasm_env_mem_t * const mem = pwasm_new_interp_get_mem(env, mem_id);
  if (!mem) {
    // return failure
    return false;
  }

  if (ret_val) {
    // save result
    *ret_val = mem->buf.len / PWASM_PAGE_SIZE;
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  // get memory, check for error
  pwasm_env_mem_t * const mem = pwasm_new_interp_get_mem(env, mem_id);
  if (!mem) {
    // return failure (trap)
    return false;
  }

  // get current size, in number of pages
  const uint32_t old_size = mem->buf.len / PWASM_PAGE_SIZE;
  const uint32_t new_size = old_size + grow;

  // check upper bound
  if (mem->limits.has_max && new_size > mem->limits.max) {
    // at this point we save a -1 to the return value pointer to
    // indicate failure, then return true from the function so that it
    // doesn't trap.
    //
    // see https://webassembly.github.io/spec/core/exec/instructions.html#exec-memory-size

    if (ret_val) {
      // return -1 to indicate failure
      *ret_val = -1;
    }

    // return "success"
    return true;
  }

  if (new_size > 0) {
    // get old pointer and number of bytes
    void *old_ptr = (void*) mem->buf.ptr;
    const size_t num_bytes = new_size * PWASM_PAGE_SIZE;

    // resize buffer, check for error
    uint8_t * const ptr = pwasm_realloc(env->mem_ctx, old_ptr, num_bytes);
    if (!ptr && num_bytes) {
      // at this point we save a -1 to the return value pointer to
      // indicate failure, then return true from the function so that it
      // doesn't trap.
      //
      // see https://webassembly.github.io/spec/core/exec/instructions.html#exec-memory-size

      if (ret_val) {
        // return -1 to indicate failure
        *ret_val = -1;
      }

      // return "success"
      return true;
    }

    // update buffer attributes
    mem->buf.ptr = ptr;
    mem->buf.len = num_bytes;
  }

  if (ret_val) {
    // update return value
    *ret_val = old_size;
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_val
) {
  pwasm_new_interp_table_t * const table = pwasm_new_interp_get_table(env, table_id);
  if (!table) {
    return false;
  }
  return pwasm_new_interp_table_get_elem(env, table, elem_ofs, ret_val);
}

static uint32_t
pwasm_new_interp_find_import(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
) {
  switch (type) {
  case PWASM_IMPORT_TYPE_FUNC:
    return pwasm_env_find_func(env, mod_id, name);
  case PWASM_IMPORT_TYPE_MEM:
    return pwasm_env_find_mem(env, mod_id, name);
  case PWASM_IMPORT_TYPE_GLOBAL:
    return pwasm_env_find_global(env, mod_id, name);
  case PWASM_IMPORT_TYPE_TABLE:
    return pwasm_env_find_table(env, mod_id, name);
  default:
    // log error, return failure
    pwasm_env_fail(env, "invalid import type");
    return 0;
  }
}

static inline bool
pwasm_new_interp_check_global(
  pwasm_env_t * const env,
  const uint32_t id
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const size_t num_rows = pwasm_vec_get_size(&(interp->globals));

  if (!id || id > num_rows) {
    // log error, return failure
    D("global index = %u", id);
    pwasm_env_fail(env, "global index out of bounds");
    return false;
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_get_global(
  pwasm_env_t * const env,
  const uint32_t id,
  pwasm_val_t * const ret_val
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_env_global_t *rows = pwasm_vec_get_data(&(interp->globals));

  if (!pwasm_new_interp_check_global(env, id)) {
    return false;
  }

  if (ret_val) {
    // copy value to destination
    *ret_val = rows[id - 1].val;
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_set_global(
  pwasm_env_t * const env,
  const uint32_t id,
  const pwasm_val_t val
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_env_global_t *rows = (pwasm_env_global_t*) pwasm_vec_get_data(&(interp->globals));

  if (!pwasm_new_interp_check_global(env, id)) {
    return false;
  }

  if (!rows[id - 1].type.mutable) {
    pwasm_env_fail(env, "write to immutable global");
    return false;
  }

  // set global value
  rows[id - 1].val = val;

  // return success
  return true;
}

/*
 * Convert an internal global ID to an externally visible global handle.
 *
 * Returns 0 on error.
 */
static uint32_t
pwasm_new_interp_mod_get_global_index(
  pwasm_env_t * const env,
  pwasm_new_interp_mod_t * const mod,
  uint32_t id
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_vec_t * const vec = &(interp->u32s);
  const uint32_t * const u32s = ((uint32_t*) pwasm_vec_get_data(vec)) + mod->globals.ofs;
  return (id < mod->globals.len) ? u32s[id] + 1 : 0;
}

// forward references
static bool pwasm_new_interp_call_func(pwasm_env_t *, pwasm_new_interp_mod_t *, uint32_t);
static bool pwasm_new_interp_call_indirect(pwasm_new_interp_frame_t, pwasm_inst_t, uint32_t);

static bool
pwasm_new_interp_eval_expr(
  pwasm_new_interp_frame_t frame,
  const pwasm_slice_t expr
) {
  pwasm_new_interp_t * const interp = frame.env->env_data;
  pwasm_ctrl_stack_t * const ctrl_stack = &(interp->ctrl_stack);
  pwasm_stack_t * const stack = frame.env->stack;
  const pwasm_inst_t * const insts = frame.mod->mod->insts + expr.ofs;

  size_t ctrl_depth = 0;

  // D("expr = { .ofs = %zu, .len = %zu }, num_insts = %zu", expr.ofs, expr.len, frame.mod->num_insts);

  for (size_t i = 0; i < expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    // D("0x%02X %s", in.op, pwasm_op_get_name(in.op));

    switch (in.op) {
    case PWASM_OP_UNREACHABLE:
      // FIXME: raise trap?
      pwasm_env_fail(frame.env, "unreachable instruction reached");
      return false;
    case PWASM_OP_NOP:
      // do nothing
      break;
    case PWASM_OP_BLOCK:
      {
        // get block.params.size, check for error
        size_t num_params;
        if (!pwasm_block_type_params_get_size(frame.mod->mod, in.v_block.block_type, &num_params)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "block: get num block params failed");
          return false;
        }

        // create control stack entry
        const pwasm_ctrl_stack_entry_t ctrl_entry = {
          .type   = CTRL_BLOCK,
          .depth  = stack->pos - num_params,
          .ofs    = i,
        };

        // push control stack entry, check for error
        if (!pwasm_ctrl_stack_push(ctrl_stack, ctrl_entry)) {
          pwasm_env_fail(frame.env, "block: ctrl_stack_push failed");
          return false;
        }

        // increment control depth
        ctrl_depth++;
      }

      break;
    case PWASM_OP_LOOP:
      {
        // get block.params.size, check for error
        size_t num_params;
        if (!pwasm_block_type_params_get_size(frame.mod->mod, in.v_block.block_type, &num_params)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "block: get num block params failed");
          return false;
        }

        // create control stack entry
        const pwasm_ctrl_stack_entry_t ctrl_entry = {
          .type   = CTRL_LOOP,
          .depth  = stack->pos - num_params,
          .ofs    = i,
        };

        // push control stack entry, check for error
        if (!pwasm_ctrl_stack_push(ctrl_stack, ctrl_entry)) {
          pwasm_env_fail(frame.env, "block: ctrl_stack_push failed");
          return false;
        }

        // increment control depth
        ctrl_depth++;
      }

      break;
    case PWASM_OP_IF:
      {
        // pop last value from value stack
        const uint32_t tail = stack->ptr[--stack->pos].i32;

        // get else/end offset
        const size_t else_ofs = in.v_block.else_ofs ? in.v_block.else_ofs : in.v_block.end_ofs;

        // get block.params.size, check for error
        size_t num_params;
        if (!pwasm_block_type_params_get_size(frame.mod->mod, in.v_block.block_type, &num_params)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "block: get num block params failed");
          return false;
        }

        // create control stack entry
        const pwasm_ctrl_stack_entry_t ctrl_entry = {
          .type   = CTRL_IF,
          .depth  = stack->pos - num_params,
          .ofs    = i,
        };

        // push control stack entry, check for error
        if (!pwasm_ctrl_stack_push(ctrl_stack, ctrl_entry)) {
          pwasm_env_fail(frame.env, "block: ctrl_stack_push failed");
          return false;
        }

        // increment control depth
        ctrl_depth++;

        // increment instruction pointer
        i += tail ? 0 : else_ofs;
      }

      break;
    case PWASM_OP_ELSE:
      // skip to end inst
      i += insts[i].v_block.end_ofs - 1;

      break;
    case PWASM_OP_END:
      if (ctrl_depth) {
        // pop control stack, check for error
        pwasm_ctrl_stack_entry_t ctrl_tail;
        if (!pwasm_ctrl_stack_pop(ctrl_stack, &ctrl_tail)) {
          pwasm_env_fail(frame.env, "end: ctrl_stack pop failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth--;

        // get mod, block type
        const pwasm_mod_t * const mod = frame.mod->mod;
        const int32_t block_type = insts[ctrl_tail.ofs].v_block.block_type;

        // get block type result count, check for error
        size_t num_results;
        if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "end: get block num_results failed");
          return false;
        }

        // pop results
        for (size_t j = 0; j < num_results; j++) {
          // calculate stack source and destination offsets
          const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
          const size_t dst_ofs = ctrl_tail.depth + j;
          stack->ptr[dst_ofs] = stack->ptr[src_ofs];
        }

        // reset value stack
        stack->pos = ctrl_tail.depth + num_results;
      } else {
        // return success
        // FIXME: is this correct?
        return true;
      }

      break;
    case PWASM_OP_BR:
      {
        // pop control stack, check for error
        if (!pwasm_ctrl_stack_popn(ctrl_stack, in.v_index)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "br: ctrl_stack_popn failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth -= in.v_index;
        if (!ctrl_depth) {
          // return success
          // FIXME: is this correct?
          return true;
        }

        // get control stack tail, check for error
        const pwasm_ctrl_stack_entry_t *ctrl_tail = pwasm_ctrl_stack_peek_tail(ctrl_stack, 0);
        if (!ctrl_tail) {
          pwasm_env_fail(frame.env, "br: ctrl_stack_peek_tail failed");
          return false;
        }

        if (ctrl_tail->type == CTRL_LOOP) {
          // reset control stack
          i = ctrl_tail->ofs;
          stack->pos = ctrl_tail->depth;
        } else {
          // get mod, block type
          const pwasm_mod_t * const mod = frame.mod->mod;
          const int32_t block_type = insts[ctrl_tail->ofs].v_block.block_type;

          // get block type result count, check for error
          size_t num_results;
          if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: get block num_results failed");
            return false;
          }

          // pop results
          for (size_t j = 0; j < num_results; j++) {
            // calculate stack source and destination offsets
            const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
            const size_t dst_ofs = ctrl_tail->depth + 1 + j;
            stack->ptr[dst_ofs] = stack->ptr[src_ofs];
          }

          // reset value stack
          stack->pos = ctrl_tail->depth + num_results;

          // pop control stack, check for error
          if (!pwasm_ctrl_stack_pop(ctrl_stack, NULL)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: ctrl_stack_pop failed");
            return false;
          }

          // decriment control stack depth
          ctrl_depth--;
        }
      }

      break;
    case PWASM_OP_BR_IF:
      if (stack->ptr[--stack->pos].i32) {
        // pop control stack, check for error
        if (!pwasm_ctrl_stack_popn(ctrl_stack, in.v_index)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_popn failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth -= in.v_index;
        if (!ctrl_depth) {
          // return success
          // FIXME: is this correct?
          return true;
        }

        // get control stack tail, check for error
        const pwasm_ctrl_stack_entry_t *ctrl_tail = pwasm_ctrl_stack_peek_tail(ctrl_stack, 0);
        if (!ctrl_tail) {
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_peek_tail failed");
          return false;
        }

        if (ctrl_tail->type == CTRL_LOOP) {
          i = ctrl_tail->ofs;
          stack->pos = ctrl_tail->depth;
        } else {
          // get mod, block type
          const pwasm_mod_t * const mod = frame.mod->mod;
          const int32_t block_type = insts[ctrl_tail->ofs].v_block.block_type;

          // get block type result count, check for error
          size_t num_results;
          if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br_if: get block num_results failed");
            return false;
          }

          // pop results
          for (size_t j = 0; j < num_results; j++) {
            // calculate stack source and destination offsets
            const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
            const size_t dst_ofs = ctrl_tail->depth + 1 + j;
            stack->ptr[dst_ofs] = stack->ptr[src_ofs];
          }

          // reset value stack
          stack->pos = ctrl_tail->depth + num_results;

          // pop control stack, check for error
          if (!pwasm_ctrl_stack_pop(ctrl_stack, NULL)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: ctrl_stack_pop failed");
            return false;
          }

          // decriment control stack depth
          ctrl_depth--;
        }
      }

      break;
    case PWASM_OP_BR_TABLE:
      {
        // get value from stack, branch labels, label offset, and then index
        const uint32_t val = stack->ptr[--stack->pos].i32;
        const pwasm_slice_t labels = in.v_br_table;
        const size_t labels_ofs = labels.ofs + MIN(val, labels.len - 1);
        const uint32_t id = frame.mod->mod->u32s[labels_ofs];

        // pop control stack, check for error
        if (!pwasm_ctrl_stack_popn(ctrl_stack, id)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_popn failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth -= id;
        if (!ctrl_depth) {
          // return success
          // FIXME: is this correct?
          return true;
        }

        // get control stack tail, check for error
        const pwasm_ctrl_stack_entry_t *ctrl_tail = pwasm_ctrl_stack_peek_tail(ctrl_stack, 0);
        if (!ctrl_tail) {
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_peek_tail failed");
          return false;
        }

        if (ctrl_tail->type == CTRL_LOOP) {
          i = ctrl_tail->ofs;
          stack->pos = ctrl_tail->depth;
        } else {
          // skip past end inst
          i += in.v_block.end_ofs + 1;

          // get mod, block type
          const pwasm_mod_t * const mod = frame.mod->mod;
          const int32_t block_type = insts[ctrl_tail->ofs].v_block.block_type;

          // get block type result count, check for error
          size_t num_results;
          if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br_if: get block num_results failed");
            return false;
          }

          // pop results
          for (size_t j = 0; j < num_results; j++) {
            // calculate stack source and destination offsets
            const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
            const size_t dst_ofs = ctrl_tail->depth + 1 + j;
            stack->ptr[dst_ofs] = stack->ptr[src_ofs];
          }

          // reset value stack
          stack->pos = ctrl_tail->depth + num_results;

          // pop control stack, check for error
          if (!pwasm_ctrl_stack_pop(ctrl_stack, NULL)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: ctrl_stack_pop failed");
            return false;
          }

          // decriment control stack depth
          ctrl_depth--;
        }
      }

      break;
    case PWASM_OP_RETURN:
      // return success
      // FIXME: is this correct?
      return true;
    case PWASM_OP_CALL:
      // call function, check for error
      if (!pwasm_new_interp_call_func(frame.env, frame.mod, in.v_index)) {
        // return failure
        return false;
      }

      break;
    case PWASM_OP_CALL_INDIRECT:
      {
        // pop element index from value stack
        const uint32_t elem_ofs = stack->ptr[--stack->pos].i32;

        // call function, check for error
        if (!pwasm_new_interp_call_indirect(frame, in, elem_ofs)) {
          // return failure
          return false;
        }
      }

      break;
    case PWASM_OP_DROP:
      stack->pos--;

      break;
    case PWASM_OP_SELECT:
      {
        const size_t ofs = stack->ptr[stack->pos - 1].i32 ? 3 : 2;
        stack->ptr[stack->pos - 3] = stack->ptr[stack->pos - ofs];
        stack->pos -= 2;
      }

      break;
    case PWASM_OP_LOCAL_GET:
      {
        // get local index
        // (note: checked in pwasm_checker_get_local_type())
        const uint32_t id = in.v_index;

        // push local value
        stack->ptr[stack->pos++] = stack->ptr[frame.locals.ofs + id];
      }

      break;
    case PWASM_OP_LOCAL_SET:
      {
        // get local index
        // (note: checked in pwasm_checker_get_local_type())
        const uint32_t id = in.v_index;

        // set local value, pop stack
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
        stack->pos--;
      }

      break;
    case PWASM_OP_LOCAL_TEE:
      {
        // get local index
        // (note: checked in pwasm_checker_get_local_type())
        const uint32_t id = in.v_index;

        // set local value, keep stack (tee)
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
      }

      break;
    case PWASM_OP_GLOBAL_GET:
      {
        // get global index
        const uint32_t id = pwasm_new_interp_mod_get_global_index(frame.env, frame.mod, in.v_index);

        // get global value, check for error
        pwasm_val_t val;
        if (!pwasm_env_get_global(frame.env, id, &val)) {
          // return failure
          return false;
        }

        // push global value
        stack->ptr[stack->pos++] = val;
      }

      break;
    case PWASM_OP_GLOBAL_SET:
      {
        // get global index
        const uint32_t id = pwasm_new_interp_mod_get_global_index(frame.env, frame.mod, in.v_index);

        // set global value, check for error
        if (!pwasm_env_set_global(frame.env, id, PWASM_PEEK(stack, 0))) {
          // return failure
          return false;
        }

        // pop stack
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LOAD:
    case PWASM_OP_I64_LOAD:
    case PWASM_OP_F32_LOAD:
    case PWASM_OP_F64_LOAD:
    case PWASM_OP_I32_LOAD8_S:
    case PWASM_OP_I32_LOAD8_U:
    case PWASM_OP_I32_LOAD16_S:
    case PWASM_OP_I32_LOAD16_U:
    case PWASM_OP_I64_LOAD8_S:
    case PWASM_OP_I64_LOAD8_U:
    case PWASM_OP_I64_LOAD16_S:
    case PWASM_OP_I64_LOAD16_U:
    case PWASM_OP_I64_LOAD32_S:
    case PWASM_OP_I64_LOAD32_U:
    case PWASM_OP_V128_LOAD:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t val;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &val)) {
          return false;
        }

        // pop ofs operand, push val
        stack->ptr[stack->pos - 1] = val;
      }

      break;
    case PWASM_OP_I32_STORE:
    case PWASM_OP_I64_STORE:
    case PWASM_OP_F32_STORE:
    case PWASM_OP_F64_STORE:
    case PWASM_OP_I32_STORE8:
    case PWASM_OP_I32_STORE16:
    case PWASM_OP_I64_STORE8:
    case PWASM_OP_I64_STORE16:
    case PWASM_OP_I64_STORE32:
    case PWASM_OP_V128_STORE:
      {
        // get offset operand and value
        const uint32_t ofs = stack->ptr[stack->pos - 2].i32;
        const pwasm_val_t val = stack->ptr[stack->pos - 1];
        stack->pos -= 2;

        // store value, check for error
        if (!pwasm_env_mem_store(frame.env, frame.mem_id, in, ofs, val)) {
          return false;
        }
      }

      break;
    case PWASM_OP_MEMORY_SIZE:
      {
        // get memory size, check for error
        uint32_t size;
        if (!pwasm_env_mem_size(frame.env, frame.mem_id, &size)) {
          return false;
        }

        // push size to stack
        stack->ptr[stack->pos++].i32 = size;
      }

      break;
    case PWASM_OP_MEMORY_GROW:
      {
        // get grow operand
        const uint32_t grow = stack->ptr[stack->pos - 1].i32;

        // grow memory, check for error
        uint32_t size;
        if (!pwasm_env_mem_grow(frame.env, frame.mem_id, grow, &size)) {
          return false;
        }

        // pop operand, push result
        stack->ptr[stack->pos - 1].i32 = size;
      }

      break;
    case PWASM_OP_I32_CONST:
      stack->ptr[stack->pos++].i32 = in.v_i32;

      break;
    case PWASM_OP_I64_CONST:
      stack->ptr[stack->pos++].i64 = in.v_i64;

      break;
    case PWASM_OP_F32_CONST:
      stack->ptr[stack->pos++].f32 = in.v_f32;

      break;
    case PWASM_OP_F64_CONST:
      stack->ptr[stack->pos++].f64 = in.v_f64;

      break;
    case PWASM_OP_I32_EQZ:
      stack->ptr[stack->pos - 1].i32 = (stack->ptr[stack->pos - 1].i32 == 0);

      break;
    case PWASM_OP_I32_EQ:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_NE:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LT_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LT_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GT_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GT_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LE_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LE_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GE_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GE_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_EQZ:
      stack->ptr[stack->pos - 1].i32 = (stack->ptr[stack->pos - 1].i64 == 0);

      break;
    case PWASM_OP_I64_EQ:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_NE:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LT_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LT_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GT_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GT_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LE_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LE_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GE_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GE_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_EQ:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_NE:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_LT:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_GT:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_LE:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_GE:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_EQ:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_NE:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_LT:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_GT:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_LE:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_GE:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_CLZ:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = a ? __builtin_clz(a) : 32;
      }

      break;
    case PWASM_OP_I32_CTZ:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = a ? __builtin_ctz(a) : 32;
      }

      break;
    case PWASM_OP_I32_POPCNT:
      {
        const int val = __builtin_popcount(stack->ptr[stack->pos - 1].i32);
        stack->ptr[stack->pos - 1].i32 = val;
      }

      break;
    case PWASM_OP_I32_ADD:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SUB:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_MUL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_DIV_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.div_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_DIV_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.div_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.rem_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.rem_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_AND:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a & b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_OR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a | b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_XOR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a ^ b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = (a << b) | (a >> (32 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = (a << (32 - b)) | (a >> b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_CLZ:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = a ? __builtin_clzl(a) : 64;
      }

      break;
    case PWASM_OP_I64_CTZ:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = a ? __builtin_ctzl(a) : 64;
      }

      break;
    case PWASM_OP_I64_POPCNT:
      {
        const int val = __builtin_popcountl(stack->ptr[stack->pos - 1].i64);
        stack->ptr[stack->pos - 1].i64 = val;
      }

      break;
    case PWASM_OP_I64_ADD:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SUB:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_MUL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_DIV_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.div_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_DIV_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.div_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.rem_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.rem_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_AND:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a & b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_OR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a | b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_XOR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a ^ b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = (a << b) | (a >> (64 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = (a << (64 - b)) | (a >> b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_ABS:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = fabsf(a);
      }

      break;
    case PWASM_OP_F32_NEG:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = -a;
      }

      break;
    case PWASM_OP_F32_CEIL:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = ceilf(a);
      }

      break;
    case PWASM_OP_F32_FLOOR:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = floorf(a);
      }

      break;
    case PWASM_OP_F32_TRUNC:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = truncf(a);
      }

      break;
    case PWASM_OP_F32_NEAREST:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = roundf(a);
      }

      break;
    case PWASM_OP_F32_SQRT:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = sqrtf(a);
      }

      break;
    case PWASM_OP_F32_ADD:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_SUB:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_MUL:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_DIV:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_MIN:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = fminf(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_MAX:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = fmaxf(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_COPYSIGN:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = copysignf(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_ABS:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = fabs(a);
      }

      break;
    case PWASM_OP_F64_NEG:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = -a;
      }

      break;
    case PWASM_OP_F64_CEIL:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = ceil(a);
      }

      break;
    case PWASM_OP_F64_FLOOR:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = floor(a);
      }

      break;
    case PWASM_OP_F64_TRUNC:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = trunc(a);
      }

      break;
    case PWASM_OP_F64_NEAREST:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = round(a);
      }

      break;
    case PWASM_OP_F64_SQRT:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = sqrt(a);
      }

      break;
    case PWASM_OP_F64_ADD:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_SUB:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_MUL:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_DIV:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_MIN:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = fmin(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_MAX:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = fmax(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_COPYSIGN:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = copysign(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_WRAP_I64:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (int32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (int32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) a;
      }

      break;
    case PWASM_OP_I64_EXTEND_I32_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i64 = (int64_t) a;
      }

      break;
    case PWASM_OP_I64_EXTEND_I32_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (int64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (int64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I32_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I32_U:
      {
        const uint32_t a = (uint32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I64_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I64_U:
      {
        const uint64_t a = (uint64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_DEMOTE_F64:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I32_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I32_U:
      {
        const uint32_t a = (uint32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I64_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I64_U:
      {
        const uint64_t a = (uint64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_PROMOTE_F32:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f64 = a;
      }

      break;
    case PWASM_OP_I32_REINTERPRET_F32:
      {
        const union {
          uint32_t i32;
          float f32;
        } v = { .f32 = stack->ptr[stack->pos - 1].f32 };
        stack->ptr[stack->pos - 1].i32 = v.i32;
      }

      break;
    case PWASM_OP_I64_REINTERPRET_F64:
      {
        const union {
          uint64_t i64;
          double f64;
        } v = { .f64 = stack->ptr[stack->pos - 1].f64 };
        stack->ptr[stack->pos - 1].i64 = v.i64;
      }

      break;
    case PWASM_OP_F32_REINTERPRET_I32:
      {
        const union {
          uint32_t i32;
          float f32;
        } v = { .i32 = stack->ptr[stack->pos - 1].i32 };
        stack->ptr[stack->pos - 1].f32 = v.f32;
      }

      break;
    case PWASM_OP_F64_REINTERPRET_I64:
      {
        const union {
          uint64_t i64;
          double f64;
        } v = { .i64 = stack->ptr[stack->pos - 1].i64 };
        stack->ptr[stack->pos - 1].f64 = v.f64;
      }

      break;
    case PWASM_OP_I32_EXTEND8_S:
      {
        const int32_t val = (int8_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) val;
      }

      break;
    case PWASM_OP_I32_EXTEND16_S:
      {
        const int32_t val = (int16_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) val;
      }

      break;
    case PWASM_OP_I64_EXTEND8_S:
      {
        const int64_t val = (int8_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) val;
      }

      break;
    case PWASM_OP_I64_EXTEND16_S:
      {
        const int64_t val = (int16_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) val;
      }

      break;
    case PWASM_OP_I64_EXTEND32_S:
      {
        const int64_t val = (int32_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) val;
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (int32_t) CLAMP(a, INT32_MIN, INT32_MAX);
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) CLAMP(a, 0, UINT32_MAX);
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (int32_t) CLAMP(a, INT32_MIN, INT32_MAX);
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) CLAMP(a, 0, UINT32_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (int64_t) CLAMP(a, INT64_MIN, INT64_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) CLAMP(a, 0, UINT64_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (int64_t) CLAMP(a, INT64_MIN, INT64_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) CLAMP(a, 0, UINT64_MAX);
      }

      break;
    case PWASM_OP_V128_CONST:
      stack->ptr[stack->pos++].v128 = in.v_v128;

      break;
    case PWASM_OP_I8X16_SPLAT:
      {
        const uint8_t a = stack->ptr[stack->pos - 1].i32;
        memset(stack->ptr[stack->pos - 1].v128.i8, a, 16);
      }

      break;
    case PWASM_OP_I8X16_EXTRACT_LANE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = (int8_t) a.i8[in.v_index];
      }

      break;
    case PWASM_OP_I8X16_EXTRACT_LANE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = a.i8[in.v_index];
      }

      break;
    case PWASM_OP_I8X16_REPLACE_LANE:
      {
        const uint8_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].v128.i8[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SPLAT:
      {
        const uint16_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].v128.i16[0] = a;
        stack->ptr[stack->pos - 1].v128.i16[1] = a;
        stack->ptr[stack->pos - 1].v128.i16[2] = a;
        stack->ptr[stack->pos - 1].v128.i16[3] = a;
        stack->ptr[stack->pos - 1].v128.i16[4] = a;
        stack->ptr[stack->pos - 1].v128.i16[5] = a;
        stack->ptr[stack->pos - 1].v128.i16[6] = a;
        stack->ptr[stack->pos - 1].v128.i16[7] = a;
      }

      break;
    case PWASM_OP_I16X8_EXTRACT_LANE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = (int16_t) a.i16[in.v_index];
      }

      break;
    case PWASM_OP_I16X8_EXTRACT_LANE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = a.i16[in.v_index];
      }

      break;
    case PWASM_OP_I16X8_REPLACE_LANE:
      {
        const uint16_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].v128.i16[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SPLAT:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].v128.i32[0] = a;
        stack->ptr[stack->pos - 1].v128.i32[1] = a;
        stack->ptr[stack->pos - 1].v128.i32[2] = a;
        stack->ptr[stack->pos - 1].v128.i32[3] = a;
      }

      break;
    case PWASM_OP_I32X4_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = a.i32[in.v_index];
      }

      break;
    case PWASM_OP_I32X4_REPLACE_LANE:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].v128.i32[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SPLAT:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        D("a = 0x%016lx", a);
        stack->ptr[stack->pos - 1].v128.i64[0] = a;
        stack->ptr[stack->pos - 1].v128.i64[1] = a;
      }

      break;
    case PWASM_OP_I64X2_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i64 = a.i64[in.v_index];
      }

      break;
    case PWASM_OP_I64X2_REPLACE_LANE:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].v128.i64[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_SPLAT:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].v128.f32[0] = a;
        stack->ptr[stack->pos - 1].v128.f32[1] = a;
        stack->ptr[stack->pos - 1].v128.f32[2] = a;
        stack->ptr[stack->pos - 1].v128.f32[3] = a;
      }

      break;
    case PWASM_OP_F32X4_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].f32 = a.f32[in.v_index];
      }

      break;
    case PWASM_OP_F32X4_REPLACE_LANE:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].v128.f32[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_SPLAT:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].v128.f64[0] = a;
        stack->ptr[stack->pos - 1].v128.f64[1] = a;
      }

      break;
    case PWASM_OP_F64X2_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].f64 = a.f64[in.v_index];
      }

      break;
    case PWASM_OP_F64X2_REPLACE_LANE:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].v128.f64[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = (a.i8[j] == b.i8[j]) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = (a.i8[j] != b.i8[j]) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av < bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av < bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av > bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av > bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av <= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av <= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av >= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av >= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = (a.i16[j] == b.i16[j]) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = (a.i16[j] != b.i16[j]) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av < bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av < bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av > bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av > bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av <= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av <= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av >= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av >= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.i32[j] == b.i32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.i32[j] != b.i32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av < bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av < bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av > bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av > bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av <= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av <= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av >= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av >= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] == b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] != b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_LT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] < b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_GT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] > b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_LE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] <= b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_GE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] >= b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] == b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] != b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_LT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] < b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_GT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] > b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_LE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] <= b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_GE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] >= b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_NOT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        b.i64[0] = ~a.i64[0];
        b.i64[1] = ~a.i64[1];
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V128_AND:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] & b.i64[0];
        c.i64[1] = a.i64[1] & b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_OR:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] | b.i64[0];
        c.i64[1] = a.i64[1] | b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_XOR:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] ^ b.i64[0];
        c.i64[1] = a.i64[1] ^ b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_BITSELECT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 3].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t m = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = (a.i64[0] & m.i64[0]) | (b.i64[0] & ~m.i64[0]);
        c.i64[1] = (a.i64[1] & m.i64[1]) | (b.i64[1] & ~m.i64[1]);

        stack->ptr[stack->pos - 3].v128 = c;
        stack->pos -= 2;
      }

      break;
    case PWASM_OP_I8X16_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 16; j++) {
          b.i8[j] = -a.i8[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I8X16_ANY_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i8[0] ||  a.i8[1] ||  a.i8[2] ||  a.i8[3] ||
          a.i8[4] ||  a.i8[5] ||  a.i8[6] ||  a.i8[7] ||
          a.i8[8] ||  a.i8[9] ||  a.i8[10] || a.i8[11] ||
          a.i8[12] || a.i8[13] || a.i8[14] || a.i8[15]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I8X16_ALL_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i8[0] &&  a.i8[1] &&  a.i8[2] &&  a.i8[3] &&
          a.i8[4] &&  a.i8[5] &&  a.i8[6] &&  a.i8[7] &&
          a.i8[8] &&  a.i8[9] &&  a.i8[10] && a.i8[11] &&
          a.i8[12] && a.i8[13] && a.i8[14] && a.i8[15]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I8X16_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x7;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = (a.i8[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x7;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          int8_t av = a.i8[j];
          c.i8[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x7;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          uint8_t av = a.i8[j];
          c.i8[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = a.i8[j] + b.i8[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ADD_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          const int32_t v = av + bv;
          c.i8[j] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ADD_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          const uint32_t v = av + bv;
          c.i8[j] = MIN(v, UINT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = a.i8[j] - b.i8[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SUB_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          const int32_t v = av - bv;
          c.i8[j] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SUB_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av > bv) ? (av - bv) : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MIN_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MIN_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MAX_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MAX_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = -a.i16[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_ANY_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i16[0] || a.i16[1] || a.i16[2] || a.i16[3] ||
          a.i16[4] || a.i16[5] || a.i16[6] || a.i16[7]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I16X8_ALL_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i16[0] && a.i16[1] && a.i16[2] && a.i16[3] &&
          a.i16[4] && a.i16[5] && a.i16[6] && a.i16[7]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I16X8_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0xF;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = (a.i16[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0xF;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          int16_t av = a.i16[j];
          c.i16[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0xF;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          uint16_t av = a.i16[j];
          c.i16[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = a.i16[j] + b.i16[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_ADD_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          const int32_t v = av + bv;
          c.i16[j] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_ADD_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          const uint32_t v = av + bv;
          c.i16[j] = MIN(v, UINT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = a.i16[j] - b.i16[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SUB_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          const int32_t v = av - bv;
          c.i16[j] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SUB_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av > bv) ? (av - bv) : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = av * bv;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MIN_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MIN_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MAX_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MAX_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = -a.i32[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_ANY_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i32[0] || a.i32[1] || a.i32[2] || a.i32[3]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I32X4_ALL_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i32[0] && a.i32[1] && a.i32[2] && a.i32[3]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I32X4_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.i32[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          int32_t av = a.i32[j];
          c.i32[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          uint32_t av = a.i32[j];
          c.i32[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = a.i32[j] + b.i32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = a.i32[j] - b.i32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = a.i32[j] * b.i32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MIN_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MIN_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MAX_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MAX_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.i64[j] = -a.i64[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I64X2_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.i64[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          int64_t av = a.i64[j];
          c.i64[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          uint64_t av = a.i64[j];
          c.i64[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = a.i64[j] + b.i64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = a.i64[j] - b.i64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = a.i64[j] * b.i64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.f32[j] = fabsf(a.f32[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.f32[j] = -a.f32[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_SQRT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.f32[j] = sqrtf(a.f32[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] + b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] - b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] * b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_DIV:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] / b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_MIN:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = fminf(a.f32[j], b.f32[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_MAX:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = fmaxf(a.f32[j], b.f32[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.f64[j] = fabs(a.f64[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F64X2_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.f64[j] = -a.f64[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F64X2_SQRT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.f64[j] = sqrt(a.f64[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F64X2_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] + b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] - b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] * b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_DIV:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] / b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_MIN:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = fmin(a.f64[j], b.f64[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_MAX:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = fmax(a.f64[j], b.f64[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_TRUNC_SAT_F32X4_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          int32_t v = CLAMP(a.f32[j], INT32_MIN, INT32_MAX);
          b.i32[j] = v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_TRUNC_SAT_F32X4_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = MIN(a.f32[j], UINT32_MAX);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_CONVERT_I32X4_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          int32_t v = a.i32[j];
          b.f32[j] = v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_CONVERT_I32X4_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          uint32_t v = a.i32[j];
          b.f32[j] = v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V8X16_SWIZZLE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint32_t ofs = (b.i8[j] < 16) ? b.i8[j] : 0;
          c.i8[j] = a.i8[ofs];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V8X16_SHUFFLE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t ofs = in.v_v128.i8[j] & 0x1F;
          c.i8[j] = (ofs < 16) ? a.i8[ofs] : b.i8[ofs - 16];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V8X16_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 16; j++) {
          b.i8[j] = a.v128.i8[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V16X8_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.v128.i16[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V32X4_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.v128.i32[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V64X2_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.i64[j] = a.v128.i64[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I8X16_NARROW_I16X8_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t v = a.i16[j];
          c.i8[j] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        for (size_t j = 0; j < 8; j++) {
          const int16_t v = b.i16[j];
          c.i8[j + 8] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_NARROW_I16X8_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t v = a.i16[j];
          c.i8[j] = CLAMP(v, 0, UINT8_MAX);
        }

        for (size_t j = 0; j < 8; j++) {
          const int16_t v = b.i16[j];
          c.i8[j + 8] = CLAMP(v, 0, UINT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NARROW_I32X4_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t v = a.i32[j];
          c.i16[j] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        for (size_t j = 0; j < 4; j++) {
          const int32_t v = b.i32[j];
          c.i16[j + 4] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NARROW_I32X4_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t v = a.i32[j];
          c.i16[j] = CLAMP(v, 0, UINT16_MAX);
        }

        for (size_t j = 0; j < 4; j++) {
          const int32_t v = b.i32[j];
          c.i16[j + 4] = CLAMP(v, 0, UINT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_LOW_I8X16_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int8_t v = a.i8[j];
          b.i16[j] = (int16_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_HIGH_I8X16_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int8_t v = a.i8[8 + j];
          b.i16[j] = (int16_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_LOW_I8X16_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.i8[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_HIGH_I8X16_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.i8[8 + j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_LOW_I16X8_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int16_t v = a.i16[j];
          b.i32[j] = (int32_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_HIGH_I16X8_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int16_t v = a.i16[4 + j];
          b.i32[j] = (int32_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_LOW_I16X8_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.i16[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_HIGH_I16X8_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.i16[4 + j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_LOAD8X8_S:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int8_t v = a.v128.i8[j];
          b.i16[j] = (int16_t) v;
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_LOAD8X8_U:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.v128.i8[j];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_LOAD16X4_S:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int16_t v = a.v128.i16[j];
          b.i32[j] = (int32_t) v;
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_LOAD16X4_U:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.v128.i16[j];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I64X2_LOAD32X2_S:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          const int32_t v = a.v128.i32[j];
          b.i64[j] = (int64_t) v;
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I64X2_LOAD32X2_U:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.i64[j] = a.v128.i32[j];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V128_ANDNOT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] & ~b.i64[0];
        c.i64[1] = a.i64[1] & ~b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_AVGR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av + bv + 1) / 2;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_AVGR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av + bv + 1) / 2;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 16; j++) {
          const int8_t v = a.i8[j];
          b.i8[j] = (v < 0) ? -v : v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int16_t v = a.i16[j];
          b.i16[j] = (v < 0) ? -v : v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int32_t v = a.i32[j];
          b.i32[j] = (v < 0) ? -v : v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    default:
      // log error, return failure
      pwasm_env_fail(frame.env, "unknown instruction");
      return false;
    }
  }

  // return success (i think?)
  return true;
}

/*
 * world's second shittiest initial interpreter
 */
static bool
pwasm_new_interp_call_func(
  pwasm_env_t * const env,
  pwasm_new_interp_mod_t * const interp_mod,
  uint32_t func_ofs
) {
  pwasm_new_interp_t * const interp = env->env_data;
  pwasm_stack_t * const stack = env->stack;
  const pwasm_mod_t * const mod = interp_mod->mod;
  // will be used for CALL and CALL_INDIRECT
  // const size_t stack_pos = env->stack->pos;

  // get func parameters and results
  const pwasm_slice_t params = mod->types[mod->funcs[func_ofs]].params;
  const pwasm_slice_t results = mod->types[mod->funcs[func_ofs]].results;

  // check stack position (e.g. missing parameters)
  // (FIXME: do we need this, should it be handled in check?)
  if (stack->pos < params.len) {
    // log error, return failure
    D("missing parameters: stack->pos = %zu, params.len = %zu", stack->pos, params.len);
    pwasm_env_fail(env, "missing function parameters");
    return false;
  }

  // get number of local slots and total frame size
  const size_t max_locals = mod->codes[func_ofs].max_locals;
  const size_t frame_size = mod->codes[func_ofs].frame_size;
  if (max_locals > 0) {
    // clear local slots
    memset(stack->ptr + stack->pos, 0, sizeof(pwasm_val_t) * max_locals);
  }

  // skip past locals
  stack->pos += max_locals;

  // get mems count and pointer in this module
  const size_t num_mems = interp_mod->mems.len;
  const uint32_t * const mems = ((uint32_t*) pwasm_vec_get_data(&(interp->u32s))) + interp_mod->mems.ofs;

  // D("mems slice = { %zu, %zu }, len = %zu, mems[0] = %u", interp_mod->mems.ofs, interp_mod->mems.len, num_mems, num_mems ? mems[0] : 0);
  // build interpreter frame
  pwasm_new_interp_frame_t frame = {
    .env = env,
    .mod = interp_mod,
    // FIXME: is this right?
    .mem_id = num_mems ? mems[0] : 0,
    .params = params,
    .locals = {
      .ofs = stack->pos - frame_size,
      .len = frame_size,
    },
  };

  // get expr instructions slice
  const pwasm_slice_t expr = mod->codes[func_ofs].expr;

  // evaluate expr, check for error
  const bool ok = pwasm_new_interp_eval_expr(frame, expr);
  if (!ok) {
    D("eval_expr() failed, func_ofs = %u", func_ofs);
    // return failure
    return false;
  }

  // get func results
  if (frame_size > 0) {
    // calc dst and src stack positions
    const size_t dst_pos = frame.locals.ofs;
    const size_t src_pos = stack->pos - results.len;
    const size_t num_bytes = sizeof(pwasm_val_t) * results.len;

    // D("stack->pos: old = %zu, new = %zu", stack->pos, dst_pos + results.len);
    // copy results, update stack position
    memmove(stack->ptr + dst_pos, stack->ptr + src_pos, num_bytes);
    stack->pos = dst_pos + results.len;
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_call(
  pwasm_env_t * const env,
  const uint32_t func_id
) {
  pwasm_new_interp_t * const interp = env->env_data;
  const pwasm_vec_t * const funcs_vec = &(interp->funcs);
  const pwasm_vec_t * const mods_vec = &(interp->mods);
  const pwasm_new_interp_func_t * const funcs = pwasm_vec_get_data(funcs_vec);
  pwasm_new_interp_mod_t * const mods = (pwasm_new_interp_mod_t*) pwasm_vec_get_data(mods_vec);
  const size_t num_funcs = pwasm_vec_get_size(funcs_vec);
  const size_t num_mods = pwasm_vec_get_size(mods_vec);

  // check that func_id is in bounds
  if (!func_id || func_id > num_funcs) {
    D("bad func_id: %u", func_id);
    pwasm_env_fail(env, "function index out of bounds");
    return false;
  }

  // convert func_id to offset, get function row
  const pwasm_new_interp_func_t func = funcs[func_id - 1];

  // check mod_ofs
  if (func.mod_ofs >= num_mods) {
    D("bad func mod_ofs: %u", func.mod_ofs);
    pwasm_env_fail(env, "function mod offset out of bounds");
    return false;
  }

  // get mod
  const pwasm_new_interp_mod_t mod = mods[func.mod_ofs];

  // check func_ofs
  if (func.func_ofs >= mod.funcs.len) {
    D("bad func_ofs: %u", func.mod_ofs);
    pwasm_env_fail(env, "function offset out of bounds");
    return false;
  }

  switch (mod.type) {
  case PWASM_NEW_INTERP_MOD_TYPE_MOD:
    D("found func, calling it: %u", func_id);
    return pwasm_new_interp_call_func(env, mods + func.mod_ofs, func.func_ofs);
  case PWASM_NEW_INTERP_MOD_TYPE_NATIVE:
    D("found native func, calling it: %u", func_id);
    const pwasm_native_func_t * const f = mod.native->funcs + func.func_ofs;
    return f->func(env, mod.native);
  default:
    D("func_id %u maps to invalid mod type %u", func_id, mod.type);
    pwasm_env_fail(env, "invalid function module type");
    return false;
  }
}

/**
 * Given a frame and a table ID, get the offset of the table instance in
 * the interpreter.
 *
 * Returns false if the table_id is invalid for the current module.
 */
static bool
pwasm_new_interp_get_mod_table_ofs(
  const pwasm_new_interp_frame_t frame,
  const uint32_t mod_table_id,
  uint32_t * const ret_ofs
) {
  pwasm_new_interp_t * const interp = frame.env->env_data;
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));
  const size_t num_tables = frame.mod->tables.len;

  // bounds check table index
  if (mod_table_id >= num_tables) {
    D("mod_table_id (%u) >= num_tables (%zu)", mod_table_id, num_tables);
    pwasm_env_fail(frame.env, "table index out of bounds");
    return false;
  }

  // map local index to table interpreter offset
  *ret_ofs = u32s[frame.mod->tables.ofs + mod_table_id];

  // return success
  return true;
}

/**
 * Given a frame and a call_indirect instruction, and a target function
 * offset in the interpreter, compare the following types:
 *
 *  * in the index immediate of the call_indirect instruction, and
 *  * the type of the target function and
 *
 * The comparison verifies all of the following:
 *
 *   * parameter count
 *   * result count
 *   * parameter value types
 *   * result value types
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_new_interp_call_indirect_check_type(
  const pwasm_new_interp_frame_t frame,
  const pwasm_inst_t in,
  const uint32_t func_ofs
) {
  pwasm_new_interp_t * const interp = frame.env->env_data;

  // get instruction immediate type
  const uint32_t * const in_u32s = frame.mod->mod->u32s;
  const pwasm_type_t in_type = frame.mod->mod->types[in.v_index];

  // get function type
  const pwasm_vec_t * const fn_vec = &(interp->funcs);
  const pwasm_new_interp_func_t * const fns = pwasm_vec_get_data(fn_vec);
  const pwasm_new_interp_func_t fn = fns[func_ofs];
  const pwasm_vec_t * const mod_vec = &(interp->mods);
  const pwasm_new_interp_mod_t * const mods = pwasm_vec_get_data(mod_vec);
  const pwasm_mod_t * const fn_mod = mods[fn.mod_ofs].mod;
  const uint32_t * const fn_u32s = fn_mod->u32s;
  const pwasm_type_t fn_type = fn_mod->types[fn_mod->funcs[fn.func_ofs]];

  // check parameter count
  if (in_type.params.len != fn_type.params.len) {
    pwasm_env_fail(frame.env, "call_indirect parameter count mismatch");
    return false;
  }

  // check result count
  if (in_type.results.len != fn_type.results.len) {
    pwasm_env_fail(frame.env, "call_indirect result count mismatch");
    return false;
  }

  // check parameters
  for (size_t i = 0 ; i < in_type.params.len; i++) {
    const uint32_t in_param_type = in_u32s[in_type.params.ofs + i];
    const uint32_t fn_param_type = fn_u32s[fn_type.params.ofs + i];

    if (in_param_type != fn_param_type) {
      pwasm_env_fail(frame.env, "call_indirect parameter type mismatch");
      return false;
    }
  }

  // return success
  return true;
}

static bool
pwasm_new_interp_call_indirect(
  const pwasm_new_interp_frame_t frame,
  const pwasm_inst_t in,
  const uint32_t elem_ofs
) {
  // get module-relative table index from instruction
  // TODO: hard-coded for now, get from instruction eventually
  const uint32_t mod_table_id = 0;

  // get interpreter table offset
  uint32_t table_ofs;
  if (!pwasm_new_interp_get_mod_table_ofs(frame, mod_table_id, &table_ofs)) {
    return false;
  }

  // convert table offset to ID
  const uint32_t table_id = table_ofs + 1;
  D("mod_table_id = %u, table_id (interp) = %u", mod_table_id, table_id);

  // get interpreter function offset, check for error
  uint32_t func_ofs;
  if (!pwasm_new_interp_get_elem(frame.env, table_id, elem_ofs, &func_ofs)) {
    return false;
  }

  // check instruction type index against function type
  if (!pwasm_new_interp_call_indirect_check_type(frame, in, func_ofs)) {
    return false;
  }

  // call function, return result
  return pwasm_new_interp_call(frame.env, func_ofs + 1);
}

//
// new interpreter callbacks
//

static bool
pwasm_new_interp_on_init(
  pwasm_env_t * const env
) {
  return pwasm_new_interp_init(env);
}

static void
pwasm_new_interp_on_fini(
  pwasm_env_t * const env
) {
  pwasm_new_interp_fini(env);
}

static uint32_t
  pwasm_new_interp_on_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  return pwasm_new_interp_add_native(env, name, mod);
}

static uint32_t
pwasm_new_interp_on_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  return pwasm_new_interp_add_mod(env, name, mod);
}

static const pwasm_mod_t *
pwasm_new_interp_on_get_mod(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  return pwasm_new_interp_get_mod(env, mod_id);
}

static const pwasm_buf_t *
pwasm_new_interp_on_get_mod_name(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  return pwasm_new_interp_get_mod_name(env, mod_id);
}

static uint32_t
pwasm_new_interp_on_find_mod(
  pwasm_env_t * const env,
  const pwasm_buf_t name
) {
  return pwasm_new_interp_find_mod(env, name);
}

static uint32_t
pwasm_new_interp_on_find_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  return pwasm_new_interp_find_func(env, mod_id, name);
}

static uint32_t
pwasm_new_interp_on_find_mem(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  return pwasm_new_interp_find_mem(env, mod_id, name);
}

/* 
 * static uint32_t
 * pwasm_new_interp_on_find_table(
 *   pwasm_env_t * const env,
 *   const uint32_t mod_id,
 *   const pwasm_buf_t name
 * ) {
 *   return pwasm_new_interp_find_table(env, mod_id, name);
 * }
 */ 

static pwasm_env_mem_t *
pwasm_new_interp_on_get_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id
) {
  return pwasm_new_interp_get_mem(env, mem_id);
}

static bool
pwasm_new_interp_on_mem_load(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_val_t * const ret_val
) {
  return pwasm_new_interp_mem_load(env, mem_id, in, arg_ofs, ret_val);
}

static bool
pwasm_new_interp_on_mem_store(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  const pwasm_val_t val
) {
  return pwasm_new_interp_mem_store(env, mem_id, in, arg_ofs, val);
}

static bool
pwasm_new_interp_on_mem_size(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  uint32_t * const ret_val
) {
  return pwasm_new_interp_mem_size(env, mem_id, ret_val);
}

static bool
pwasm_new_interp_on_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  return pwasm_new_interp_mem_grow(env, mem_id, grow, ret_val);
}

static bool
pwasm_new_interp_on_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_val
) {
  return pwasm_new_interp_get_elem(env, table_id, elem_ofs, ret_val);
}

static uint32_t
pwasm_new_interp_on_find_import(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
) {
  return pwasm_new_interp_find_import(env, mod_id, type, name);
}

static bool
pwasm_new_interp_on_get_global(
  pwasm_env_t * const env,
  const uint32_t id,
  pwasm_val_t * const ret_val
) {
  return pwasm_new_interp_get_global(env, id, ret_val);
}

static bool
pwasm_new_interp_on_set_global(
  pwasm_env_t * const env,
  const uint32_t id,
  const pwasm_val_t val
) {
  return pwasm_new_interp_set_global(env, id, val);
}

static bool
pwasm_new_interp_on_call(
  pwasm_env_t * const env,
  const uint32_t func_id
) {
  return pwasm_new_interp_call(env, func_id);
}

/*
 * Interpreter environment callbacks.
 */
static const pwasm_env_cbs_t
NEW_PWASM_INTERP_CBS = {
  .init         = pwasm_new_interp_on_init,
  .fini         = pwasm_new_interp_on_fini,
  .add_native   = pwasm_new_interp_on_add_native,
  .add_mod      = pwasm_new_interp_on_add_mod,
  .get_mod      = pwasm_new_interp_on_get_mod,
  .get_mod_name = pwasm_new_interp_on_get_mod_name,
  .find_mod     = pwasm_new_interp_on_find_mod,
  .find_func    = pwasm_new_interp_on_find_func,
  .find_mem     = pwasm_new_interp_on_find_mem,
  // .find_table   = pwasm_new_interp_on_find_table,
  .get_mem      = pwasm_new_interp_on_get_mem,
  .mem_load     = pwasm_new_interp_on_mem_load,
  .mem_store    = pwasm_new_interp_on_mem_store,
  .mem_size     = pwasm_new_interp_on_mem_size,
  .mem_grow     = pwasm_new_interp_on_mem_grow,
  .get_elem     = pwasm_new_interp_on_get_elem,
  .find_import  = pwasm_new_interp_on_find_import,
  .get_global   = pwasm_new_interp_on_get_global,
  .set_global   = pwasm_new_interp_on_set_global,
  .call         = pwasm_new_interp_on_call,
};

/*
 * Return new interpreter environment callbacks.
 */
const pwasm_env_cbs_t *
pwasm_new_interpreter_get_cbs(void) {
  return &NEW_PWASM_INTERP_CBS;
}

//
// aot jit
//

typedef enum {
  PWASM_AOT_JIT_MOD_TYPE_MOD,
  PWASM_AOT_JIT_MOD_TYPE_NATIVE,
  PWASM_AOT_JIT_MOD_TYPE_LAST,
} pwasm_aot_jit_mod_type_t;

typedef struct {
  // module name
  pwasm_buf_t name;

  // module type (internal or native)
  pwasm_aot_jit_mod_type_t type;

  // references to the u32s vector in the parent environment
  pwasm_slice_t funcs;
  pwasm_slice_t globals;
  pwasm_slice_t mems;
  pwasm_slice_t tables;

  // array of buffers containing pointers to compiled functions
  pwasm_buf_t *fns;

  union {
    const pwasm_native_t * const native;
    const pwasm_mod_t * const mod;
  };
} pwasm_aot_jit_mod_t;

typedef struct {
  // mod offset in parent environment
  uint32_t mod_ofs;

  // func offset in parent mod
  uint32_t func_ofs;
} pwasm_aot_jit_func_t;

typedef struct {
  pwasm_env_mem_t * const mem; // memory pointer
  const size_t ofs; // absolute offset, in bytes
  const size_t size; // size, in bytes
} pwasm_aot_jit_mem_chunk_t;

/*
 * static void
 * pwasm_aot_jit_dump_mem_chunk(
 *   const pwasm_aot_jit_mem_chunk_t chunk
 * ) {
 *   D("{ .mem = %p, .ofs = %zu, .size = %zu }", (void*) chunk.mem, chunk.ofs, chunk.size);
 * }
 */

typedef struct {
  // mod offset in parent environment
  uint32_t mod_ofs;

  // table offset in parent mod
  uint32_t table_ofs;

  // limits from initial mod
  const pwasm_limits_t limits;

  // vec of u32 values
  uint32_t *vals;

  // array of u64s indicating set elements
  uint64_t *masks;

  // maximum number of elements
  size_t max_vals;
} pwasm_aot_jit_table_t;

static pwasm_aot_jit_table_t
pwasm_aot_jit_table_init(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const uint32_t table_ofs,
  pwasm_limits_t limits
) {
  (void) env;
  return (pwasm_aot_jit_table_t) {
    .mod_ofs    = mod_ofs,
    .table_ofs  = table_ofs,
    .limits     = limits,
    .vals       = NULL,
    .masks      = NULL,
    .max_vals   = 0,
  };
}

static void
pwasm_aot_jit_table_fini(
  pwasm_env_t * const env,
  pwasm_aot_jit_table_t * const table
) {
  if (!table->max_vals) {
    return;
  }

  if (table->max_vals > 0) {
    // free memory and masks
    pwasm_realloc(env->mem_ctx, table->vals, 0);
    pwasm_realloc(env->mem_ctx, table->masks, 0);
    table->vals = NULL;
    table->masks = NULL;
    table->max_vals = 0;
  }
}

static bool
pwasm_aot_jit_table_grow(
  pwasm_env_t * const env,
  pwasm_aot_jit_table_t * const table,
  const size_t src_new_len
) {
  // check existing capacity
  if (src_new_len <= table->max_vals) {
    return true;
  }

  // clamp to minimum size
  const size_t new_len = (table->limits.min > src_new_len) ? table->limits.min : src_new_len;

  // check table maximum limit
  if (table->limits.has_max && src_new_len > table->limits.max) {
    D("src_new_len = %zu, max = %u", src_new_len, table->limits.max);
    pwasm_env_fail(env, "length greater than table limit");
    return false;
  }

  // reallocate vals, check for error
  uint32_t *tmp_vals = pwasm_realloc(env->mem_ctx, table->vals, new_len * sizeof(uint32_t));
  if (!tmp_vals && new_len > 0) {
    pwasm_env_fail(env, "vals pwasm_realloc() failed");
    return false;
  }

  // reallocate masks, check for error
  const size_t new_num_masks = (new_len / 64) + ((new_len & 0x3F) ? 1 : 0);
  uint64_t *tmp_masks = pwasm_realloc(env->mem_ctx, table->masks, new_num_masks * sizeof(uint64_t));
  if (!tmp_vals && new_len > 0) {
    pwasm_env_fail(env, "masks pwasm_realloc() failed");
    return false;
  }

  // clear masks
  for (size_t i = table->max_vals; i < new_len; i++) {
    tmp_masks[i >> 6] &= ~(((uint64_t) 1) << (i & 0x3F));
  }

  // update vals, masks, and max_len
  table->vals = tmp_vals;
  table->masks = tmp_masks;
  table->max_vals = new_len;

  // return success
  return true;
}

static bool
pwasm_aot_jit_table_set(
  pwasm_env_t * const env,
  pwasm_aot_jit_table_t * const table,
  const size_t ofs,
  const uint32_t * const vals,
  const size_t num_vals
) {
  if (!pwasm_aot_jit_table_grow(env, table, ofs + num_vals)) {
    // return failure
    return false;
  }

  // copy values
  memcpy(table->vals + ofs, vals, num_vals * sizeof(uint32_t));

  // set masks
  for (size_t i = ofs; i < ofs + num_vals; i++) {
    table->masks[i >> 6] |= ((uint64_t) 1) << (i & 0x3F);
  }

  // return success
  return true;
}

/**
 * Get element (u32) from table.
 *
 * At the moment the only supported types are function references, so
 * this returns a u32 offset into the parent environment's funcs table.
 */
static bool
pwasm_aot_jit_table_get_elem(
  pwasm_env_t * const env,
  pwasm_aot_jit_table_t * const table,
  const size_t ofs,
  uint32_t * const ret_val
) {
  // check element offset
  if (ofs >= table->max_vals) {
    D("ofs = %zu, table->max_vals = %zu", ofs, table->max_vals);
    // log error, return failure
    pwasm_env_fail(env, "table element offset out of bounds");
    return false;
  }

  // check element mask
  const bool set = table->masks[ofs >> 6] & ((uint64_t) 1) << (ofs & 0x3F);
  if (!set) {
    // log error, return failure
    pwasm_env_fail(env, "table element is not set");
    return false;
  }

  if (ret_val) {
    // write value to destination
    *ret_val = table->vals[ofs];
  }

  // return success
  return true;
}

#define PWASM_AOT_JIT_VECS \
  PWASM_AOT_JIT_VEC(u32s, uint32_t) \
  PWASM_AOT_JIT_VEC(mods, pwasm_aot_jit_mod_t) \
  PWASM_AOT_JIT_VEC(funcs, pwasm_aot_jit_func_t) \
  PWASM_AOT_JIT_VEC(globals, pwasm_env_global_t) \
  PWASM_AOT_JIT_VEC(mems, pwasm_env_mem_t) \
  PWASM_AOT_JIT_VEC(tables, pwasm_aot_jit_table_t)

typedef struct {
  #define PWASM_AOT_JIT_VEC(NAME, TYPE) pwasm_vec_t NAME;
  PWASM_AOT_JIT_VECS
  #undef PWASM_AOT_JIT_VEC
  pwasm_ctrl_stack_t ctrl_stack;
} pwasm_aot_jit_t;

typedef struct {
  pwasm_env_t * const env;
  pwasm_aot_jit_mod_t * const mod;

  // memory for this frame
  const uint32_t mem_id;

  // function parameters
  const pwasm_slice_t params;

  // offset and length of locals on the stack
  // NOTE: the offset and length include function parameters
  pwasm_slice_t locals;
} pwasm_aot_jit_frame_t;

static bool
pwasm_aot_jit_init(
  pwasm_env_t * const env
) {
  const size_t interp_size = sizeof(pwasm_aot_jit_t);
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // allocate interpreter data store
  pwasm_aot_jit_t *interp = pwasm_realloc(mem_ctx, NULL, interp_size);
  if (!interp) {
    // log error, return failure
    D("pwasm_realloc() failed (size = %zu)", interp_size);
    pwasm_env_fail(env, "interpreter memory allocation failed");
    return false;
  }

  #define PWASM_AOT_JIT_VEC(NAME, TYPE) \
    /* allocate vector, check for error */ \
    if (!pwasm_vec_init(mem_ctx, &(interp->NAME), sizeof(TYPE))) { \
      /* log error, return failure */ \
      D("pwasm_vec_init() failed (stride = %zu)", sizeof(TYPE)); \
      pwasm_env_fail(env, "interpreter " #NAME " vector init failed"); \
      return false; \
    }
  PWASM_AOT_JIT_VECS
  #undef PWASM_AOT_JIT_VEC

  // init control stack, check for error
  if (!pwasm_ctrl_stack_init(&(interp->ctrl_stack), mem_ctx)) {
    // return failure
    return false;
  }

  // save interpreter, return success
  env->env_data = interp;
  return true;
}

static void
pwasm_aot_jit_fini_tables(
  pwasm_env_t * const env
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const vec = &(interp->tables);
  pwasm_aot_jit_table_t *rows = (pwasm_aot_jit_table_t*) pwasm_vec_get_data(vec);
  const size_t num_rows = pwasm_vec_get_size(vec);

  for (size_t i = 0; i < num_rows; i++) {
    pwasm_aot_jit_table_fini(env, rows + i);
  }
}

static void
pwasm_aot_jit_fini(
  pwasm_env_t * const env
) {
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // get interpreter data
  pwasm_aot_jit_t *data = env->env_data;
  if (!data) {
    return;
  }

  // finalize tables
  pwasm_aot_jit_fini_tables(env);

  // fini control stack, check for error
  pwasm_ctrl_stack_fini(&(data->ctrl_stack));

  // free vectors
  #define PWASM_AOT_JIT_VEC(NAME, TYPE) pwasm_vec_fini(&(data->NAME));
  PWASM_AOT_JIT_VECS
  #undef PWASM_AOT_JIT_VEC

  // free backing data
  pwasm_realloc(mem_ctx, data, 0);
  env->env_data = NULL;
}

/**
 * Given an environment and a table handle, return a pointer to the
 * table instance.
 *
 * Returns `NULL` if the table handle is invalid.
 */
static pwasm_aot_jit_table_t *
pwasm_aot_jit_get_table(
  pwasm_env_t * const env,
  const uint32_t table_id
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const vec = &(interp->tables);
  const pwasm_aot_jit_table_t * const rows = pwasm_vec_get_data(vec);
  const size_t num_rows = pwasm_vec_get_size(vec);

  // check that table_id is in bounds
  if (!table_id || table_id > num_rows) {
    // log error, return failure
    D("bad table_id: %u, num_rows = %zu", table_id, num_rows);
    pwasm_env_fail(env, "interpreter table index out of bounds");
    return NULL;
  }

  // return pointer to table
  return (pwasm_aot_jit_table_t*) rows + (table_id - 1);
}

static bool
pwasm_aot_jit_push_u32s(
  pwasm_env_t * const env,
  const size_t ofs,
  const size_t len
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const u32s = &(interp->u32s);
  const pwasm_slice_t slice = { ofs, len };

  uint32_t ids[PWASM_BATCH_SIZE];
  size_t num = 0;

  for (size_t i = 0; i < slice.len; i++) {
    ids[num++] = slice.ofs + i;

    if (num == LEN(ids)) {
      // clear count
      num = 0;

      // append results, check for error
      if (!pwasm_vec_push(u32s, LEN(ids), ids, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append offsets failed");
        return false;
      }
    }
  }

  if (num > 0) {
    // append results, check for error
    if (!pwasm_vec_push(u32s, num, ids, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining offsets failed");
      return false;
    }
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_add_native_funcs(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_native_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->funcs);
  const size_t dst_ofs = pwasm_vec_get_size(dst);
  const size_t u32s_ofs = pwasm_vec_get_size(&(interp->u32s));

  pwasm_aot_jit_func_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_funcs; i++) {
    tmp[tmp_ofs++] = (pwasm_aot_jit_func_t) {
      .mod_ofs  = mod_ofs,
      .func_ofs = i,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append native functions failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native functions failed");
      return false;
    }
  }

  // add IDs
  if (!pwasm_aot_jit_push_u32s(env, u32s_ofs, mod->num_funcs)) {
    // return failure
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = dst_ofs,
    .len = mod->num_funcs,
  };

  // return success
  return true;
}

static bool
pwasm_aot_jit_add_native_globals(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_native_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->globals);
  const size_t dst_ofs = pwasm_vec_get_size(dst);
  const size_t u32s_ofs = pwasm_vec_get_size(&(interp->u32s));
  (void) mod_ofs;

  pwasm_env_global_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_globals; i++) {
    tmp[tmp_ofs++] = (pwasm_env_global_t) {
      .type = mod->globals[i].type,
      .val  = mod->globals[i].val,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append native globals failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native globals failed");
      return false;
    }
  }

  // add IDs
  if (!pwasm_aot_jit_push_u32s(env, u32s_ofs, mod->num_globals)) {
    // return failure
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = dst_ofs,
    .len = mod->num_globals,
  };

  // return success
  return true;
}

static bool
pwasm_aot_jit_add_native_mems(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_native_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->mems);
  const size_t dst_ofs = pwasm_vec_get_size(dst);
  (void) mod_ofs;

  pwasm_env_mem_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_mems; i++) {
    tmp[tmp_ofs++] = (pwasm_env_mem_t) {
      .buf    = mod->mems[i].buf,
      .limits = mod->mems[i].limits,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append native mems failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native mems failed");
      return false;
    }
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = dst_ofs,
    .len = mod->num_mems,
  };

  // return success
  return true;
}

static uint32_t
pwasm_aot_jit_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const size_t mod_ofs = pwasm_vec_get_size(&(interp->mods));

  // add native functions, check for error
  pwasm_slice_t funcs;
  if (!pwasm_aot_jit_add_native_funcs(env, mod_ofs, mod, &funcs)) {
    // return failure
    return 0;
  }

  // add native globals, check for error
  pwasm_slice_t globals;
  if (!pwasm_aot_jit_add_native_globals(env, mod_ofs, mod, &globals)) {
    // return failure
    return 0;
  }

  // add native mems, check for error
  pwasm_slice_t mems;
  if (!pwasm_aot_jit_add_native_mems(env, mod_ofs, mod, &mems)) {
    // return failure
    return 0;
  }

  // build row
  const pwasm_aot_jit_mod_t interp_mod = {
    .type     = PWASM_AOT_JIT_MOD_TYPE_NATIVE,
    .name     = pwasm_buf_str(name),
    .native   = mod,

    .funcs    = funcs,
    .globals  = globals,
    .mems     = mems,
  };

  // append native mod, check for error
  if (!pwasm_vec_push(&(interp->mods), 1, &interp_mod, NULL)) {
    // log error, return failure
    pwasm_env_fail(env, "append native mod failed");
    return 0;
  }

  // convert offset to ID by adding 1
  return mod_ofs + 1;
}

/*
 * Resolve imports of a given type.
 *
 * On success, a slice of import IDs is stored in +ret+, and this
 * function returns true.
 *
 * Returns false on error.
 */
static bool
pwasm_aot_jit_add_mod_imports(
  pwasm_env_t * const env,
  const pwasm_mod_t * const mod,
  const pwasm_import_type_t type,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const u32s = &(interp->u32s);
  const size_t ret_ofs = pwasm_vec_get_size(u32s);

  uint32_t ids[PWASM_BATCH_SIZE];
  size_t num_ids = 0;

  // loop over imports and resolve each one
  for (size_t i = 0; i < mod->num_imports; i++) {
    // get import, check type
    const pwasm_import_t import = mod->imports[i];
    if (import.type != type) {
      continue;
    }

    const pwasm_buf_t mod_buf = {
      .ptr = mod->bytes + import.module.ofs,
      .len = import.module.len,
    };

    // find mod, check for error
    const uint32_t mod_id = pwasm_env_find_mod(env, mod_buf);
    if (!mod_id) {
      // return failure
      return false;
    }

    const pwasm_buf_t name_buf = {
      .ptr = mod->bytes + import.name.ofs,
      .len = import.name.len,
    };

    // find import ID, check for error
    const uint32_t id = pwasm_env_find_import(env, mod_id, import.type, name_buf);
    if (!id) {
      // return failure
      return false;
    }

    // add item to results, increment count
    ids[num_ids++] = id;

    if (num_ids == LEN(ids)) {
      // clear count
      num_ids = 0;

      // append results, check for error
      if (!pwasm_vec_push(u32s, LEN(ids), ids, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append import ids failed");
        return false;
      }
    }
  }

  if (num_ids > 0) {
    // append remaining results
    if (!pwasm_vec_push(u32s, num_ids, ids, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native imports failed");
      return false;
    }
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = ret_ofs,
    .len = pwasm_vec_get_size(u32s) - ret_ofs,
  };

  // return success
  return true;
}

static bool
pwasm_aot_jit_add_mod_funcs(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->funcs);
  const size_t funcs_ofs = pwasm_vec_get_size(dst);

  // add imported functions, check for error
  pwasm_slice_t imports;
  if (!pwasm_aot_jit_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_FUNC, &imports)) {
    return false;
  }

  pwasm_aot_jit_func_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_funcs; i++) {
    tmp[tmp_ofs++] = (pwasm_aot_jit_func_t) {
      .mod_ofs  = mod_ofs,
      .func_ofs = i,
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append functions failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining functions failed");
      return false;
    }
  }

  if (!pwasm_aot_jit_push_u32s(env, funcs_ofs, mod->num_funcs)) {
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_funcs,
  };

  // return success
  return true;
}

static bool
pwasm_aot_jit_add_mod_globals(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->globals);
  const size_t globals_ofs = pwasm_vec_get_size(dst);
  (void) mod_ofs;

  // add imported globals, check for error
  pwasm_slice_t imports;
  if (!pwasm_aot_jit_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_GLOBAL, &imports)) {
    return false;
  }

  pwasm_env_global_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_globals; i++) {
    tmp[tmp_ofs++] = (pwasm_env_global_t) {
      .type  = mod->globals[i].type,
      // .val = // FIXME: uninitialized
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append globals failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining globals failed");
      return false;
    }
  }

  // add IDs
  if (!pwasm_aot_jit_push_u32s(env, globals_ofs, mod->num_globals)) {
    // return failure
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_globals,
  };

  // return success
  return true;
}

static bool
pwasm_aot_jit_add_mod_mems(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->mems);
  const size_t mems_ofs = pwasm_vec_get_size(dst);
  (void) mod_ofs;

  // add imported mems, check for error
  pwasm_slice_t imports;
  if (!pwasm_aot_jit_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_MEM, &imports)) {
    return false;
  }

  pwasm_env_mem_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_mems; i++) {
    // allocate buffer
    const size_t num_bytes = mod->mems[i].min * PWASM_PAGE_SIZE;
    uint8_t * const ptr = pwasm_realloc(env->mem_ctx, NULL, num_bytes);
    if (!ptr && num_bytes) {
      // log error, return failure
      pwasm_env_fail(env, "allocate memory buffer failed");
      return false;
    }

    tmp[tmp_ofs++] = (pwasm_env_mem_t) {
      .limits = mod->mems[i],
      .buf = (pwasm_buf_t) {
        .ptr = ptr,
        .len = num_bytes,
      },
    };

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append mems failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining mems failed");
      return false;
    }
  }

  if (!pwasm_aot_jit_push_u32s(env, mems_ofs, mod->num_mems)) {
    return false;
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_mems,
  };

  // return success
  return true;
}

static bool
pwasm_aot_jit_add_mod_tables(
  pwasm_env_t * const env,
  const uint32_t mod_ofs,
  const pwasm_mod_t * const mod,
  pwasm_slice_t * const ret
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_vec_t * const dst = &(interp->tables);
  pwasm_vec_t * const u32s = &(interp->u32s);

  // add imported tables, check for error
  pwasm_slice_t imports = { 0, 0 };
  if (!pwasm_aot_jit_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_TABLE, &imports)) {
    return false;
  }

  uint32_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_tables; i++) {
    // init table
    pwasm_aot_jit_table_t table = pwasm_aot_jit_table_init(env, mod_ofs, i, mod->tables[i].limits);

    // append initialized table to interpreter, check for error
    size_t tmp_pos;
    if (!pwasm_vec_push(dst, 1, &table, &tmp_pos)) {
      // log error, return failure
      pwasm_env_fail(env, "append tables failed");
      return false;
    }

    // append to table offsets, increment count
    tmp[tmp_ofs] = tmp_pos;
    tmp_ofs++;

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append IDs, check for error
      if (!pwasm_vec_push(u32s, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append table ids failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // flush IDs, check for error
    if (!pwasm_vec_push(u32s, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining table IDs failed");
      return false;
    }
  }

  // populate result
  *ret = (pwasm_slice_t) {
    .ofs = imports.ofs,
    .len = imports.len + mod->num_tables,
  };

  // return success
  return true;
}

// forward declarations
static bool pwasm_aot_jit_eval_expr(
  pwasm_aot_jit_frame_t frame,
  const pwasm_slice_t
);

static bool pwasm_aot_jit_call(pwasm_env_t *, const uint32_t);

static bool
pwasm_aot_jit_init_globals(
  pwasm_aot_jit_frame_t frame
) {
  pwasm_aot_jit_t * const interp = frame.env->env_data;
  pwasm_env_global_t *env_globals = (pwasm_env_global_t*) pwasm_vec_get_data(&(interp->globals));
  pwasm_stack_t * const stack = frame.env->stack;
  const pwasm_global_t * const mod_globals = frame.mod->mod->globals;
  const uint32_t * interp_u32s = (uint32_t*) pwasm_vec_get_data(&(interp->u32s)) + frame.mod->globals.ofs;
  const size_t num_globals = frame.mod->mod->num_globals;
  const pwasm_val_t zero = { .i64 = 0 };

  for (size_t i = 0; i < num_globals; i++) {
    // clear stack
    stack->pos = 0;

    // evaluate init
    if (!pwasm_aot_jit_eval_expr(frame, mod_globals[i].expr)) {
      // return failure
      return false;
    }

    // get destination offset, save value to global
    const uint32_t ofs = interp_u32s[i];
    env_globals[ofs].val = stack->pos ? stack->ptr[0] : zero;
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_init_elem_funcs(
  pwasm_aot_jit_frame_t frame,
  const uint32_t ofs,
  const pwasm_elem_t elem
) {
  // get table and mod to interpreter func ID map
  pwasm_aot_jit_t * const interp = frame.env->env_data;
  const uint32_t *u32s = pwasm_vec_get_data(&(interp->u32s));
  const uint32_t table_ofs = u32s[frame.mod->tables.ofs + elem.table_id];
  D("frame.mod->tables.ofs = %zu, elem.table_id = %u, table_ofs = %u", frame.mod->tables.ofs, elem.table_id, table_ofs);
  const uint32_t * const funcs = u32s + frame.mod->funcs.ofs;
  pwasm_aot_jit_table_t * const table = pwasm_aot_jit_get_table(frame.env, table_ofs + 1);
  if (!table) {
    return false;
  }

  uint32_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < elem.funcs.len; i++) {
    // remap function id from module ID to interpreter ID
    const uint32_t func_id = frame.mod->mod->u32s[elem.funcs.ofs + i];
    tmp[tmp_ofs++] = funcs[func_id];

    if (tmp_ofs == LEN(tmp)) {
      // get destination offset
      const size_t dst_ofs = ofs + i - LEN(tmp);

      // set table elements, check for error
      if (!pwasm_aot_jit_table_set(frame.env, table, dst_ofs, tmp, LEN(tmp))) {
        // return failure
        return false;
      }

      // reset offset
      tmp_ofs = 0;
    }
  }

  if (tmp_ofs > 0) {
    // get destination offset
    const size_t dst_ofs = ofs + elem.funcs.len - tmp_ofs;

    // flush table elements, check for error
    if (!pwasm_aot_jit_table_set(frame.env, table, dst_ofs, tmp, tmp_ofs)) {
      // return failure
      return false;
    }
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_init_elems(
  pwasm_aot_jit_frame_t frame
) {
  const pwasm_elem_t * const elems = frame.mod->mod->elems;
  const size_t num_elems = frame.mod->mod->num_elems;
  pwasm_stack_t * const stack = frame.env->stack;

  // init table elements
  for (size_t i = 0; i < num_elems; i++) {
    const pwasm_elem_t elem = elems[i];

    // evaluate offset expression, check for error
    stack->pos = 0;
    if (!pwasm_aot_jit_eval_expr(frame, elem.expr)) {
      // return failure
      return false;
    }

    // check that const expr has at least one result
    // FIXME: should this be done in check()?
    if (!stack->pos) {
      pwasm_env_fail(frame.env, "constant expression must return table offset");
      return false;
    }

    // get destination offset
    const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

    // populate table elements, check for error
    if (!pwasm_aot_jit_init_elem_funcs(frame, ofs, elem)) {
      // return failure
      return false;
    }
  }

  // return success
  return true;
}

// forward reference
static pwasm_env_mem_t *pwasm_aot_jit_get_mem(pwasm_env_t * const, const uint32_t);

static bool
pwasm_aot_jit_init_segments(
  pwasm_aot_jit_frame_t frame
) {
  pwasm_aot_jit_t * const interp = frame.env->env_data;
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));
  const pwasm_segment_t * const segments = frame.mod->mod->segments;
  const size_t num_segments = frame.mod->mod->num_segments;
  pwasm_stack_t * const stack = frame.env->stack;

  // init memory elements
  for (size_t i = 0; i < num_segments; i++) {
    const pwasm_segment_t segment = segments[i];

    // get interpreter memory ID
    // FIXME
    const uint32_t real_mem_id = u32s[frame.mod->mems.ofs + segment.mem_id];
    const uint32_t mem_id = 1;
    #ifndef PWASM_DEBUG
    (void) real_mem_id
    #endif /* !PWASM_DEBUG */
    D("frame.mod->mems.ofs = %zu, segment.mem_id = %u, real_mem_id = %u", frame.mod->mems.ofs, segment.mem_id, real_mem_id);

    // get memory, check for error
    pwasm_env_mem_t * const mem = pwasm_aot_jit_get_mem(frame.env, mem_id);
    if (!mem) {
      // return failure
      return false;
    }

    // evaluate offset expression, check for error
    stack->pos = 0;
    if (!pwasm_aot_jit_eval_expr(frame, segment.expr)) {
      // return failure
      return false;
    }

    // check that const expr has at least one result
    if (!stack->pos) {
      pwasm_env_fail(frame.env, "constant expression must return segment offset");
      return false;
    }

    // get destination offset
    const uint32_t dst_ofs = stack->ptr[stack->pos - 1].i32;

    // check destination length
    // FIXME: should we resize here?
    if (dst_ofs + segment.data.len > mem->buf.len) {
      pwasm_env_fail(frame.env, "segment destination out of bounds");
      return false;
    }

    // D("mem = %u, ofs = %u, len = %zu", mem_id, dst_ofs, segment.data.len);

    // get dst, src pointers
    uint8_t * const dst = (uint8_t*) mem->buf.ptr + dst_ofs;
    const uint8_t * const src = frame.mod->mod->bytes + segment.data.ofs;

    // copy data
    memcpy(dst, src, segment.data.len);
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_compile_func(
  pwasm_buf_t * const dst,
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const size_t func_ofs
) {
  pwasm_jit_t *jit = env->cbs->jit;
  if (!jit) {
    // log error, return failure
    pwasm_env_fail(env, "JIT == NULL, cannot compile function");
    return NULL;
  }

  return pwasm_jit_compile(jit, dst, env, mod_id, func_ofs);
}

static bool
pwasm_aot_jit_compile_funcs(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_mod_t * const mod,
  pwasm_buf_t ** const dst
) {
  const size_t num_codes = mod->num_codes;
  const size_t num_bytes = num_codes * sizeof(pwasm_buf_t);

  D("num_codes = %zu, num_bytes = %zu", num_codes, num_bytes);
  if (num_bytes > 0) {
    // allocate function pointers
    pwasm_buf_t *fns = pwasm_realloc(env->mem_ctx, NULL, num_bytes);
    if (!fns) {
      pwasm_env_fail(env, "allocate function pointer list failed");
      return NULL;
    }

    // walk/compile functions
    for (size_t i = 0; i < num_codes; i++) {
      // compile function, check for error
      if (!pwasm_aot_jit_compile_func(fns + i, env, mod_id, i)) {
        // return failure
        return false;
      }
    }

    // save compiled function pointers (hack)
    *dst = fns;
  } else {
    // no functions, save null pointer
    *dst = NULL;
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_init_start(
  pwasm_aot_jit_frame_t frame
) {
  // check for start function
  if (!frame.mod->mod->has_start) {
    // return success
    return true;
  }

  // get start function ID in mod, map to interpreter func ID
  pwasm_aot_jit_t * const interp = frame.env->env_data;
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));
  const uint32_t start = frame.mod->mod->start;
  const uint32_t func_id = u32s[frame.mod->funcs.ofs + start] + 1;

  // call start function, return result
  return pwasm_aot_jit_call(frame.env, func_id);
}

static uint32_t
pwasm_aot_jit_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const size_t mod_ofs = pwasm_vec_get_size(&(interp->mods));

  // add mod funcs, check for error
  pwasm_slice_t funcs;
  if (!pwasm_aot_jit_add_mod_funcs(env, mod_ofs, mod, &funcs)) {
    // return failure
    return 0;
  }

  // add mod globals, check for error
  pwasm_slice_t globals;
  if (!pwasm_aot_jit_add_mod_globals(env, mod_ofs, mod, &globals)) {
    // return failure
    return 0;
  }

  // add mod mems, check for error
  pwasm_slice_t mems;
  if (!pwasm_aot_jit_add_mod_mems(env, mod_ofs, mod, &mems)) {
    // return failure
    return 0;
  }

  // add mod tables, check for error
  pwasm_slice_t tables;
  if (!pwasm_aot_jit_add_mod_tables(env, mod_ofs, mod, &tables)) {
    // return failure
    return 0;
  }

  // build mod instance
  pwasm_aot_jit_mod_t interp_mod = {
    .type     = PWASM_AOT_JIT_MOD_TYPE_MOD,
    .name     = pwasm_buf_str(name),
    .mod      = mod,

    .funcs    = funcs,
    .globals  = globals,
    .mems     = mems,
    .tables   = tables,
  };

  // append mod, check for error
  size_t interp_mod_ofs;
  if (!pwasm_vec_push(&(interp->mods), 1, &interp_mod, &interp_mod_ofs)) {
    // log error, return failure
    pwasm_env_fail(env, "append mod failed");
    return 0;
  }

  // convert offset to ID by adding 1
  const uint32_t ret_mod_id = mod_ofs + 1;

  pwasm_aot_jit_mod_t * const dst_interp_mod = (pwasm_aot_jit_mod_t*) pwasm_vec_get_data(&(interp->mods)) + interp_mod_ofs;

  // set up a temporary frame to init globals, tables, and mems
  pwasm_aot_jit_frame_t frame = {
    .env = env,
    .mod = &interp_mod,
  };

  // init globals, mems, tables, and start (in that order)
  // source: https://webassembly.github.io/spec/core/exec/modules.html#exec-instantiation)

  // init globals, check for error
  if (!pwasm_aot_jit_init_globals(frame)) {
    // return failure
    return 0;
  }

  // init tables, check for error
  if (!pwasm_aot_jit_init_elems(frame)) {
    // return failure
    return 0;
  }

  // init segments, check for error
  if (!pwasm_aot_jit_init_segments(frame)) {
    // return failure
    return 0;
  }

  // compile funcs, check for error
  pwasm_buf_t *fns = NULL;
  if (!pwasm_aot_jit_compile_funcs(env, ret_mod_id, mod, &fns)) {
    // return failure
    return 0;
  }

  // save compiled functions
  dst_interp_mod->fns = fns;

  // call start func, check for error
  if (!pwasm_aot_jit_init_start(frame)) {
    // return failure
    return 0;
  }

  // return success (non-zero mod ID)
  return ret_mod_id;
}

static const pwasm_mod_t *
pwasm_aot_jit_get_mod(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_aot_jit_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  const bool ok = (mod_id && mod_id <= num_rows);
  return ok ? rows[mod_id - 1].mod : NULL;
}

static const pwasm_buf_t *
pwasm_aot_jit_get_mod_name(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_aot_jit_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  const bool ok = (mod_id && mod_id <= num_rows);
  return ok ? &(rows[mod_id - 1].name) : NULL;
}

static uint32_t
pwasm_aot_jit_find_mod(
  pwasm_env_t * const env,
  const pwasm_buf_t name
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_aot_jit_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  for (size_t i = 0; i < num_rows; i++) {
    if (
      (rows[i].name.len == name.len) &&
      !memcmp(name.ptr, rows[i].name.ptr, name.len)
    ) {
      // return offset + 1 (prevent zero IDs)
      return i + 1;
    }
  }

  // log error, return failure
  pwasm_env_fail(env, "module not found");
  return 0;
}

static uint32_t
pwasm_aot_jit_find_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_vec_t * const vec = &(interp->mods);
  const pwasm_aot_jit_mod_t * const mods = pwasm_vec_get_data(vec);
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));

  // check mod_id
  if (mod_id > pwasm_vec_get_size(&(interp->mods))) {
    pwasm_env_fail(env, "invalid mod ID");
    return 0;
  }

  // get mod
  const pwasm_aot_jit_mod_t mod = mods[mod_id - 1];

  switch (mod.type) {
  case PWASM_AOT_JIT_MOD_TYPE_MOD:
    for (size_t i = 0; i < mod.mod->num_exports; i++) {
      const pwasm_export_t row = mod.mod->exports[i];

      if (
        (row.type == PWASM_IMPORT_TYPE_FUNC) &&
        (row.name.len == name.len) &&
        !memcmp(mod.mod->bytes + row.name.ofs, name.ptr, name.len)
      ) {
        // return offset + 1 (prevent zero IDs)
        return u32s[mod.funcs.ofs + row.id] + 1;
      }
    }

    break;
  case PWASM_AOT_JIT_MOD_TYPE_NATIVE:
    for (size_t i = 0; i < mod.native->num_funcs; i++) {
      const pwasm_native_func_t row = mod.native->funcs[i];
      const pwasm_buf_t row_buf = pwasm_buf_str(row.name);

      if (
        row_buf.ptr &&
        (row_buf.len == name.len) &&
        !memcmp(row_buf.ptr, name.ptr, name.len)
      ) {
        D("u32s = %p, mod->funcs.ofs = %zu, i = %zu", (void*) u32s, mod.funcs.ofs, i);
        // return offset + 1 (prevent zero IDs)
        return u32s[mod.funcs.ofs + i] + 1;
      }
    }

    break;
  default:
    // log error, return failure
    pwasm_env_fail(env, "unknown module type (bug?)");
    return 0;
  }

  // log error, return failure
  pwasm_env_fail(env, "function not found");
  return 0;
}

static uint32_t
pwasm_aot_jit_find_mem(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_vec_t * const vec = &(interp->mods);
  const pwasm_aot_jit_mod_t * const mods = pwasm_vec_get_data(vec);
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));

  // check mod_id
  if (mod_id > pwasm_vec_get_size(&(interp->mods))) {
    pwasm_env_fail(env, "invalid mod ID");
    return 0;
  }

  // get mod
  const pwasm_aot_jit_mod_t * const mod = mods + (mod_id - 1);

  switch (mod->type) {
  case PWASM_AOT_JIT_MOD_TYPE_MOD:
    for (size_t i = 0; i < mod->mod->num_exports; i++) {
      const pwasm_export_t row = mod->mod->exports[i];

      if (
        (row.type == PWASM_IMPORT_TYPE_MEM) &&
        (row.name.len == name.len) &&
        !memcmp(mod->mod->bytes + row.name.ofs, name.ptr, name.len)
      ) {
        // return offset + 1 (prevent zero IDs)
        return u32s[mod->mems.ofs + row.id] + 1;
      }
    }

    break;
  case PWASM_AOT_JIT_MOD_TYPE_NATIVE:
    for (size_t i = 0; i < mod->native->num_funcs; i++) {
      const pwasm_native_mem_t row = mod->native->mems[i];
      const pwasm_buf_t row_buf = pwasm_buf_str(row.name);

      if (
        row_buf.ptr &&
        (row_buf.len == name.len) &&
        !memcmp(row_buf.ptr, name.ptr, name.len)
      ) {
        // return offset + 1 (prevent zero IDs)
        return u32s[mod->mems.ofs + i] + 1;
      }
    }

    break;
  default:
    // log error, return failure
    pwasm_env_fail(env, "unknown module type (bug?)");
    return 0;
  }

  // log error, return failure
  pwasm_env_fail(env, "memory not found");
  return 0;
}

static pwasm_env_mem_t *
pwasm_aot_jit_get_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_env_mem_t *rows = pwasm_vec_get_data(&(interp->mems));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mems));
  D("num mems: %zu", num_rows);

  // check that mem_id is in bounds
  if (!mem_id || mem_id > num_rows) {
    // log error, return failure
    D("bad mem_id: %u", mem_id);
    pwasm_env_fail(env, "memory index out of bounds");
    return NULL;
  }

  // return pointer to memory
  return (pwasm_env_mem_t*) rows + (mem_id - 1);
}

/*
 * Get the absolute memory offset from the immediate offset and the
 * offset operand.
 */
static bool
pwasm_aot_jit_get_mem_chunk(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_aot_jit_mem_chunk_t * const ret
) {
  pwasm_env_mem_t * const mem = pwasm_aot_jit_get_mem(env, mem_id);
  size_t ofs = in.v_mem.offset + arg_ofs;
  size_t size = pwasm_op_get_num_bytes(in.op);
  const bool ok = mem && size && (ofs + size < mem->buf.len);

  if (!ok) {
    // dissect error
    if (!mem) {
      D("mem_id = %u", mem_id);
      pwasm_env_fail(env, "invalid memory index");
    } else if (!size) {
      D("op = %s", pwasm_op_get_name(in.op));
      pwasm_env_fail(env, "invalid memory instruction");
    } else {
      D("ofs = %zu, size = %zu", ofs, size);
      pwasm_env_fail(env, "invalid memory address");
    }

    // return failure
    return false;
  }

  const pwasm_aot_jit_mem_chunk_t tmp = {
    .mem  = mem,
    .ofs  = ofs,
    .size = size,
  };

  // populate result
  memcpy(ret, &tmp, sizeof(pwasm_aot_jit_mem_chunk_t));

  // return success
  return true;
}

static bool
pwasm_aot_jit_mem_load(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_val_t * const ret_val
) {
  // get memory chunk, check for error
  pwasm_aot_jit_mem_chunk_t chunk;
  if (!pwasm_aot_jit_get_mem_chunk(env, mem_id, in, arg_ofs, &chunk)) {
    return false;
  }

  // pwasm_aot_jit_dump_mem_chunk(chunk);
  // D("load i32 = %u", ((pwasm_val_t*) (chunk.mem->buf.ptr + chunk.ofs))->i32);

  // clear temp val
  pwasm_val_t tmp;
  memset(&tmp, 0, sizeof(pwasm_val_t));

  // copy to temp
  memcpy(&tmp, chunk.mem->buf.ptr + chunk.ofs, chunk.size);

  // copy temp to destination
  memcpy(ret_val, &tmp, sizeof(pwasm_val_t));

  // return success
  return true;
}

static bool
pwasm_aot_jit_mem_store(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  const pwasm_val_t val
) {
  // get memory chunk, check for error
  pwasm_aot_jit_mem_chunk_t chunk;
  if (!pwasm_aot_jit_get_mem_chunk(env, mem_id, in, arg_ofs, &chunk)) {
    return false;
  }

  // pwasm_aot_jit_dump_mem_chunk(chunk);
  // D("chunk.size = %zu", chunk.size);

  // copy to result
  memcpy((uint8_t*) chunk.mem->buf.ptr + chunk.ofs, &val, chunk.size);

  // return success
  return true;
}

static bool
pwasm_aot_jit_mem_size(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  uint32_t * const ret_val
) {
  // get memory, check for error
  pwasm_env_mem_t * const mem = pwasm_aot_jit_get_mem(env, mem_id);
  if (!mem) {
    // return failure
    return false;
  }

  if (ret_val) {
    // save result
    *ret_val = mem->buf.len / PWASM_PAGE_SIZE;
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  // get memory, check for error
  pwasm_env_mem_t * const mem = pwasm_aot_jit_get_mem(env, mem_id);
  if (!mem) {
    // return failure (trap)
    return false;
  }

  // get current size, in number of pages
  const uint32_t old_size = mem->buf.len / PWASM_PAGE_SIZE;
  const uint32_t new_size = old_size + grow;

  // check upper bound
  if (mem->limits.has_max && new_size > mem->limits.max) {
    // at this point we save a -1 to the return value pointer to
    // indicate failure, then return true from the function so that it
    // doesn't trap.
    //
    // see https://webassembly.github.io/spec/core/exec/instructions.html#exec-memory-size

    if (ret_val) {
      // return -1 to indicate failure
      *ret_val = -1;
    }

    // return "success"
    return true;
  }

  if (new_size > 0) {
    // get old pointer and number of bytes
    void *old_ptr = (void*) mem->buf.ptr;
    const size_t num_bytes = new_size * PWASM_PAGE_SIZE;

    // resize buffer, check for error
    uint8_t * const ptr = pwasm_realloc(env->mem_ctx, old_ptr, num_bytes);
    if (!ptr && num_bytes) {
      // at this point we save a -1 to the return value pointer to
      // indicate failure, then return true from the function so that it
      // doesn't trap.
      //
      // see https://webassembly.github.io/spec/core/exec/instructions.html#exec-memory-size

      if (ret_val) {
        // return -1 to indicate failure
        *ret_val = -1;
      }

      // return "success"
      return true;
    }

    // update buffer attributes
    mem->buf.ptr = ptr;
    mem->buf.len = num_bytes;
  }

  if (ret_val) {
    // update return value
    *ret_val = old_size;
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_val
) {
  pwasm_aot_jit_table_t * const table = pwasm_aot_jit_get_table(env, table_id);
  if (!table) {
    return false;
  }
  return pwasm_aot_jit_table_get_elem(env, table, elem_ofs, ret_val);
}

static uint32_t
pwasm_aot_jit_find_import(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
) {
  switch (type) {
  case PWASM_IMPORT_TYPE_FUNC:
    return pwasm_env_find_func(env, mod_id, name);
  case PWASM_IMPORT_TYPE_MEM:
    return pwasm_env_find_mem(env, mod_id, name);
  case PWASM_IMPORT_TYPE_GLOBAL:
    return pwasm_env_find_global(env, mod_id, name);
  case PWASM_IMPORT_TYPE_TABLE:
    return pwasm_env_find_table(env, mod_id, name);
  default:
    // log error, return failure
    pwasm_env_fail(env, "invalid import type");
    return 0;
  }
}

static inline bool
pwasm_aot_jit_check_global(
  pwasm_env_t * const env,
  const uint32_t id
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const size_t num_rows = pwasm_vec_get_size(&(interp->globals));

  if (!id || id > num_rows) {
    // log error, return failure
    D("global index = %u", id);
    pwasm_env_fail(env, "global index out of bounds");
    return false;
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_get_global(
  pwasm_env_t * const env,
  const uint32_t id,
  pwasm_val_t * const ret_val
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_env_global_t *rows = pwasm_vec_get_data(&(interp->globals));

  if (!pwasm_aot_jit_check_global(env, id)) {
    return false;
  }

  if (ret_val) {
    // copy value to destination
    *ret_val = rows[id - 1].val;
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_set_global(
  pwasm_env_t * const env,
  const uint32_t id,
  const pwasm_val_t val
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_env_global_t *rows = (pwasm_env_global_t*) pwasm_vec_get_data(&(interp->globals));

  if (!pwasm_aot_jit_check_global(env, id)) {
    return false;
  }

  if (!rows[id - 1].type.mutable) {
    pwasm_env_fail(env, "write to immutable global");
    return false;
  }

  // set global value
  rows[id - 1].val = val;

  // return success
  return true;
}

/*
 * Convert an internal global ID to an externally visible global handle.
 *
 * Returns 0 on error.
 */
static uint32_t
pwasm_aot_jit_mod_get_global_index(
  pwasm_env_t * const env,
  pwasm_aot_jit_mod_t * const mod,
  uint32_t id
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_vec_t * const vec = &(interp->u32s);
  const uint32_t * const u32s = ((uint32_t*) pwasm_vec_get_data(vec)) + mod->globals.ofs;
  return (id < mod->globals.len) ? u32s[id] + 1 : 0;
}

// forward references
static bool pwasm_aot_jit_call_func(pwasm_env_t *, pwasm_aot_jit_mod_t *, uint32_t);
static bool pwasm_aot_jit_call_indirect(pwasm_aot_jit_frame_t, pwasm_inst_t, uint32_t);

static bool
pwasm_aot_jit_eval_expr(
  pwasm_aot_jit_frame_t frame,
  const pwasm_slice_t expr
) {
  pwasm_aot_jit_t * const interp = frame.env->env_data;
  pwasm_ctrl_stack_t * const ctrl_stack = &(interp->ctrl_stack);
  pwasm_stack_t * const stack = frame.env->stack;
  const pwasm_inst_t * const insts = frame.mod->mod->insts + expr.ofs;

  size_t ctrl_depth = 0;

  // D("expr = { .ofs = %zu, .len = %zu }, num_insts = %zu", expr.ofs, expr.len, frame.mod->num_insts);

  for (size_t i = 0; i < expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    // D("0x%02X %s", in.op, pwasm_op_get_name(in.op));

    switch (in.op) {
    case PWASM_OP_UNREACHABLE:
      // FIXME: raise trap?
      pwasm_env_fail(frame.env, "unreachable instruction reached");
      return false;
    case PWASM_OP_NOP:
      // do nothing
      break;
    case PWASM_OP_BLOCK:
      {
        // get block.params.size, check for error
        size_t num_params;
        if (!pwasm_block_type_params_get_size(frame.mod->mod, in.v_block.block_type, &num_params)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "block: get num block params failed");
          return false;
        }

        // create control stack entry
        const pwasm_ctrl_stack_entry_t ctrl_entry = {
          .type   = CTRL_BLOCK,
          .depth  = stack->pos - num_params,
          .ofs    = i,
        };

        // push control stack entry, check for error
        if (!pwasm_ctrl_stack_push(ctrl_stack, ctrl_entry)) {
          pwasm_env_fail(frame.env, "block: ctrl_stack_push failed");
          return false;
        }

        // increment control depth
        ctrl_depth++;
      }

      break;
    case PWASM_OP_LOOP:
      {
        // get block.params.size, check for error
        size_t num_params;
        if (!pwasm_block_type_params_get_size(frame.mod->mod, in.v_block.block_type, &num_params)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "block: get num block params failed");
          return false;
        }

        // create control stack entry
        const pwasm_ctrl_stack_entry_t ctrl_entry = {
          .type   = CTRL_LOOP,
          .depth  = stack->pos - num_params,
          .ofs    = i,
        };

        // push control stack entry, check for error
        if (!pwasm_ctrl_stack_push(ctrl_stack, ctrl_entry)) {
          pwasm_env_fail(frame.env, "block: ctrl_stack_push failed");
          return false;
        }

        // increment control depth
        ctrl_depth++;
      }

      break;
    case PWASM_OP_IF:
      {
        // pop last value from value stack
        const uint32_t tail = stack->ptr[--stack->pos].i32;

        // get else/end offset
        const size_t else_ofs = in.v_block.else_ofs ? in.v_block.else_ofs : in.v_block.end_ofs;

        // get block.params.size, check for error
        size_t num_params;
        if (!pwasm_block_type_params_get_size(frame.mod->mod, in.v_block.block_type, &num_params)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "block: get num block params failed");
          return false;
        }

        // create control stack entry
        const pwasm_ctrl_stack_entry_t ctrl_entry = {
          .type   = CTRL_IF,
          .depth  = stack->pos - num_params,
          .ofs    = i,
        };

        // push control stack entry, check for error
        if (!pwasm_ctrl_stack_push(ctrl_stack, ctrl_entry)) {
          pwasm_env_fail(frame.env, "block: ctrl_stack_push failed");
          return false;
        }

        // increment control depth
        ctrl_depth++;

        // increment instruction pointer
        i += tail ? 0 : else_ofs;
      }

      break;
    case PWASM_OP_ELSE:
      // skip to end inst
      i += insts[i].v_block.end_ofs - 1;

      break;
    case PWASM_OP_END:
      if (ctrl_depth) {
        // pop control stack, check for error
        pwasm_ctrl_stack_entry_t ctrl_tail;
        if (!pwasm_ctrl_stack_pop(ctrl_stack, &ctrl_tail)) {
          pwasm_env_fail(frame.env, "end: ctrl_stack pop failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth--;

        // get mod, block type
        const pwasm_mod_t * const mod = frame.mod->mod;
        const int32_t block_type = insts[ctrl_tail.ofs].v_block.block_type;

        // get block type result count, check for error
        size_t num_results;
        if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "end: get block num_results failed");
          return false;
        }

        // pop results
        for (size_t j = 0; j < num_results; j++) {
          // calculate stack source and destination offsets
          const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
          const size_t dst_ofs = ctrl_tail.depth + j;
          stack->ptr[dst_ofs] = stack->ptr[src_ofs];
        }

        // reset value stack
        stack->pos = ctrl_tail.depth + num_results;
      } else {
        // return success
        // FIXME: is this correct?
        return true;
      }

      break;
    case PWASM_OP_BR:
      {
        // pop control stack, check for error
        if (!pwasm_ctrl_stack_popn(ctrl_stack, in.v_index)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "br: ctrl_stack_popn failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth -= in.v_index;
        if (!ctrl_depth) {
          // return success
          // FIXME: is this correct?
          return true;
        }

        // get control stack tail, check for error
        const pwasm_ctrl_stack_entry_t *ctrl_tail = pwasm_ctrl_stack_peek_tail(ctrl_stack, 0);
        if (!ctrl_tail) {
          pwasm_env_fail(frame.env, "br: ctrl_stack_peek_tail failed");
          return false;
        }

        if (ctrl_tail->type == CTRL_LOOP) {
          // reset control stack
          i = ctrl_tail->ofs;
          stack->pos = ctrl_tail->depth;
        } else {
          // get mod, block type
          const pwasm_mod_t * const mod = frame.mod->mod;
          const int32_t block_type = insts[ctrl_tail->ofs].v_block.block_type;

          // get block type result count, check for error
          size_t num_results;
          if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: get block num_results failed");
            return false;
          }

          // pop results
          for (size_t j = 0; j < num_results; j++) {
            // calculate stack source and destination offsets
            const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
            const size_t dst_ofs = ctrl_tail->depth + 1 + j;
            stack->ptr[dst_ofs] = stack->ptr[src_ofs];
          }

          // reset value stack
          stack->pos = ctrl_tail->depth + num_results;

          // pop control stack, check for error
          if (!pwasm_ctrl_stack_pop(ctrl_stack, NULL)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: ctrl_stack_pop failed");
            return false;
          }

          // decriment control stack depth
          ctrl_depth--;
        }
      }

      break;
    case PWASM_OP_BR_IF:
      if (stack->ptr[--stack->pos].i32) {
        // pop control stack, check for error
        if (!pwasm_ctrl_stack_popn(ctrl_stack, in.v_index)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_popn failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth -= in.v_index;
        if (!ctrl_depth) {
          // return success
          // FIXME: is this correct?
          return true;
        }

        // get control stack tail, check for error
        const pwasm_ctrl_stack_entry_t *ctrl_tail = pwasm_ctrl_stack_peek_tail(ctrl_stack, 0);
        if (!ctrl_tail) {
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_peek_tail failed");
          return false;
        }

        if (ctrl_tail->type == CTRL_LOOP) {
          i = ctrl_tail->ofs;
          stack->pos = ctrl_tail->depth;
        } else {
          // get mod, block type
          const pwasm_mod_t * const mod = frame.mod->mod;
          const int32_t block_type = insts[ctrl_tail->ofs].v_block.block_type;

          // get block type result count, check for error
          size_t num_results;
          if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br_if: get block num_results failed");
            return false;
          }

          // pop results
          for (size_t j = 0; j < num_results; j++) {
            // calculate stack source and destination offsets
            const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
            const size_t dst_ofs = ctrl_tail->depth + 1 + j;
            stack->ptr[dst_ofs] = stack->ptr[src_ofs];
          }

          // reset value stack
          stack->pos = ctrl_tail->depth + num_results;

          // pop control stack, check for error
          if (!pwasm_ctrl_stack_pop(ctrl_stack, NULL)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: ctrl_stack_pop failed");
            return false;
          }

          // decriment control stack depth
          ctrl_depth--;
        }
      }

      break;
    case PWASM_OP_BR_TABLE:
      {
        // get value from stack, branch labels, label offset, and then index
        const uint32_t val = stack->ptr[--stack->pos].i32;
        const pwasm_slice_t labels = in.v_br_table;
        const size_t labels_ofs = labels.ofs + MIN(val, labels.len - 1);
        const uint32_t id = frame.mod->mod->u32s[labels_ofs];

        // pop control stack, check for error
        if (!pwasm_ctrl_stack_popn(ctrl_stack, id)) {
          // log error, return failure
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_popn failed");
          return false;
        }

        // decriment control stack depth
        ctrl_depth -= id;
        if (!ctrl_depth) {
          // return success
          // FIXME: is this correct?
          return true;
        }

        // get control stack tail, check for error
        const pwasm_ctrl_stack_entry_t *ctrl_tail = pwasm_ctrl_stack_peek_tail(ctrl_stack, 0);
        if (!ctrl_tail) {
          pwasm_env_fail(frame.env, "br_if: ctrl_stack_peek_tail failed");
          return false;
        }

        if (ctrl_tail->type == CTRL_LOOP) {
          i = ctrl_tail->ofs;
          stack->pos = ctrl_tail->depth;
        } else {
          // skip past end inst
          i += in.v_block.end_ofs + 1;

          // get mod, block type
          const pwasm_mod_t * const mod = frame.mod->mod;
          const int32_t block_type = insts[ctrl_tail->ofs].v_block.block_type;

          // get block type result count, check for error
          size_t num_results;
          if (!pwasm_block_type_results_get_size(mod, block_type, &num_results)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br_if: get block num_results failed");
            return false;
          }

          // pop results
          for (size_t j = 0; j < num_results; j++) {
            // calculate stack source and destination offsets
            const size_t src_ofs = stack->pos - 1 - (num_results - 1 - j);
            const size_t dst_ofs = ctrl_tail->depth + 1 + j;
            stack->ptr[dst_ofs] = stack->ptr[src_ofs];
          }

          // reset value stack
          stack->pos = ctrl_tail->depth + num_results;

          // pop control stack, check for error
          if (!pwasm_ctrl_stack_pop(ctrl_stack, NULL)) {
            // log error, return failure
            pwasm_env_fail(frame.env, "br: ctrl_stack_pop failed");
            return false;
          }

          // decriment control stack depth
          ctrl_depth--;
        }
      }

      break;
    case PWASM_OP_RETURN:
      // return success
      // FIXME: is this correct?
      return true;
    case PWASM_OP_CALL:
      // call function, check for error
      if (!pwasm_aot_jit_call_func(frame.env, frame.mod, in.v_index)) {
        // return failure
        return false;
      }

      break;
    case PWASM_OP_CALL_INDIRECT:
      {
        // pop element index from value stack
        const uint32_t elem_ofs = stack->ptr[--stack->pos].i32;

        // call function, check for error
        if (!pwasm_aot_jit_call_indirect(frame, in, elem_ofs)) {
          // return failure
          return false;
        }
      }

      break;
    case PWASM_OP_DROP:
      stack->pos--;

      break;
    case PWASM_OP_SELECT:
      {
        const size_t ofs = stack->ptr[stack->pos - 1].i32 ? 3 : 2;
        stack->ptr[stack->pos - 3] = stack->ptr[stack->pos - ofs];
        stack->pos -= 2;
      }

      break;
    case PWASM_OP_LOCAL_GET:
      {
        // get local index
        // (note: checked in pwasm_checker_get_local_type())
        const uint32_t id = in.v_index;

        // push local value
        stack->ptr[stack->pos++] = stack->ptr[frame.locals.ofs + id];
      }

      break;
    case PWASM_OP_LOCAL_SET:
      {
        // get local index
        // (note: checked in pwasm_checker_get_local_type())
        const uint32_t id = in.v_index;

        // set local value, pop stack
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
        stack->pos--;
      }

      break;
    case PWASM_OP_LOCAL_TEE:
      {
        // get local index
        // (note: checked in pwasm_checker_get_local_type())
        const uint32_t id = in.v_index;

        // set local value, keep stack (tee)
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
      }

      break;
    case PWASM_OP_GLOBAL_GET:
      {
        // get global index
        const uint32_t id = pwasm_aot_jit_mod_get_global_index(frame.env, frame.mod, in.v_index);

        // get global value, check for error
        pwasm_val_t val;
        if (!pwasm_env_get_global(frame.env, id, &val)) {
          // return failure
          return false;
        }

        // push global value
        stack->ptr[stack->pos++] = val;
      }

      break;
    case PWASM_OP_GLOBAL_SET:
      {
        // get global index
        const uint32_t id = pwasm_aot_jit_mod_get_global_index(frame.env, frame.mod, in.v_index);

        // set global value, check for error
        if (!pwasm_env_set_global(frame.env, id, PWASM_PEEK(stack, 0))) {
          // return failure
          return false;
        }

        // pop stack
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LOAD:
    case PWASM_OP_I64_LOAD:
    case PWASM_OP_F32_LOAD:
    case PWASM_OP_F64_LOAD:
    case PWASM_OP_I32_LOAD8_S:
    case PWASM_OP_I32_LOAD8_U:
    case PWASM_OP_I32_LOAD16_S:
    case PWASM_OP_I32_LOAD16_U:
    case PWASM_OP_I64_LOAD8_S:
    case PWASM_OP_I64_LOAD8_U:
    case PWASM_OP_I64_LOAD16_S:
    case PWASM_OP_I64_LOAD16_U:
    case PWASM_OP_I64_LOAD32_S:
    case PWASM_OP_I64_LOAD32_U:
    case PWASM_OP_V128_LOAD:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t val;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &val)) {
          return false;
        }

        // pop ofs operand, push val
        stack->ptr[stack->pos - 1] = val;
      }

      break;
    case PWASM_OP_I32_STORE:
    case PWASM_OP_I64_STORE:
    case PWASM_OP_F32_STORE:
    case PWASM_OP_F64_STORE:
    case PWASM_OP_I32_STORE8:
    case PWASM_OP_I32_STORE16:
    case PWASM_OP_I64_STORE8:
    case PWASM_OP_I64_STORE16:
    case PWASM_OP_I64_STORE32:
    case PWASM_OP_V128_STORE:
      {
        // get offset operand and value
        const uint32_t ofs = stack->ptr[stack->pos - 2].i32;
        const pwasm_val_t val = stack->ptr[stack->pos - 1];
        stack->pos -= 2;

        // store value, check for error
        if (!pwasm_env_mem_store(frame.env, frame.mem_id, in, ofs, val)) {
          return false;
        }
      }

      break;
    case PWASM_OP_MEMORY_SIZE:
      {
        // get memory size, check for error
        uint32_t size;
        if (!pwasm_env_mem_size(frame.env, frame.mem_id, &size)) {
          return false;
        }

        // push size to stack
        stack->ptr[stack->pos++].i32 = size;
      }

      break;
    case PWASM_OP_MEMORY_GROW:
      {
        // get grow operand
        const uint32_t grow = stack->ptr[stack->pos - 1].i32;

        // grow memory, check for error
        uint32_t size;
        if (!pwasm_env_mem_grow(frame.env, frame.mem_id, grow, &size)) {
          return false;
        }

        // pop operand, push result
        stack->ptr[stack->pos - 1].i32 = size;
      }

      break;
    case PWASM_OP_I32_CONST:
      stack->ptr[stack->pos++].i32 = in.v_i32;

      break;
    case PWASM_OP_I64_CONST:
      stack->ptr[stack->pos++].i64 = in.v_i64;

      break;
    case PWASM_OP_F32_CONST:
      stack->ptr[stack->pos++].f32 = in.v_f32;

      break;
    case PWASM_OP_F64_CONST:
      stack->ptr[stack->pos++].f64 = in.v_f64;

      break;
    case PWASM_OP_I32_EQZ:
      stack->ptr[stack->pos - 1].i32 = (stack->ptr[stack->pos - 1].i32 == 0);

      break;
    case PWASM_OP_I32_EQ:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_NE:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LT_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LT_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GT_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GT_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LE_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_LE_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GE_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GE_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_EQZ:
      stack->ptr[stack->pos - 1].i32 = (stack->ptr[stack->pos - 1].i64 == 0);

      break;
    case PWASM_OP_I64_EQ:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_NE:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LT_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LT_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GT_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GT_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LE_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_LE_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GE_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GE_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_EQ:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_NE:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_LT:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_GT:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_LE:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_GE:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_EQ:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a == b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_NE:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a != b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_LT:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a < b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_GT:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a > b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_LE:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_GE:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].i32 = (a >= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_CLZ:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = a ? __builtin_clz(a) : 32;
      }

      break;
    case PWASM_OP_I32_CTZ:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = a ? __builtin_ctz(a) : 32;
      }

      break;
    case PWASM_OP_I32_POPCNT:
      {
        const int val = __builtin_popcount(stack->ptr[stack->pos - 1].i32);
        stack->ptr[stack->pos - 1].i32 = val;
      }

      break;
    case PWASM_OP_I32_ADD:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SUB:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_MUL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_DIV_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.div_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_DIV_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.div_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.rem_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i32.rem_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i32 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_AND:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a & b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_OR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a | b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_XOR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a ^ b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = (a << b) | (a >> (32 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;
        stack->ptr[stack->pos - 2].i32 = (a << (32 - b)) | (a >> b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_CLZ:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = a ? __builtin_clzl(a) : 64;
      }

      break;
    case PWASM_OP_I64_CTZ:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = a ? __builtin_ctzl(a) : 64;
      }

      break;
    case PWASM_OP_I64_POPCNT:
      {
        const int val = __builtin_popcountl(stack->ptr[stack->pos - 1].i64);
        stack->ptr[stack->pos - 1].i64 = val;
      }

      break;
    case PWASM_OP_I64_ADD:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SUB:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_MUL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_DIV_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.div_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_DIV_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.div_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.rem_s: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;

        if (!b) {
          // log error, return failure
          pwasm_env_fail(frame.env, "i64.rem_u: division by zero");
          return false;
        }

        stack->ptr[stack->pos - 2].i64 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_AND:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a & b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_OR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a | b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_XOR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a ^ b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = (a << b) | (a >> (64 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x3F;
        stack->ptr[stack->pos - 2].i64 = (a << (64 - b)) | (a >> b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_ABS:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = fabsf(a);
      }

      break;
    case PWASM_OP_F32_NEG:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = -a;
      }

      break;
    case PWASM_OP_F32_CEIL:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = ceilf(a);
      }

      break;
    case PWASM_OP_F32_FLOOR:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = floorf(a);
      }

      break;
    case PWASM_OP_F32_TRUNC:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = truncf(a);
      }

      break;
    case PWASM_OP_F32_NEAREST:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = roundf(a);
      }

      break;
    case PWASM_OP_F32_SQRT:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f32 = sqrtf(a);
      }

      break;
    case PWASM_OP_F32_ADD:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_SUB:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_MUL:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_DIV:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_MIN:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = fminf(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_MAX:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = fmaxf(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F32_COPYSIGN:
      {
        const float a = stack->ptr[stack->pos - 2].f32;
        const float b = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].f32 = copysignf(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_ABS:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = fabs(a);
      }

      break;
    case PWASM_OP_F64_NEG:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = -a;
      }

      break;
    case PWASM_OP_F64_CEIL:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = ceil(a);
      }

      break;
    case PWASM_OP_F64_FLOOR:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = floor(a);
      }

      break;
    case PWASM_OP_F64_TRUNC:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = trunc(a);
      }

      break;
    case PWASM_OP_F64_NEAREST:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = round(a);
      }

      break;
    case PWASM_OP_F64_SQRT:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f64 = sqrt(a);
      }

      break;
    case PWASM_OP_F64_ADD:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a + b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_SUB:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a - b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_MUL:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a * b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_DIV:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_MIN:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = fmin(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_MAX:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = fmax(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_F64_COPYSIGN:
      {
        const double a = stack->ptr[stack->pos - 2].f64;
        const double b = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].f64 = copysign(a, b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_WRAP_I64:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (int32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (int32_t) a;
      }

      break;
    case PWASM_OP_I32_TRUNC_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) a;
      }

      break;
    case PWASM_OP_I64_EXTEND_I32_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i64 = (int64_t) a;
      }

      break;
    case PWASM_OP_I64_EXTEND_I32_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (int64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (int64_t) a;
      }

      break;
    case PWASM_OP_I64_TRUNC_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I32_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I32_U:
      {
        const uint32_t a = (uint32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I64_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_CONVERT_I64_U:
      {
        const uint64_t a = (uint64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F32_DEMOTE_F64:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].f32 = (float) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I32_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I32_U:
      {
        const uint32_t a = (uint32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I64_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_CONVERT_I64_U:
      {
        const uint64_t a = (uint64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].f64 = (double) a;
      }

      break;
    case PWASM_OP_F64_PROMOTE_F32:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].f64 = a;
      }

      break;
    case PWASM_OP_I32_REINTERPRET_F32:
      {
        const union {
          uint32_t i32;
          float f32;
        } v = { .f32 = stack->ptr[stack->pos - 1].f32 };
        stack->ptr[stack->pos - 1].i32 = v.i32;
      }

      break;
    case PWASM_OP_I64_REINTERPRET_F64:
      {
        const union {
          uint64_t i64;
          double f64;
        } v = { .f64 = stack->ptr[stack->pos - 1].f64 };
        stack->ptr[stack->pos - 1].i64 = v.i64;
      }

      break;
    case PWASM_OP_F32_REINTERPRET_I32:
      {
        const union {
          uint32_t i32;
          float f32;
        } v = { .i32 = stack->ptr[stack->pos - 1].i32 };
        stack->ptr[stack->pos - 1].f32 = v.f32;
      }

      break;
    case PWASM_OP_F64_REINTERPRET_I64:
      {
        const union {
          uint64_t i64;
          double f64;
        } v = { .i64 = stack->ptr[stack->pos - 1].i64 };
        stack->ptr[stack->pos - 1].f64 = v.f64;
      }

      break;
    case PWASM_OP_I32_EXTEND8_S:
      {
        const int32_t val = (int8_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) val;
      }

      break;
    case PWASM_OP_I32_EXTEND16_S:
      {
        const int32_t val = (int16_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) val;
      }

      break;
    case PWASM_OP_I64_EXTEND8_S:
      {
        const int64_t val = (int8_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) val;
      }

      break;
    case PWASM_OP_I64_EXTEND16_S:
      {
        const int64_t val = (int16_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) val;
      }

      break;
    case PWASM_OP_I64_EXTEND32_S:
      {
        const int64_t val = (int32_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) val;
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (int32_t) CLAMP(a, INT32_MIN, INT32_MAX);
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) CLAMP(a, 0, UINT32_MAX);
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (int32_t) CLAMP(a, INT32_MIN, INT32_MAX);
      }

      break;
    case PWASM_OP_I32_TRUNC_SAT_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i32 = (uint32_t) CLAMP(a, 0, UINT32_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F32_S:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (int64_t) CLAMP(a, INT64_MIN, INT64_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F32_U:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) CLAMP(a, 0, UINT64_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F64_S:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (int64_t) CLAMP(a, INT64_MIN, INT64_MAX);
      }

      break;
    case PWASM_OP_I64_TRUNC_SAT_F64_U:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].i64 = (uint64_t) CLAMP(a, 0, UINT64_MAX);
      }

      break;
    case PWASM_OP_V128_CONST:
      stack->ptr[stack->pos++].v128 = in.v_v128;

      break;
    case PWASM_OP_I8X16_SPLAT:
      {
        const uint8_t a = stack->ptr[stack->pos - 1].i32;
        memset(stack->ptr[stack->pos - 1].v128.i8, a, 16);
      }

      break;
    case PWASM_OP_I8X16_EXTRACT_LANE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = (int8_t) a.i8[in.v_index];
      }

      break;
    case PWASM_OP_I8X16_EXTRACT_LANE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = a.i8[in.v_index];
      }

      break;
    case PWASM_OP_I8X16_REPLACE_LANE:
      {
        const uint8_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].v128.i8[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SPLAT:
      {
        const uint16_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].v128.i16[0] = a;
        stack->ptr[stack->pos - 1].v128.i16[1] = a;
        stack->ptr[stack->pos - 1].v128.i16[2] = a;
        stack->ptr[stack->pos - 1].v128.i16[3] = a;
        stack->ptr[stack->pos - 1].v128.i16[4] = a;
        stack->ptr[stack->pos - 1].v128.i16[5] = a;
        stack->ptr[stack->pos - 1].v128.i16[6] = a;
        stack->ptr[stack->pos - 1].v128.i16[7] = a;
      }

      break;
    case PWASM_OP_I16X8_EXTRACT_LANE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = (int16_t) a.i16[in.v_index];
      }

      break;
    case PWASM_OP_I16X8_EXTRACT_LANE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = a.i16[in.v_index];
      }

      break;
    case PWASM_OP_I16X8_REPLACE_LANE:
      {
        const uint16_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].v128.i16[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SPLAT:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 1].v128.i32[0] = a;
        stack->ptr[stack->pos - 1].v128.i32[1] = a;
        stack->ptr[stack->pos - 1].v128.i32[2] = a;
        stack->ptr[stack->pos - 1].v128.i32[3] = a;
      }

      break;
    case PWASM_OP_I32X4_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i32 = a.i32[in.v_index];
      }

      break;
    case PWASM_OP_I32X4_REPLACE_LANE:
      {
        const uint32_t a = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].v128.i32[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SPLAT:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        D("a = 0x%016lx", a);
        stack->ptr[stack->pos - 1].v128.i64[0] = a;
        stack->ptr[stack->pos - 1].v128.i64[1] = a;
      }

      break;
    case PWASM_OP_I64X2_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].i64 = a.i64[in.v_index];
      }

      break;
    case PWASM_OP_I64X2_REPLACE_LANE:
      {
        const uint64_t a = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].v128.i64[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_SPLAT:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 1].v128.f32[0] = a;
        stack->ptr[stack->pos - 1].v128.f32[1] = a;
        stack->ptr[stack->pos - 1].v128.f32[2] = a;
        stack->ptr[stack->pos - 1].v128.f32[3] = a;
      }

      break;
    case PWASM_OP_F32X4_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].f32 = a.f32[in.v_index];
      }

      break;
    case PWASM_OP_F32X4_REPLACE_LANE:
      {
        const float a = stack->ptr[stack->pos - 1].f32;
        stack->ptr[stack->pos - 2].v128.f32[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_SPLAT:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 1].v128.f64[0] = a;
        stack->ptr[stack->pos - 1].v128.f64[1] = a;
      }

      break;
    case PWASM_OP_F64X2_EXTRACT_LANE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;
        stack->ptr[stack->pos - 1].f64 = a.f64[in.v_index];
      }

      break;
    case PWASM_OP_F64X2_REPLACE_LANE:
      {
        const double a = stack->ptr[stack->pos - 1].f64;
        stack->ptr[stack->pos - 2].v128.f64[in.v_index] = a;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = (a.i8[j] == b.i8[j]) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = (a.i8[j] != b.i8[j]) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av < bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av < bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av > bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av > bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av <= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_LE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av <= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = (av >= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_GE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av >= bv) ? 0xFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = (a.i16[j] == b.i16[j]) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = (a.i16[j] != b.i16[j]) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av < bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av < bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av > bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av > bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av <= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_LE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av <= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = (av >= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_GE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av >= bv) ? 0xFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.i32[j] == b.i32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.i32[j] != b.i32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av < bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av < bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GT_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av > bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GT_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av > bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av <= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_LE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av <= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = (av >= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_GE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = (av >= bv) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] == b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] != b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_LT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] < b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_GT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] > b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_LE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] <= b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_GE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.f32[j] >= b.f32[j]) ? 0xFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_EQ:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] == b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_NE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] != b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_LT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] < b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_GT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] > b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_LE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] <= b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_GE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.f64[j] >= b.f64[j]) ? 0xFFFFFFFFFFFFFFFF : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_NOT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        b.i64[0] = ~a.i64[0];
        b.i64[1] = ~a.i64[1];
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V128_AND:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] & b.i64[0];
        c.i64[1] = a.i64[1] & b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_OR:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] | b.i64[0];
        c.i64[1] = a.i64[1] | b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_XOR:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] ^ b.i64[0];
        c.i64[1] = a.i64[1] ^ b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V128_BITSELECT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 3].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t m = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = (a.i64[0] & m.i64[0]) | (b.i64[0] & ~m.i64[0]);
        c.i64[1] = (a.i64[1] & m.i64[1]) | (b.i64[1] & ~m.i64[1]);

        stack->ptr[stack->pos - 3].v128 = c;
        stack->pos -= 2;
      }

      break;
    case PWASM_OP_I8X16_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 16; j++) {
          b.i8[j] = -a.i8[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I8X16_ANY_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i8[0] ||  a.i8[1] ||  a.i8[2] ||  a.i8[3] ||
          a.i8[4] ||  a.i8[5] ||  a.i8[6] ||  a.i8[7] ||
          a.i8[8] ||  a.i8[9] ||  a.i8[10] || a.i8[11] ||
          a.i8[12] || a.i8[13] || a.i8[14] || a.i8[15]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I8X16_ALL_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i8[0] &&  a.i8[1] &&  a.i8[2] &&  a.i8[3] &&
          a.i8[4] &&  a.i8[5] &&  a.i8[6] &&  a.i8[7] &&
          a.i8[8] &&  a.i8[9] &&  a.i8[10] && a.i8[11] &&
          a.i8[12] && a.i8[13] && a.i8[14] && a.i8[15]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I8X16_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x7;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = (a.i8[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x7;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          int8_t av = a.i8[j];
          c.i8[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x7;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          uint8_t av = a.i8[j];
          c.i8[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = a.i8[j] + b.i8[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ADD_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          const int32_t v = av + bv;
          c.i8[j] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ADD_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          const uint32_t v = av + bv;
          c.i8[j] = MIN(v, UINT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          c.i8[j] = a.i8[j] - b.i8[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SUB_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          const int32_t v = av - bv;
          c.i8[j] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_SUB_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av > bv) ? (av - bv) : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MIN_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MIN_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MAX_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const int8_t av = a.i8[j];
          const int8_t bv = b.i8[j];
          c.i8[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_MAX_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = -a.i16[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_ANY_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i16[0] || a.i16[1] || a.i16[2] || a.i16[3] ||
          a.i16[4] || a.i16[5] || a.i16[6] || a.i16[7]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I16X8_ALL_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i16[0] && a.i16[1] && a.i16[2] && a.i16[3] &&
          a.i16[4] && a.i16[5] && a.i16[6] && a.i16[7]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I16X8_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0xF;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = (a.i16[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0xF;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          int16_t av = a.i16[j];
          c.i16[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0xF;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          uint16_t av = a.i16[j];
          c.i16[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = a.i16[j] + b.i16[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_ADD_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          const int32_t v = av + bv;
          c.i16[j] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_ADD_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          const uint32_t v = av + bv;
          c.i16[j] = MIN(v, UINT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          c.i16[j] = a.i16[j] - b.i16[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SUB_SATURATE_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          const int32_t v = av - bv;
          c.i16[j] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_SUB_SATURATE_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av > bv) ? (av - bv) : 0;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = av * bv;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MIN_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MIN_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MAX_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t av = a.i16[j];
          const int16_t bv = b.i16[j];
          c.i16[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_MAX_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = -a.i32[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_ANY_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i32[0] || a.i32[1] || a.i32[2] || a.i32[3]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I32X4_ALL_TRUE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        stack->ptr[stack->pos - 1].i32 = (
          a.i32[0] && a.i32[1] && a.i32[2] && a.i32[3]
        ) ? 1 : 0;
      }

      break;
    case PWASM_OP_I32X4_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = (a.i32[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          int32_t av = a.i32[j];
          c.i32[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x1F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          uint32_t av = a.i32[j];
          c.i32[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = a.i32[j] + b.i32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = a.i32[j] - b.i32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.i32[j] = a.i32[j] * b.i32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MIN_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MIN_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = MIN(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MAX_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t av = a.i32[j];
          const int32_t bv = b.i32[j];
          c.i32[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_MAX_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const uint32_t av = a.i32[j];
          const uint32_t bv = b.i32[j];
          c.i32[j] = MAX(av, bv);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.i64[j] = -a.i64[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I64X2_SHL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = (a.i64[j] << b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SHR_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          int64_t av = a.i64[j];
          c.i64[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SHR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          uint64_t av = a.i64[j];
          c.i64[j] = (av >> b);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = a.i64[j] + b.i64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = a.i64[j] - b.i64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64X2_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.i64[j] = a.i64[j] * b.i64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.f32[j] = fabsf(a.f32[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.f32[j] = -a.f32[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_SQRT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.f32[j] = sqrtf(a.f32[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] + b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] - b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] * b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_DIV:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = a.f32[j] / b.f32[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_MIN:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = fminf(a.f32[j], b.f32[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F32X4_MAX:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          c.f32[j] = fmaxf(a.f32[j], b.f32[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.f64[j] = fabs(a.f64[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F64X2_NEG:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.f64[j] = -a.f64[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F64X2_SQRT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.f64[j] = sqrt(a.f64[j]);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F64X2_ADD:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] + b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_SUB:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] - b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_MUL:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] * b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_DIV:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = a.f64[j] / b.f64[j];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_MIN:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = fmin(a.f64[j], b.f64[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_F64X2_MAX:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 2; j++) {
          c.f64[j] = fmax(a.f64[j], b.f64[j]);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32X4_TRUNC_SAT_F32X4_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          int32_t v = CLAMP(a.f32[j], INT32_MIN, INT32_MAX);
          b.i32[j] = v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_TRUNC_SAT_F32X4_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = MIN(a.f32[j], UINT32_MAX);
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_CONVERT_I32X4_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          int32_t v = a.i32[j];
          b.f32[j] = v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_F32X4_CONVERT_I32X4_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          uint32_t v = a.i32[j];
          b.f32[j] = v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V8X16_SWIZZLE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint32_t ofs = (b.i8[j] < 16) ? b.i8[j] : 0;
          c.i8[j] = a.i8[ofs];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V8X16_SHUFFLE:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t ofs = in.v_v128.i8[j] & 0x1F;
          c.i8[j] = (ofs < 16) ? a.i8[ofs] : b.i8[ofs - 16];
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_V8X16_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 16; j++) {
          b.i8[j] = a.v128.i8[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V16X8_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.v128.i16[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V32X4_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.v128.i32[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V64X2_LOAD_SPLAT:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.i64[j] = a.v128.i64[0];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I8X16_NARROW_I16X8_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t v = a.i16[j];
          c.i8[j] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        for (size_t j = 0; j < 8; j++) {
          const int16_t v = b.i16[j];
          c.i8[j + 8] = CLAMP(v, INT8_MIN, INT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_NARROW_I16X8_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const int16_t v = a.i16[j];
          c.i8[j] = CLAMP(v, 0, UINT8_MAX);
        }

        for (size_t j = 0; j < 8; j++) {
          const int16_t v = b.i16[j];
          c.i8[j + 8] = CLAMP(v, 0, UINT8_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NARROW_I32X4_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t v = a.i32[j];
          c.i16[j] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        for (size_t j = 0; j < 4; j++) {
          const int32_t v = b.i32[j];
          c.i16[j + 4] = CLAMP(v, INT16_MIN, INT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_NARROW_I32X4_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 4; j++) {
          const int32_t v = a.i32[j];
          c.i16[j] = CLAMP(v, 0, UINT16_MAX);
        }

        for (size_t j = 0; j < 4; j++) {
          const int32_t v = b.i32[j];
          c.i16[j + 4] = CLAMP(v, 0, UINT16_MAX);
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_LOW_I8X16_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int8_t v = a.i8[j];
          b.i16[j] = (int16_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_HIGH_I8X16_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int8_t v = a.i8[8 + j];
          b.i16[j] = (int16_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_LOW_I8X16_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.i8[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_WIDEN_HIGH_I8X16_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.i8[8 + j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_LOW_I16X8_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int16_t v = a.i16[j];
          b.i32[j] = (int32_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_HIGH_I16X8_S:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int16_t v = a.i16[4 + j];
          b.i32[j] = (int32_t) v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_LOW_I16X8_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.i16[j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_WIDEN_HIGH_I16X8_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.i16[4 + j];
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_LOAD8X8_S:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int8_t v = a.v128.i8[j];
          b.i16[j] = (int16_t) v;
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_LOAD8X8_U:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          b.i16[j] = a.v128.i8[j];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_LOAD16X4_S:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int16_t v = a.v128.i16[j];
          b.i32[j] = (int32_t) v;
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_LOAD16X4_U:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          b.i32[j] = a.v128.i16[j];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I64X2_LOAD32X2_S:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          const int32_t v = a.v128.i32[j];
          b.i64[j] = (int64_t) v;
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I64X2_LOAD32X2_U:
      {
        // get offset operand
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;

        // load value, check for error
        pwasm_val_t a;
        if (!pwasm_env_mem_load(frame.env, frame.mem_id, in, ofs, &a)) {
          return false;
        }

        // splat
        pwasm_v128_t b;
        for (size_t j = 0; j < 2; j++) {
          b.i64[j] = a.v128.i32[j];
        }

        // push val
        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_V128_ANDNOT:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        c.i64[0] = a.i64[0] & ~b.i64[0];
        c.i64[1] = a.i64[1] & ~b.i64[1];

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_AVGR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 16; j++) {
          const uint8_t av = a.i8[j];
          const uint8_t bv = b.i8[j];
          c.i8[j] = (av + bv + 1) / 2;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I16X8_AVGR_U:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 2].v128;
        const pwasm_v128_t b = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t c;
        for (size_t j = 0; j < 8; j++) {
          const uint16_t av = a.i16[j];
          const uint16_t bv = b.i16[j];
          c.i16[j] = (av + bv + 1) / 2;
        }

        stack->ptr[stack->pos - 2].v128 = c;
        stack->pos--;
      }

      break;
    case PWASM_OP_I8X16_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 16; j++) {
          const int8_t v = a.i8[j];
          b.i8[j] = (v < 0) ? -v : v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I16X8_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 8; j++) {
          const int16_t v = a.i16[j];
          b.i16[j] = (v < 0) ? -v : v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    case PWASM_OP_I32X4_ABS:
      {
        const pwasm_v128_t a = stack->ptr[stack->pos - 1].v128;

        pwasm_v128_t b;
        for (size_t j = 0; j < 4; j++) {
          const int32_t v = a.i32[j];
          b.i32[j] = (v < 0) ? -v : v;
        }

        stack->ptr[stack->pos - 1].v128 = b;
      }

      break;
    default:
      // log error, return failure
      pwasm_env_fail(frame.env, "unknown instruction");
      return false;
    }
  }

  // return success (i think?)
  return true;
}

#if 0
static void
pwasm_aot_jit_dump_stack(
  pwasm_env_t * const env,
  const char * const text
) {
  for (size_t i = 0; i < env->stack->pos; i++) {
    D("%s: env->stack->ptr[%zu] = 0x%016lx", text, i, env->stack->ptr[i].i64);
  }
}
#endif /* 0 */

/*
 * world's second shittiest initial interpreter
 */
static bool
pwasm_aot_jit_call_func(
  pwasm_env_t * const env,
  pwasm_aot_jit_mod_t * const interp_mod,
  uint32_t func_ofs
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_stack_t * const stack = env->stack;
  const pwasm_mod_t * const mod = interp_mod->mod;

  // get func parameters and results
  const pwasm_slice_t params = mod->types[mod->funcs[func_ofs]].params;
  const pwasm_slice_t results = mod->types[mod->funcs[func_ofs]].results;

  // check stack position (e.g. missing parameters)
  // (FIXME: do we need this, should it be handled in check?)
  if (stack->pos < params.len) {
    // log error, return failure
    D("missing parameters: stack->pos = %zu, params.len = %zu", stack->pos, params.len);
    pwasm_env_fail(env, "missing function parameters");
    return false;
  }

  // get number of local slots and total frame size
  const size_t max_locals = mod->codes[func_ofs].max_locals;
  const size_t frame_size = mod->codes[func_ofs].frame_size;
  if (max_locals > 0) {
    // clear local slots
    memset(stack->ptr + stack->pos, 0, sizeof(pwasm_val_t) * max_locals);
  }

  // skip past locals
  stack->pos += max_locals;

  // get mems count and pointer in this module
  const size_t num_mems = interp_mod->mems.len;
  const uint32_t * const mems = ((uint32_t*) pwasm_vec_get_data(&(interp->u32s))) + interp_mod->mems.ofs;

  // D("mems slice = { %zu, %zu }, len = %zu, mems[0] = %u", interp_mod->mems.ofs, interp_mod->mems.len, num_mems, num_mems ? mems[0] : 0);
  // build interpreter frame
  pwasm_aot_jit_frame_t frame = {
    .env = env,
    .mod = interp_mod,
    // FIXME: is this right?
    .mem_id = num_mems ? mems[0] : 0,
    .params = params,
    .locals = {
      .ofs = stack->pos - frame_size,
      .len = frame_size,
    },
  };

  // get expr instructions slice
  // const pwasm_slice_t expr = mod->codes[func_ofs].expr;
  // (void) expr;

  D("func_ofs = %u", func_ofs);
  D("fns = %p", (void*) interp_mod->fns);
  D("fns[%u] = { %p, %zu }", func_ofs, (void*) interp_mod->fns[func_ofs].ptr, interp_mod->fns[func_ofs].len);
  union {
    const void *ptr_void;
    bool (*ptr_func)(pwasm_env_t *, const pwasm_mod_t *, uint32_t);
  } pun = { .ptr_void = interp_mod->fns[func_ofs].ptr };

  // pwasm_aot_jit_dump_stack(env, "before");

  // D("calling func (%p)", (void*) pun.ptr_void);
  const bool ok = pun.ptr_func(env, interp_mod->mod, func_ofs);
  D("call done, ok == %d", ok);
  // const bool ok = pwasm_aot_jit_eval_expr(frame, expr);
  if (!ok) {
    D("eval_expr() failed, func_ofs = %u", func_ofs);
    // return failure
    return false;
  }

  // get func results
  if (frame_size > 0) {
    // calc dst and src stack positions
    const size_t dst_pos = frame.locals.ofs;
    const size_t src_pos = stack->pos - results.len;
    const size_t num_bytes = sizeof(pwasm_val_t) * results.len;

    // D("stack->pos =  %zu, dst_pos = %zu, src_pos = %zu", stack->pos, dst_pos, src_pos);
    // copy results, update stack position
    memmove(stack->ptr + dst_pos, stack->ptr + src_pos, num_bytes);
    stack->pos = dst_pos + results.len;
  }

  // pwasm_aot_jit_dump_stack(env, "after");

  // return success
  return true;
}

static bool
pwasm_aot_jit_call(
  pwasm_env_t * const env,
  const uint32_t func_id
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_vec_t * const funcs_vec = &(interp->funcs);
  const pwasm_vec_t * const mods_vec = &(interp->mods);
  const pwasm_aot_jit_func_t * const funcs = pwasm_vec_get_data(funcs_vec);
  pwasm_aot_jit_mod_t * const mods = (pwasm_aot_jit_mod_t*) pwasm_vec_get_data(mods_vec);
  const size_t num_funcs = pwasm_vec_get_size(funcs_vec);
  const size_t num_mods = pwasm_vec_get_size(mods_vec);

  // check that func_id is in bounds
  if (!func_id || func_id > num_funcs) {
    D("bad func_id: %u", func_id);
    pwasm_env_fail(env, "function index out of bounds");
    return false;
  }

  // convert func_id to offset, get function row
  const pwasm_aot_jit_func_t func = funcs[func_id - 1];

  // check mod_ofs
  if (func.mod_ofs >= num_mods) {
    D("bad func mod_ofs: %u", func.mod_ofs);
    pwasm_env_fail(env, "function mod offset out of bounds");
    return false;
  }

  // get mod
  const pwasm_aot_jit_mod_t mod = mods[func.mod_ofs];

  // check func_ofs
  if (func.func_ofs >= mod.funcs.len) {
    D("bad func_ofs: %u", func.mod_ofs);
    pwasm_env_fail(env, "function offset out of bounds");
    return false;
  }

  switch (mod.type) {
  case PWASM_AOT_JIT_MOD_TYPE_MOD:
    D("found func, calling it: %u", func_id);
    return pwasm_aot_jit_call_func(env, mods + func.mod_ofs, func.func_ofs);
  case PWASM_AOT_JIT_MOD_TYPE_NATIVE:
    D("found native func, calling it: %u", func_id);
    const pwasm_native_func_t * const f = mod.native->funcs + func.func_ofs;
    return f->func(env, mod.native);
  default:
    D("func_id %u maps to invalid mod type %u", func_id, mod.type);
    pwasm_env_fail(env, "invalid function module type");
    return false;
  }
}

/**
 * Given a frame and a table ID, get the offset of the table instance in
 * the interpreter.
 *
 * Returns false if the table_id is invalid for the current module.
 */
static bool
pwasm_aot_jit_get_mod_table_ofs(
  const pwasm_aot_jit_frame_t frame,
  const uint32_t mod_table_id,
  uint32_t * const ret_ofs
) {
  pwasm_aot_jit_t * const interp = frame.env->env_data;
  const uint32_t * const u32s = pwasm_vec_get_data(&(interp->u32s));
  const size_t num_tables = frame.mod->tables.len;

  // bounds check table index
  if (mod_table_id >= num_tables) {
    D("mod_table_id (%u) >= num_tables (%zu)", mod_table_id, num_tables);
    pwasm_env_fail(frame.env, "table index out of bounds");
    return false;
  }

  // map local index to table interpreter offset
  *ret_ofs = u32s[frame.mod->tables.ofs + mod_table_id];

  // return success
  return true;
}

/**
 * Given a frame and a call_indirect instruction, and a target function
 * offset in the interpreter, compare the following types:
 *
 *  * in the index immediate of the call_indirect instruction, and
 *  * the type of the target function and
 *
 * The comparison verifies all of the following:
 *
 *   * parameter count
 *   * result count
 *   * parameter value types
 *   * result value types
 *
 * Returns `true` on success or `false` on error.
 */
static bool
pwasm_aot_jit_call_indirect_check_type(
  const pwasm_aot_jit_frame_t frame,
  const pwasm_inst_t in,
  const uint32_t func_ofs
) {
  pwasm_aot_jit_t * const interp = frame.env->env_data;

  // get instruction immediate type
  const uint32_t * const in_u32s = frame.mod->mod->u32s;
  const pwasm_type_t in_type = frame.mod->mod->types[in.v_index];

  // get function type
  const pwasm_vec_t * const fn_vec = &(interp->funcs);
  const pwasm_aot_jit_func_t * const fns = pwasm_vec_get_data(fn_vec);
  const pwasm_aot_jit_func_t fn = fns[func_ofs];
  const pwasm_vec_t * const mod_vec = &(interp->mods);
  const pwasm_aot_jit_mod_t * const mods = pwasm_vec_get_data(mod_vec);
  const pwasm_mod_t * const fn_mod = mods[fn.mod_ofs].mod;
  const uint32_t * const fn_u32s = fn_mod->u32s;
  const pwasm_type_t fn_type = fn_mod->types[fn_mod->funcs[fn.func_ofs]];

  // check parameter count
  if (in_type.params.len != fn_type.params.len) {
    pwasm_env_fail(frame.env, "call_indirect parameter count mismatch");
    return false;
  }

  // check result count
  if (in_type.results.len != fn_type.results.len) {
    pwasm_env_fail(frame.env, "call_indirect result count mismatch");
    return false;
  }

  // check parameters
  for (size_t i = 0 ; i < in_type.params.len; i++) {
    const uint32_t in_param_type = in_u32s[in_type.params.ofs + i];
    const uint32_t fn_param_type = fn_u32s[fn_type.params.ofs + i];

    if (in_param_type != fn_param_type) {
      pwasm_env_fail(frame.env, "call_indirect parameter type mismatch");
      return false;
    }
  }

  // return success
  return true;
}

static bool
pwasm_aot_jit_call_indirect(
  const pwasm_aot_jit_frame_t frame,
  const pwasm_inst_t in,
  const uint32_t elem_ofs
) {
  // get module-relative table index from instruction
  // TODO: hard-coded for now, get from instruction eventually
  const uint32_t mod_table_id = 0;

  // get interpreter table offset
  uint32_t table_ofs;
  if (!pwasm_aot_jit_get_mod_table_ofs(frame, mod_table_id, &table_ofs)) {
    return false;
  }

  // convert table offset to ID
  const uint32_t table_id = table_ofs + 1;
  D("mod_table_id = %u, table_id (interp) = %u", mod_table_id, table_id);

  // get interpreter function offset, check for error
  uint32_t func_ofs;
  if (!pwasm_aot_jit_get_elem(frame.env, table_id, elem_ofs, &func_ofs)) {
    return false;
  }

  // check instruction type index against function type
  if (!pwasm_aot_jit_call_indirect_check_type(frame, in, func_ofs)) {
    return false;
  }

  // call function, return result
  return pwasm_aot_jit_call(frame.env, func_ofs + 1);
}

/*
 * Convert an internal global ID to an externally visible global handle.
 *
 * Returns 0 on error.
 */
static uint32_t
pwasm_aot_jit_get_global_index(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const uint32_t global_ofs
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_new_interp_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  // check mod_id
  if (!mod_id || mod_id > num_rows) {
    // log error, return failure
    pwasm_env_fail(env, "get_global_index: invalid mod ID");
    return false;
  }

  // get slice
  const pwasm_slice_t globals = rows[mod_id - 1].globals;

  // check global offset
  if (global_ofs >= globals.len) {
    // log error, return failure
    pwasm_env_fail(env, "get_global_index: invalid mod ID");
    return false;
  }

  // get u32s
  const pwasm_vec_t * const vec = &(interp->u32s);
  const uint32_t * const u32s = ((uint32_t*) pwasm_vec_get_data(vec)) + globals.ofs;

  // return global handle
  return u32s[global_ofs] + 1;
}

/*
 * Convert an internal table ID to an externally visible table handle.
 *
 * Returns 0 on error.
 */
static uint32_t
pwasm_aot_jit_get_table_index(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const uint32_t table_ofs
) {
  pwasm_aot_jit_t * const interp = env->env_data;
  const pwasm_new_interp_mod_t *rows = pwasm_vec_get_data(&(interp->mods));
  const size_t num_rows = pwasm_vec_get_size(&(interp->mods));

  // check mod_id
  if (!mod_id || mod_id > num_rows) {
    // log error, return failure
    pwasm_env_fail(env, "get_table_index: invalid mod ID");
    return false;
  }

  // get slice
  const pwasm_slice_t tables = rows[mod_id - 1].tables;

  // check table offset
  if (table_ofs >= tables.len) {
    // log error, return failure
    pwasm_env_fail(env, "get_table_index: invalid mod ID");
    return false;
  }

  // get u32s
  const pwasm_vec_t * const vec = &(interp->u32s);
  const uint32_t * const u32s = ((uint32_t*) pwasm_vec_get_data(vec)) + tables.ofs;

  // return table handle
  return u32s[table_ofs] + 1;
}

//
// aot jit callbacks
//

static bool
pwasm_aot_jit_on_init(
  pwasm_env_t * const env
) {
  return pwasm_aot_jit_init(env);
}

static void
pwasm_aot_jit_on_fini(
  pwasm_env_t * const env
) {
  pwasm_aot_jit_fini(env);
}

static uint32_t
  pwasm_aot_jit_on_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  return pwasm_aot_jit_add_native(env, name, mod);
}

static uint32_t
pwasm_aot_jit_on_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  return pwasm_aot_jit_add_mod(env, name, mod);
}

static const pwasm_mod_t *
pwasm_aot_jit_on_get_mod(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  return pwasm_aot_jit_get_mod(env, mod_id);
}

static const pwasm_buf_t *
pwasm_aot_jit_on_get_mod_name(
  pwasm_env_t * const env,
  const uint32_t mod_id
) {
  return pwasm_aot_jit_get_mod_name(env, mod_id);
}

static uint32_t
pwasm_aot_jit_on_find_mod(
  pwasm_env_t * const env,
  const pwasm_buf_t name
) {
  return pwasm_aot_jit_find_mod(env, name);
}

static uint32_t
pwasm_aot_jit_on_find_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  return pwasm_aot_jit_find_func(env, mod_id, name);
}

static uint32_t
pwasm_aot_jit_on_find_mem(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  return pwasm_aot_jit_find_mem(env, mod_id, name);
}

/* 
 * static uint32_t
 * pwasm_aot_jit_on_find_table(
 *   pwasm_env_t * const env,
 *   const uint32_t mod_id,
 *   const pwasm_buf_t name
 * ) {
 *   return pwasm_aot_jit_find_table(env, mod_id, name);
 * }
 */ 

static pwasm_env_mem_t *
pwasm_aot_jit_on_get_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id
) {
  return pwasm_aot_jit_get_mem(env, mem_id);
}

static bool
pwasm_aot_jit_on_mem_load(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_val_t * const ret_val
) {
  return pwasm_aot_jit_mem_load(env, mem_id, in, arg_ofs, ret_val);
}

static bool
pwasm_aot_jit_on_mem_store(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  const pwasm_val_t val
) {
  return pwasm_aot_jit_mem_store(env, mem_id, in, arg_ofs, val);
}

static bool
pwasm_aot_jit_on_mem_size(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  uint32_t * const ret_val
) {
  return pwasm_aot_jit_mem_size(env, mem_id, ret_val);
}

static bool
pwasm_aot_jit_on_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  return pwasm_aot_jit_mem_grow(env, mem_id, grow, ret_val);
}

static bool
pwasm_aot_jit_on_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_val
) {
  return pwasm_aot_jit_get_elem(env, table_id, elem_ofs, ret_val);
}

static uint32_t
pwasm_aot_jit_on_find_import(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
) {
  return pwasm_aot_jit_find_import(env, mod_id, type, name);
}

static bool
pwasm_aot_jit_on_get_global(
  pwasm_env_t * const env,
  const uint32_t id,
  pwasm_val_t * const ret_val
) {
  return pwasm_aot_jit_get_global(env, id, ret_val);
}

static bool
pwasm_aot_jit_on_set_global(
  pwasm_env_t * const env,
  const uint32_t id,
  const pwasm_val_t val
) {
  return pwasm_aot_jit_set_global(env, id, val);
}

static bool
pwasm_aot_jit_on_call(
  pwasm_env_t * const env,
  const uint32_t func_id
) {
  return pwasm_aot_jit_call(env, func_id);
}

static bool
pwasm_aot_jit_on_call_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const uint32_t func_ofs
) {
  // FIXME
  pwasm_aot_jit_t * const interp = env->env_data;
  pwasm_aot_jit_mod_t *rows = (pwasm_aot_jit_mod_t*) pwasm_vec_get_data(&(interp->mods));
  return pwasm_aot_jit_call_func(env, rows + (mod_id - 1), func_ofs);
}

static uint32_t
pwasm_aot_jit_on_get_global_index(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const uint32_t global_ofs
) {
  return pwasm_aot_jit_get_global_index(env, mod_id, global_ofs);
}

static uint32_t
pwasm_aot_jit_on_get_table_index(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const uint32_t table_ofs
) {
  return pwasm_aot_jit_get_table_index(env, mod_id, table_ofs);
}

/*
 * AOT JIT environment callbacks.
 */
static const pwasm_env_cbs_t
PWASM_AOT_JIT_CBS = {
  .init         = pwasm_aot_jit_on_init,
  .fini         = pwasm_aot_jit_on_fini,
  .add_native   = pwasm_aot_jit_on_add_native,
  .add_mod      = pwasm_aot_jit_on_add_mod,
  .get_mod      = pwasm_aot_jit_on_get_mod,
  .get_mod_name = pwasm_aot_jit_on_get_mod_name,
  .find_mod     = pwasm_aot_jit_on_find_mod,
  .find_func    = pwasm_aot_jit_on_find_func,
  .find_mem     = pwasm_aot_jit_on_find_mem,
  // .find_table   = pwasm_aot_jit_on_find_table,
  .get_mem      = pwasm_aot_jit_on_get_mem,
  .mem_load     = pwasm_aot_jit_on_mem_load,
  .mem_store    = pwasm_aot_jit_on_mem_store,
  .mem_size     = pwasm_aot_jit_on_mem_size,
  .mem_grow     = pwasm_aot_jit_on_mem_grow,
  .get_elem     = pwasm_aot_jit_on_get_elem,
  .find_import  = pwasm_aot_jit_on_find_import,
  .get_global   = pwasm_aot_jit_on_get_global,
  .set_global   = pwasm_aot_jit_on_set_global,
  .call         = pwasm_aot_jit_on_call,
  .call_func    = pwasm_aot_jit_on_call_func,
  .get_global_index = pwasm_aot_jit_on_get_global_index,
  .get_table_index = pwasm_aot_jit_on_get_table_index,
};

/*
 * Get AOT JIT environment callbacks.
 */
void
pwasm_aot_jit_get_cbs(
  pwasm_env_cbs_t * const cbs,
  pwasm_jit_t * const jit
) {
  *cbs = PWASM_AOT_JIT_CBS;
  cbs->jit = jit;
}
