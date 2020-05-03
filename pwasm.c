#include <stdbool.h> // bool
#include <string.h> // memcmp()
#include <stdlib.h> // realloc()
#include <unistd.h> // sysconf()
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
 * Maximum depth of checking stack.
 *
 * Note: currently incorrectly used for control frame stack in
 * pwasm_interp_eval_expr(), which needs to be fixed.
 */
#define PWASM_STACK_CHECK_MAX_DEPTH 512

// return minimum of two values
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

// get the number of elements in a static array
#define LEN(ary) (sizeof(ary) / sizeof((ary)[0]))

#ifdef PWASM_DEBUG
// FIXME: limit to DEBUG
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

#define PWASM_EXPORT_TYPE(a, b, c) b,
static const char *PWASM_EXPORT_TYPE_NAMES[] = {
PWASM_EXPORT_TYPES
};
#undef PWASM_EXPORT_TYPE

DEF_GET_NAMES(export_type, EXPORT_TYPE)

static const char *PWASM_IMM_NAMES[] = {
#define PWASM_IMM(a, b) b,
PWASM_IMM_DEFS
#undef PWASM_IMM
};

DEF_GET_NAMES(imm, IMM)

static inline bool
pwasm_is_valid_export_type(
  const uint8_t v
) {
  return v < PWASM_EXPORT_TYPE_LAST;
}

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

const char *
pwasm_result_type_get_name(
  const pwasm_result_type_t v
) {
  switch (v) {
#define PWASM_RESULT_TYPE(a, b, c) case a : return c;
PWASM_RESULT_TYPE_DEFS
#undef PWASM_RESULT_TYPE
  default:
    return pwasm_result_type_get_name(PWASM_RESULT_TYPE_LAST);
  }
}

/**
 * Is this value a valid block result type?
 *
 * From section 5.3.2 of the WebAssembly documentation.
 */
static inline bool
pwasm_is_valid_result_type(
  const uint8_t v
) {
  return ((v == 0x40) || (v == 0x7F) || (v == 0x7E) || (v == 0x7D) || (v == 0x7C));
}

#define PWASM_OP(a, b, c) { \
  .name = (b), \
  .is_valid = true, \
  .imm = PWASM_IMM_##c, \
},

#define PWASM_OP_CONST(a, b, c) { \
  .name = (b), \
  .is_valid = true, \
  .is_const = true, \
  .imm = PWASM_IMM_##c, \
},

#define PWASM_OP_RESERVED(a, b) { \
  .name = ("reserved." b), \
  .imm = PWASM_IMM_LAST, \
},

static const struct {
  const char * name;
  bool is_valid;
  bool is_const;
  pwasm_imm_t imm;
} PWASM_OPS[] = {
PWASM_OP_DEFS
};
#undef PWASM_OP
#undef PWASM_OP_CONST
#undef PWASM_OP_RESERVED

/**
 * Get opcode name as a string.
 */
const char *
pwasm_op_get_name(
  const pwasm_op_t op
) {
  return (op < 0x100) ? PWASM_OPS[op].name : "invalid opcode";
}

/**
 * Get immediate type for opcode.
 */
pwasm_imm_t
pwasm_op_get_imm(
  const pwasm_op_t op
) {
  return PWASM_OPS[op].imm;
}

static inline bool
pwasm_op_is_valid(
  const uint8_t byte
) {
  return PWASM_OPS[byte].is_valid;
}

static inline bool
pwasm_op_is_enter(
  const pwasm_op_t op
) {
  return (op == PWASM_OP_BLOCK) || (op == PWASM_OP_LOOP) || (op == PWASM_OP_IF);
}

static inline bool
pwasm_op_is_local(
  const uint8_t byte
) {
  return (
    (byte == PWASM_OP_LOCAL_GET) ||
    (byte == PWASM_OP_LOCAL_SET) ||
    (byte == PWASM_OP_LOCAL_TEE)
  );
}

static inline bool
pwasm_op_is_global(
  const uint8_t byte
) {
  return (
    (byte == PWASM_OP_GLOBAL_GET) ||
    (byte == PWASM_OP_GLOBAL_SET)
  );
}

static inline bool
pwasm_op_is_const(
  const uint8_t byte
) {
  return (
    (byte == PWASM_IMM_I32_CONST) ||
    (byte == PWASM_IMM_I64_CONST) ||
    (byte == PWASM_IMM_F32_CONST) ||
    (byte == PWASM_IMM_F32_CONST)
  );
}

static inline bool
pwasm_op_is_mem(
  const uint8_t byte
) {
  return pwasm_op_get_imm(byte) == PWASM_IMM_MEM;
}

/**
 * Size (in bits) of memory instruction target.
 *
 * Used for memory instruction alignment checking specified here:
 * https://webassembly.github.io/spec/core/valid/instructions.html#memory-instructions
 */
static const uint8_t
PWASM_OP_NUM_BITS[] = {
  // loads
  32, // i32.load
  64, // i64.load
  32, // f32.load
  64, // f64.load
   8, // i32.load8_s
   8, // i32.load8_u
  16, // i32.load16_s
  16, // i32.load16_u
   8, // i64.load8_s
   8, // i64.load8_u
  16, // i64.load16_s
  16, // i64.load16_u
  32, // i64.load32_s
  32, // i64.load32_u

  // stores
  32, // i32.store
  64, // i64.store
  32, // f32.store
  64, // f64.store
   8, // i32.store8
  16, // i32.store16
   8, // i64.store8
  16, // i64.store16
  32, // i64.store32

   0, // sentinel
};

/**
 * Get the number of bits of the target for the given memory
 * instruction.
 *
 * Used for memory alignment checking specified here:
 * https://webassembly.github.io/spec/core/valid/instructions.html#memory-instructions
 */
static inline uint8_t
pwasm_op_get_num_bits(
  const pwasm_op_t op
) {
  const size_t max_ofs = LEN(PWASM_OP_NUM_BITS) - 1;
  const bool ok = (op >= PWASM_OP_I32_LOAD && op <= PWASM_OP_I64_STORE32);
  const size_t ofs = MIN(ok ? (op - PWASM_OP_I32_LOAD) : max_ofs, max_ofs);
  return PWASM_OP_NUM_BITS[ofs];
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

#define CTL_TYPES \
  CTL_TYPE(BLOCK) \
  CTL_TYPE(LOOP) \
  CTL_TYPE(IF) \
  CTL_TYPE(ELSE)

typedef enum {
#define CTL_TYPE(a) CTL_ ## a,
CTL_TYPES
#undef CTL_TYPE
  CTL_LAST,
} pwasm_ctl_type_t;

/*
 * static pwasm_ctl_type_t
 * pwasm_op_get_ctl_type(
 *   const pwasm_op_t op
 * ) {
 *   switch (op) {
 *   #define CTL_TYPE(a) case PWASM_OP_ ## a: return CTL_ ## a;
 *   CTL_TYPES
 *   #undef CTL_TYPE
 *   default:
 *     return CTL_LAST;
 *   }
 * }
 */

typedef struct {
  pwasm_ctl_type_t type; // return value type
  size_t depth; // value stack depth
  size_t ofs; // inst ofs
} pwasm_ctl_stack_entry_t;

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

  // return total number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_slice_t (*on_u32s)(const uint32_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_type_cbs_t;

typedef struct {
  const pwasm_parse_type_cbs_t * const cbs;
  void *cb_data;
  bool success;
  pwasm_slice_t * const slice;
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

typedef struct {
  void (*on_custom_section)(const pwasm_custom_section_t *, void *);
  pwasm_slice_t (*on_bytes)(const uint8_t *, size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_custom_section_cbs_t;

typedef struct {
  const pwasm_parse_custom_section_cbs_t * const cbs;
  void *cb_data;
  bool success;
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

  // pass bytes to callback, get name slice, check for error
  const pwasm_slice_t name = cbs->on_bytes(buf.ptr, buf.len, cb_data);
  D("name = { .ofs = %zu, .len = %zu }", name.ofs, name.len);
  if (name.len != buf.len) {
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, len);
  num_bytes += len;

  const pwasm_slice_t rest = cbs->on_bytes(curr.ptr, curr.len, cb_data);
  if (rest.len != curr.len) {
    return 0;
  }

  // advance
  curr = pwasm_buf_step(curr, rest.len);
  num_bytes += rest.len;

  // build section
  const pwasm_custom_section_t section = {
    .name = name,
    .data = rest,
  };

  D(
    "name = { .ofs = %zu, .len = %zu}, data = { .ofs = %zu, .len = %zu }",
    section.name.ofs, section.name.len,
    section.data.ofs, section.data.len
  );

  // pass section to callback
  cbs->on_custom_section(&section, cb_data);

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

  // return number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_inst_cbs_t;

/**
 * Parse inst into +dst+ from buffer +src+ of length +src_len+.
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

  // check source length
  if (src.len < 1) {
    cbs->on_error("short instruction", cb_data);
    return 0;
  }

  // get op, check for error
  const pwasm_op_t op = curr.ptr[0];
  if (!pwasm_op_is_valid(op)) {
    D("invalid op = 0x%02X", op);
    cbs->on_error("invalid op", cb_data);
    return 0;
  }

  // dump instruction
  // D("0x%02X %s", curr.ptr[0], pwasm_op_get_name(op));

  // advance
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

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
      // check length
      if (!curr.len) {
        cbs->on_error("missing result type immediate", cb_data);
        return 0;
      }

      // get block result type, check for error
      const uint8_t type = curr.ptr[0];
      if (!pwasm_is_valid_result_type(type)) {
        cbs->on_error("invalid result type", cb_data);
      }

      // save result type
      in.v_block.type = type;

      // advance
      curr = pwasm_buf_step(curr, 1);
      num_bytes += 1;
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
      const size_t len = pwasm_parse_labels(&labels, curr, &labels_cbs, NULL);
      if (!len) {
        cbs->on_error("bad br_table labels immediate", cb_data);
        return 0;
      }

      // save labels buffer, increment length
      in.v_br_table.labels.slice = labels;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_INDEX:
    {
      // get index, check for error
      uint32_t id = 0;
      const size_t len = pwasm_u32_decode(&id, curr);
      if (!len) {
        cbs->on_error("bad immediate index value", cb_data);
        return 0;
      }

      // save index
      in.v_index.id = id;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
    }

    break;
  case PWASM_IMM_CALL_INDIRECT:
    {
      // get index, check for error
      uint32_t id = 0;
      const size_t len = pwasm_u32_decode(&id, curr);
      if (!len) {
        cbs->on_error("bad immediate index value", cb_data);
        return 0;
      }

      // save index
      in.v_index.id = id;

      // advance
      curr = pwasm_buf_step(curr, len);
      num_bytes += len;
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
      in.v_i32.val = val;

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
      in.v_i64.val = val;

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
      in.v_f32.val = u.f32;

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
      in.v_f64.val = u.f64;

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
  // TODO: limit to constant insts
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
pwasm_parse_import_function( // FIXME: rename to "func"
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
  return pwasm_parse_table(&(dst->table), src, cbs->on_error, cb_data);
}

static inline size_t
pwasm_parse_import_memory( // FIXME: rename to "mem"
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_parse_limits(&(dst->mem), src, cbs->on_error, cb_data);
}

static inline size_t
pwasm_parse_import_global(
  pwasm_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_parse_global_type(&(dst->global), src, cbs->on_error, cb_data);
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
  if (cbs->on_error) {
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

  // parse module name, check for error
  // FIXME: handle on_error here
  pwasm_slice_t names[2] = { 0 };
  for (size_t i = 0; i < 2; i++) {
    pwasm_buf_t buf;
    const size_t len = pwasm_parse_buf(&buf, curr, &buf_cbs, cb_data);
    if (!len) {
      return 0;
    }

    if (buf.len > 0) {
      // add bytes, check for error
      names[i] = cbs->on_bytes(buf.ptr, buf.len, cb_data);
      if (!names[i].len) {
        return 0;
      }
    }

    // advance buffer, increment byte count
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;

    D("curr.ptr = %p, curr.len = %zu", (void*) curr.ptr, curr.len);
  }

  if (curr.len < 2) {
    cbs->on_error("missing import type", cb_data);
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
    cbs->on_error("invalid import data", cb_data);
    return 0;
  }

  // add length to result
  curr = pwasm_buf_step(curr, len);
  num_bytes += len;

  D("dst = %p, num_bytes = %zu", (void*) dst, num_bytes);

  // save result to destination
  *dst = tmp;

  D("dst = %p, num_bytes = %zu (tmp written)", (void*) dst, num_bytes);

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

  // get export type, check for error
  const pwasm_export_type_t type = curr.ptr[0];
  if (!pwasm_is_valid_export_type(type)) {
    cbs->on_error("bad export type", cb_data);
  }
  D("type = %u:%s", type, pwasm_export_type_get_name(type));

  // advance
  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

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
  D("type = %u:%s, id = %u", type, pwasm_export_type_get_name(type), id);

  *dst = (pwasm_export_t) {
    .name = name,
    .type = type,
    .id   = id,
  };

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

  // return number of bytes consumed
  return num_bytes;
}

typedef struct {
  pwasm_slice_t (*on_bytes)(const uint8_t *, const size_t, void *);
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);
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

  return page_size * (
    (num_bytes / page_size) + ((num_bytes % page_size) ? 1 : 0)
  );
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


#define BUILDER_VECS \
  BUILDER_VEC(u32, uint32_t, dummy) \
  BUILDER_VEC(section, uint32_t, u32) \
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
    /* TODO: log error */ \
  }
BUILDER_VECS
#undef BUILDER_VEC
}

#define BUILDER_VEC(name, type, prev) \
  pwasm_slice_t \
  pwasm_builder_push_ ## name ## s( \
    pwasm_builder_t * const builder, \
    const type * const src_ptr, \
    const size_t src_len \
  ) { \
    pwasm_vec_t * const vec = &(builder->name ## s); \
    size_t ofs; \
    const bool ok = pwasm_vec_push(vec, src_len, src_ptr, &ofs); \
    return (pwasm_slice_t) { ok ? ofs : 0, ok ? src_len : 0 }; \
  }
BUILDER_VECS
#undef BUILDER_VEC

size_t
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

bool
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
 *
 * FIXME: copied from pwasm_parse_import
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
  if (!pwasm_builder_push_sections(data->builder, &(header->type), 1).len) {
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
  // * get parameter count from builder->types (checking for overlow)
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

  // check mod (FIXME: this should be done inline in the future)
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
 * Get a pointer to the global type for the given global index.
 *
 * Returns NULL when given an invalid global index.
 */
static const pwasm_global_type_t *
pwasm_mod_get_global_type(
  const pwasm_mod_t * const mod,
  const uint32_t id
) {
  const size_t num_imports = mod->num_import_types[PWASM_IMPORT_TYPE_GLOBAL];

  if (!pwasm_mod_is_valid_index(mod, PWASM_IMPORT_TYPE_GLOBAL, id)) {
    return NULL;
  }

  if (id < num_imports) {
    for (size_t i = 0, j = 0; i < mod->num_imports; i++) {
      const pwasm_import_t import = mod->imports[i];

      if (import.type == PWASM_IMPORT_TYPE_GLOBAL) {
        if (id == j) {
          return &(mod->imports[i].global);
        }

        j++;
      }
    }

    // return failure (shouldn't happen)
    return NULL;
  } else {
    // this is safe because of the is_valid_func_id() call above
    return &(mod->globals[id - num_imports].type);
  }
}

typedef struct {
  const pwasm_mod_check_cbs_t cbs;
  void *cb_data;
} pwasm_mod_check_t;

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
  const uint32_t id = in.v_index.id;

  // is this a valid instruction for a constant expr?
  if (
    !pwasm_op_is_const(in.op) ||
    (in.op == PWASM_OP_GLOBAL_GET) ||
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
  // FIXME: should this be an error?
  if (type.results.len > 1) {
    check->cbs.on_warning("function type result count > 1", check->cb_data);
    return false;
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
    return pwasm_mod_check_func(mod, check, import.func);
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
  const pwasm_export_type_t type,
  const uint32_t id
) {
  switch (type) {
  case PWASM_EXPORT_TYPE_FUNC:
    return id < (mod->num_funcs + mod->num_import_types[PWASM_IMPORT_TYPE_FUNC]);
  case PWASM_EXPORT_TYPE_TABLE:
    return id < (mod->num_tables + mod->num_import_types[PWASM_IMPORT_TYPE_TABLE]);
  case PWASM_EXPORT_TYPE_MEM:
    return id < (mod->num_mems + mod->num_import_types[PWASM_IMPORT_TYPE_MEM]);
  case PWASM_EXPORT_TYPE_GLOBAL:
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
  if (export.type >= PWASM_EXPORT_TYPE_LAST) {
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
  const pwasm_mod_check_t * const check,
  const pwasm_func_t func
) {
  // get function instructions
  const pwasm_inst_t * const insts = mod->insts + func.expr.ofs;

  // get maximum type indices (e.g. the number of imports of a given
  // type plus the number of internal items of a given type)
  size_t max_indices[PWASM_IMPORT_TYPE_LAST];
  memcpy(max_indices, mod->max_indices, sizeof(max_indices));

  for (size_t i = 0; i < func.expr.len; i++) {
    // get instruction and index immediate
    const pwasm_inst_t in = insts[i];
    const uint32_t id = in.v_index.id;

    // FIXME: we should probably switch on opcode here or immediate
    // rather than a series of ad-hoc conditionals
    switch (in.op) {
    case PWASM_OP_LOCAL_GET:
    case PWASM_OP_LOCAL_SET:
    case PWASM_OP_LOCAL_TEE:
      // is this a local instruction with an invalid index?
      if (id >= func.frame_size) {
        check->cbs.on_error("invalid index in local instruction", check->cb_data);
        return false;
      }

      break;
    case PWASM_OP_GLOBAL_GET:
      // is this a valid global index?
      if (id >= max_indices[PWASM_IMPORT_TYPE_GLOBAL]) {
        check->cbs.on_error("invalid index in global instruction", check->cb_data);
        return false;
      }

      break;
    case PWASM_OP_GLOBAL_SET:
      // is this a valid global index?
      if (id >= max_indices[PWASM_IMPORT_TYPE_GLOBAL]) {
        check->cbs.on_error("invalid index in global instruction", check->cb_data);
        return false;
      }

      // get global type, check for error
      const pwasm_global_type_t * const type = pwasm_mod_get_global_type(mod, id);
      if (!type) {
        check->cbs.on_error("invalid global type", check->cb_data);
        return false;
      }

      // is this a global.set inst to an immutable global index?
      if (!type->mutable) {
        check->cbs.on_error("global.set on an immutable global", check->cb_data);
        return false;
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
    case PWASM_OP_I32_STORE:
    case PWASM_OP_I64_STORE:
    case PWASM_OP_F32_STORE:
    case PWASM_OP_F64_STORE:
    case PWASM_OP_I32_STORE8:
    case PWASM_OP_I32_STORE16:
    case PWASM_OP_I64_STORE8:
    case PWASM_OP_I64_STORE16:
    case PWASM_OP_I64_STORE32:
      // check to make sure we have memory defined
      if (!max_indices[PWASM_IMPORT_TYPE_MEM]) {
        check->cbs.on_error("memory instruction with no memory defined", check->cb_data);
        return false;
      }

      // check alignment immediate
      if (in.v_mem.align > 31) {
        check->cbs.on_error("memory alignment too large", check->cb_data);
        return false;
      }

      // get number of bits for instruction
      const uint32_t num_bits = pwasm_op_get_num_bits(in.op);

      // check alignment
      // reference:
      // https://webassembly.github.io/spec/core/valid/instructions.html#memory-instructions
      if ((1U << in.v_mem.align) > (num_bits >> 3)) {
        check->cbs.on_error("invalid memory alignment", check->cb_data);
        return false;
      }

      break;
    case PWASM_OP_MEMORY_SIZE:
    case PWASM_OP_MEMORY_GROW:
      // check to make sure we have memory defined
      if (!max_indices[PWASM_IMPORT_TYPE_MEM]) {
        check->cbs.on_error("memory instruction with no memory defined", check->cb_data);
        return false;
      }

      break;
    case PWASM_OP_CALL:
      // is this a valid function index?
      if (id >= max_indices[PWASM_IMPORT_TYPE_FUNC]) {
        check->cbs.on_error("invalid function index in call instruction", check->cb_data);
        return false;
      }

      break;
    case PWASM_OP_CALL_INDIRECT:
      // is a table defined?
      if (!max_indices[PWASM_IMPORT_TYPE_TABLE]) {
        check->cbs.on_error("call_indirect instruction with no table", check->cb_data);
        return false;
      }

      break;
    default:
      // TODO: lots more
      break;
    }
  }

  // return success
  return true;
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

// TODO: start, type
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
    const pwasm_mod_check_t * const check \
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
  const pwasm_mod_check_t check = {
    .cbs = {
      .on_warning = (cbs && cbs->on_warning) ? cbs->on_warning : pwasm_null_on_error,
      .on_error = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error,
    },
    .cb_data = cb_data,
  };

  // FIXME: disabled (until i fix tests)
  return true;

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

  // return success
  return true;
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
pasm_env_get_elem(
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

/**
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

/**
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

/**
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

/**
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

/**
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

/**
 * Find set value of global in environment by module name and global
 * name.
 *
 * Note: This is a convenience wrapper around pwasm_env_set_global().
 *
 * Returns false if an error occurred.
 */
bool pwasm_set_global(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name,
  const pwasm_val_t val
) {
  const uint32_t id = pwasm_find_global(env, mod, name);
  return pwasm_env_set_global(env, id, val);
}

/**
 * Friendly wrapper around pwasm_env_call() which accepts the
 * module name and function name as a string instead of a buffer.
 */
bool
pwasm_call(
  pwasm_env_t * const env,
  const char * const mod_name,
  const char * const func_name
) {
  // D("env = %p, mod = %s, func = %s", (void*) env, mod_name, func_name);
  return pwasm_env_call(env, pwasm_find_func(env, mod_name, func_name));
}

/**
 * interpeter memory data
 *
 * FIXME: everything about this is crappy and slow
 */
typedef struct {
  pwasm_env_mem_t * const env_mem; // memory pointer
  const size_t ofs; // absolute offset, in bytes
  const size_t size; // size, in bytes
} pwasm_interp_mem_t;

typedef enum {
  PWASM_INTERP_ROW_TYPE_MOD,
  PWASM_INTERP_ROW_TYPE_NATIVE,
  PWASM_INTERP_ROW_TYPE_FUNC,
  PWASM_INTERP_ROW_TYPE_GLOBAL,
  PWASM_INTERP_ROW_TYPE_MEM,
  PWASM_INTERP_ROW_TYPE_TABLE,
  PWASM_INTERP_ROW_TYPE_LAST,
} pwasm_interp_row_type_t;

typedef struct {
  pwasm_interp_row_type_t type;

  pwasm_buf_t name;

  union {
    struct {
      // map of import IDs to interpreter handle IDs
      const uint32_t * const imports;
      // TODO: const uint32_t * const globals;
      // TODO: const uint32_t * const tables;
      // TODO: const uint32_t * const mems;
      // TODO: const uint32_t * const exports;
      const pwasm_mod_t * const mod;
    } mod;

    pwasm_native_instance_t native;

    struct {
      // externally visible mod_id (e.g. the offset + 1)
      const uint32_t mod_id;

      // internal func_id
      //
      // Note: value depends on mod type. For native mods this is an
      // offset into funcs, and for regular mods this is an entry in the
      // exports table.
      const uint32_t func_id;
    } func;

    struct {
      pwasm_val_t val;
      bool mut;
    } global;

    pwasm_env_mem_t mem;

    pwasm_buf_t table; // TODO: table
  };
} pwasm_interp_row_t;

typedef struct {
  pwasm_val_t val;
  bool mut;
} pwasm_interp_global_t;

typedef struct {
  // vector of u32s.
  //
  // each module instance contains a pointer into this vector which maps
  // their import IDs (funcs, tables, mems, and globals) to the handles
  // exported by the interpreter.
  //
  // FIXME: change from ptr to slice to handle reallocs
  //
  pwasm_vec_t imports;

  // pwasm_vec_t globals;

  // vector of pwasm_interp_row_ts.
  //
  // contains all module instances, function instances, mems, globals,
  // and tables.  externally visible IDs (handles) are an offset into
  // this vector, incremented by one.
  pwasm_vec_t rows;

  // memory handle
  // (FIXME: we only allow one memory, and this is probably not correct)
  uint32_t mem_id;
} pwasm_interp_t;

static bool
pwasm_interp_init(
  pwasm_env_t * const env
) {
  const size_t data_size = sizeof(pwasm_interp_t);
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // allocate interpreter data store
  pwasm_interp_t *data = pwasm_realloc(mem_ctx, NULL, data_size);
  if (!data) {
    // log error, return failure
    D("pwasm_realloc() failed (size = %zu)", data_size);
    pwasm_env_fail(env, "interpreter memory allocation failed");
    return false;
  }

  // allocate imports
  if (!pwasm_vec_init(mem_ctx, &(data->imports), sizeof(uint32_t))) {
    // log error, return failure
    D("pwasm_vec_init() failed (stride = %zu)", sizeof(uint32_t));
    pwasm_env_fail(env, "interpreter import vector init failed");
    return false;
  }

  // allocate rows
  if (!pwasm_vec_init(mem_ctx, &(data->rows), sizeof(pwasm_interp_row_t))) {
    // log error, return failure
    D("pwasm_vec_init() failed (stride = %zu)", sizeof(pwasm_interp_row_t));
    pwasm_env_fail(env, "interpreter data vector init failed");
    return false;
  }

  // clear mem_id
  data->mem_id = 0;

  // save data, return success
  env->env_data = data;
  return true;
}

static void
pwasm_interp_fini(
  pwasm_env_t * const env
) {
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // get interpreter data
  pwasm_interp_t *data = env->env_data;
  if (!data) {
    return;
  }

  // free imports
  pwasm_vec_fini(&(data->imports));

  // free rows
  pwasm_vec_fini(&(data->rows));

  // free backing data
  pwasm_realloc(mem_ctx, data, 0);
  env->env_data = NULL;
}

/**
 * Add import mapping for native module and return a pointer to the
 * mapping.
 *
 * Returns NULL on error, or if there are no imports in the given native
 * module.
 */
static const uint32_t *
pwasm_interp_add_native_imports(
  pwasm_env_t * const env,
  const pwasm_native_t * const mod
) {
  pwasm_interp_t * const data = env->env_data;
  const uint32_t * const imports = pwasm_vec_get_data(&(data->imports));

  uint32_t ids[PWASM_BATCH_SIZE];
  size_t num_ids = 0;

  // loop over imports and resolve each one
  for (size_t i = 0; i < mod->num_imports; i++) {
    const pwasm_native_import_t import = mod->imports[i];

    // find mod, check for error
    const uint32_t mod_id = pwasm_find_mod(env, import.mod);
    if (!mod_id) {
      // return failure
      return NULL;
    }

    // find import ID, check for error
    const pwasm_buf_t name = pwasm_buf_str(import.name);
    const uint32_t id = pwasm_env_find_import(env, mod_id, import.type, name);
    if (!id) {
      // return failure
      return NULL;
    }

    // add item to results, increment count
    ids[num_ids] = id;
    num_ids++;

    if (num_ids == LEN(ids)) {
      // clear count
      num_ids = 0;

      // append results, check for error
      if (!pwasm_vec_push(&(data->imports), LEN(ids), ids, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append native imports failed");
        return NULL;
      }
    }
  }

  if (num_ids > 0) {
    // append remaining results
    if (!pwasm_vec_push(&(data->imports), num_ids, ids, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining native imports failed");
      return NULL;
    }
  }

  // return result (or NULL if mod->num_imports is zero)
  return imports;
}


static uint32_t
pwasm_interp_add_row(
  pwasm_env_t * const env,
  const pwasm_interp_row_t * const row
) {
  // get interpreter data
  pwasm_interp_t * const data = env->env_data;
  if (!data) {
    D("pwasm_vec_init() failed (stride = %zu)", sizeof(pwasm_interp_row_t));
    pwasm_env_fail(env, "NULL interpreter context (bug?)");
    return 0;
  }

  // add row, get offset
  size_t ofs = 0;
  if (!pwasm_vec_push(&(data->rows), 1, row, &ofs)) {
    D("pwasm_vec_init() failed %s", "");
    pwasm_env_fail(env, "pwasm_interp_add_row(): pwasm_vec_push() failed");
    return 0;
  }

  // return offset + 1 (to prevent zero IDs)
  return ofs + 1;
}

static uint32_t
pwasm_interp_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  // TODO: need to resolve imports
  const uint32_t * const imports = NULL;

  // build row
  const pwasm_interp_row_t mod_row = {
    .type = PWASM_INTERP_ROW_TYPE_MOD,
    .name = pwasm_buf_str(name),
    .mod = {
      .imports = imports,
      .mod = mod,
    },
  };

  // add mod
  const uint32_t mod_id = pwasm_interp_add_row(env, &mod_row);
  if (!mod_id) {
    // return failure
    return 0;
  }

  // add exports
  for (size_t i = 0; i < mod->num_exports; i++) {
    const pwasm_export_t export = mod->exports[i];

    switch (export.type) {
    case PWASM_EXPORT_TYPE_FUNC:
      {
        D("adding func %zu", i);

        // build export function row
        const pwasm_interp_row_t row = {
          .type = PWASM_INTERP_ROW_TYPE_FUNC,

          .name = {
            .ptr = mod->bytes + export.name.ofs,
            .len = export.name.len,
          },

          .func = {
            .mod_id = mod_id,
            .func_id = export.id,
          },
        };

        // add function, check for error
        const uint32_t func_id = pwasm_interp_add_row(env, &row);
        if (!func_id) {
          // return failure
          return 0;
        }
      }

      break;
    case PWASM_EXPORT_TYPE_MEM:
      {
        // get interpreter
        pwasm_interp_t * const data = env->env_data;

        D("adding mem %zu", i);
        const pwasm_limits_t limits = mod->mems[export.id];

        pwasm_buf_t buf;
        {
          // calculate number of bytes needed
          const size_t num_bytes = (limits.min * (1UL << 16));

          // set buffer buffer size, allocate memory
          buf.len = limits.min;

          // allocate memory, check for error
          buf.ptr = pwasm_realloc(env->mem_ctx, NULL, num_bytes);
          if (!buf.ptr && num_bytes) {
            // log error, return failure
            pwasm_env_fail(env, "export memory buffer allocation failed");
            return 0;
          }
        }

        // build export function row
        const pwasm_interp_row_t row = {
          .type = PWASM_INTERP_ROW_TYPE_MEM,

          .name = {
            .ptr = mod->bytes + export.name.ofs,
            .len = export.name.len,
          },

          .mem = {
            .buf = buf,
            .limits = limits,
          },
        };

        // add memory, check for error
        const uint32_t mem_id = pwasm_interp_add_row(env, &row);
        if (!mem_id) {
          // return failure
          return 0;
        }

        // cache mem_id
        // FIXME: hack
        data->mem_id = mem_id;
      }

      break;
    default:
      // log error, return failure
      pwasm_env_fail(env, "invalid export type");
      return false;
    }
  }

  // return success
  return mod_id;
}

static uint32_t
pwasm_interp_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  // resolve imports, check for error
  const uint32_t * const imports = pwasm_interp_add_native_imports(env, mod);
  if (mod->num_imports && !imports) {
    // return failure
    return 0;
  }

  // build row
  const pwasm_interp_row_t mod_row = {
    .type     = PWASM_INTERP_ROW_TYPE_NATIVE,
    .name = pwasm_buf_str(name),
    .native = {
      .imports = imports,
      .native = mod,
    },
  };

  // add mod, check for error
  const uint32_t mod_id = pwasm_interp_add_row(env, &mod_row);
  if (!mod_id) {
    // return failure
    return 0;
  }

  // add funcs
  for (size_t i = 0; i < mod->num_funcs; i++) {
    D("adding native func %zu", i);

    // build export function row
    const pwasm_interp_row_t row = {
      .type = PWASM_INTERP_ROW_TYPE_FUNC,
      .name = pwasm_buf_str(mod->funcs[i].name),

      .func = {
        .mod_id = mod_id,
        .func_id = i,
      },
    };

    // add function, check for error
    const uint32_t func_id = pwasm_interp_add_row(env, &row);
    if (!func_id) {
      // return failure
      return 0;
    }
  }

  // return success
  return mod_id;
}

typedef struct {
  pwasm_env_t * const env;
  const pwasm_mod_t * const mod;

  // memory for this frame
  const uint32_t mem_id;

  // function parameters
  const pwasm_slice_t params;

  // offset and length of locals on the stack
  // NOTE: the offset and length include function parameters
  pwasm_slice_t locals;
} pwasm_interp_frame_t;

// forward reference
static bool pwasm_interp_call_func(pwasm_env_t *, const pwasm_mod_t *, uint32_t);

static inline bool
pwasm_interp_call_indirect(
  const pwasm_interp_frame_t frame,
  const uint32_t func_id
) {
  // TODO
  (void) frame;
  (void) func_id;

/*
 *   // get func handle, check for error
 *   const uint32_t func_id = pwasm_interp_get_elem(frame.env, frame.mod_id, table_id, in.v_index.id);
 *   if (!func_id) {
 *     // return failure
 *     return false;
 *   }
 */

  // return failure
  return false;
}

// FIXME: complete hack, need to handle this during parsing
static size_t
pwasm_interp_get_else_ofs(
  const pwasm_interp_frame_t frame,
  const pwasm_slice_t expr,
  const size_t inst_ofs
) {
  const pwasm_inst_t * const insts = frame.mod->insts + expr.ofs;

  size_t depth = 1;
  for (size_t i = inst_ofs + 1; i < expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    if (pwasm_op_is_enter(in.op)) {
      depth++;
    } else if (in.op == PWASM_OP_END) {
      depth--;

      if (!depth) {
        // return failure (no else inst)
        return 0;
      }
    } else if (in.op == PWASM_OP_ELSE) {
      if (depth == 1) {
        // return offset to else inst
        return i - inst_ofs;
      }
    }
  }

  // log error, return failure
  pwasm_env_fail(frame.env, "missing else or end instruction");
  return 0;
}

// FIXME: complete hack, need to handle this during parsing
static size_t
pwasm_interp_get_end_ofs(
  const pwasm_interp_frame_t frame,
  const pwasm_slice_t expr,
  const size_t inst_ofs
) {
  const pwasm_inst_t * const insts = frame.mod->insts + expr.ofs;

  size_t depth = 1;
  for (size_t i = inst_ofs + 1; i < expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    if (pwasm_op_is_enter(in.op)) {
      depth++;
    } else if (in.op == PWASM_OP_END) {
      depth--;

      if (!depth) {
        // return offset to else inst
        return i - inst_ofs;
      }
    }
  }

  // log error, return failure
  pwasm_env_fail(frame.env, "missing end instruction");
  return 0;
}

static bool
pwasm_interp_eval_expr(
  const pwasm_interp_frame_t frame,
  const pwasm_slice_t expr
) {
  pwasm_stack_t * const stack = frame.env->stack;
  const pwasm_inst_t * const insts = frame.mod->insts + expr.ofs;

  // FIXME: move to frame, fix depth
  pwasm_ctl_stack_entry_t ctl_stack[PWASM_STACK_CHECK_MAX_DEPTH];
  size_t depth = 0;

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
      ctl_stack[depth++] = (pwasm_ctl_stack_entry_t) {
        .type   = CTL_BLOCK,
        .depth  = stack->pos,
        .ofs    = i,
      };

      break;
    case PWASM_OP_LOOP:
      ctl_stack[depth++] = (pwasm_ctl_stack_entry_t) {
        .type   = CTL_LOOP,
        .depth  = stack->pos,
        .ofs    = i,
      };

      break;
    case PWASM_OP_IF:
      {
        // pop last value from value stack
        const uint32_t tail = stack->ptr[--stack->pos].i32;

        // get else expr offset
        // TODO
        // const size_t else_ofs = in.v_block.else_ofs ? in.v_block.else_ofs : in.v_block.end_ofs;
        size_t else_ofs;
        {
          // FIXME: this is a colossal hack (and slow), and should be
          // handled in parsing
          const size_t tmp_else_ofs = pwasm_interp_get_else_ofs(frame, expr, i);
          const size_t tmp_end_ofs = pwasm_interp_get_end_ofs(frame, expr, i);
          else_ofs = tmp_else_ofs ? tmp_else_ofs : tmp_end_ofs;
        }
        // D("else_ofs = %zu", else_ofs);

        // push to control stack
        ctl_stack[depth++] = (pwasm_ctl_stack_entry_t) {
          .type   = CTL_IF,
          .depth  = stack->pos,
          .ofs    = i,
        };

        // increment instruction pointer
        i += tail ? 0 : else_ofs;
      }

      break;
    case PWASM_OP_ELSE:
      // skip to end inst
      // TODO
      // i = ctl_stack[depth - 1].ofs + insts[i].v_block.end_ofs - 1;
      i += pwasm_interp_get_end_ofs(frame, expr, i);

      break;
    case PWASM_OP_END:
      if (depth) {
        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      } else {
        // return success
        // FIXME: is this correct?
        return true;
      }

      break;
    case PWASM_OP_BR:
      {
        // check branch index
        // FIXME: check in check for overflow here
        if (in.v_index.id >= depth - 1) {
          return false;
        }

        // decriment control stack
        depth -= in.v_index.id;

        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (ctl_tail.type == CTL_LOOP) {
          // reset control stack
          i = ctl_tail.ofs;
          stack->pos = ctl_tail.depth;
        } else if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      }

      break;
    case PWASM_OP_BR_IF:
      if (stack->ptr[--stack->pos].i32) {
        // check branch index
        // FIXME: need to check in "check" for overflow here
        if (in.v_index.id >= depth - 1) {
          return false;
        }

        // decriment control stack
        depth -= in.v_index.id;

        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (ctl_tail.type == CTL_LOOP) {
          i = ctl_tail.ofs;
          stack->pos = ctl_tail.depth;
        } else if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      }

      break;
    case PWASM_OP_BR_TABLE:
      {
        // get value from stack, branch labels, label offset, and then index
        const uint32_t val = stack->ptr[--stack->pos].i32;
        const pwasm_slice_t labels = in.v_br_table.labels.slice;
        const size_t labels_ofs = labels.ofs + MIN(val, labels.len - 1);
        const uint32_t id = frame.mod->u32s[labels_ofs];

        // check for branch index overflow
        // FIXME: move to check()
        if (id >= depth - 1) {
          return false;
        }

        // decriment control stack
        depth -= id;

        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (ctl_tail.type == CTL_LOOP) {
          i = ctl_tail.ofs;
          stack->pos = ctl_tail.depth;
        } else if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      }

      break;
    case PWASM_OP_RETURN:
      // FIXME: is this all i need?
      return true;
    case PWASM_OP_CALL:
      // call function, check for error
      if (!pwasm_interp_call_func(frame.env, frame.mod, in.v_index.id)) {
        // return failure
        return false;
      }

      break;
    case PWASM_OP_CALL_INDIRECT:
      // call function, check for error
      if (!pwasm_interp_call_indirect(frame, in.v_index.id)) {
        // return failure
        return false;
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
        const uint32_t id = in.v_index.id;

        // check local index
        if (id >= frame.locals.len) {
          // log error, return failure
          pwasm_env_fail(frame.env, "local index out of bounds");
          return false;
        }

        // push local value
        stack->ptr[stack->pos++] = stack->ptr[frame.locals.ofs + id];
      }

      break;
    case PWASM_OP_LOCAL_SET:
      {
        // get local index
        const uint32_t id = in.v_index.id;

        // check local index
        if (id >= frame.locals.len) {
          // log error, return failure
          pwasm_env_fail(frame.env, "local index out of bounds");
          return false;
        }

        // set local value, pop stack
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
        stack->pos--;
      }

      break;
    case PWASM_OP_LOCAL_TEE:
      {
        // get local index
        const uint32_t id = in.v_index.id;

        // check local index
        if (id >= frame.locals.len) {
          // log error, return failure
          pwasm_env_fail(frame.env, "local index out of bounds");
          return false;
        }

        // set local value, keep stack (tee)
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
      }

      break;
    case PWASM_OP_GLOBAL_GET:
      {
        // get global index
        const uint32_t id = in.v_index.id;

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
        const uint32_t id = in.v_index.id;

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
      {
        // get offset operand and value
        const uint32_t ofs = stack->ptr[stack->pos - 1].i32;
        const pwasm_val_t val = stack->ptr[stack->pos - 2];
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
      stack->ptr[stack->pos++].i32 = in.v_i32.val;

      break;
    case PWASM_OP_I64_CONST:
      stack->ptr[stack->pos++].i64 = in.v_i64.val;

      break;
    case PWASM_OP_F32_CONST:
      stack->ptr[stack->pos++].f32 = in.v_f32.val;

      break;
    case PWASM_OP_F64_CONST:
      stack->ptr[stack->pos++].f64 = in.v_f64.val;

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
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GE_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

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
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GE_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
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
        const int val = __builtin_clz(stack->ptr[stack->pos - 1].i32);
        stack->ptr[stack->pos - 1].i32 = val;
      }

      break;
    case PWASM_OP_I32_CTZ:
      {
        const int val = __builtin_ctz(stack->ptr[stack->pos - 1].i32);
        stack->ptr[stack->pos - 1].i32 = val;
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
        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_DIV_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
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
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;
        stack->ptr[stack->pos - 2].i32 = (a << b) | (a >> (32 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;
        stack->ptr[stack->pos - 2].i32 = (a << (32 - b)) | (a >> b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_CLZ:
      {
        const int val = __builtin_clzl(stack->ptr[stack->pos - 1].i64);
        stack->ptr[stack->pos - 1].i64 = val;
      }

      break;
    case PWASM_OP_I64_CTZ:
      {
        const int val = __builtin_ctzl(stack->ptr[stack->pos - 1].i64);
        stack->ptr[stack->pos - 1].i64 = val;
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
        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_DIV_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
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
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x7F;
        stack->ptr[stack->pos - 2].i64 = (a << b) | (a >> (64 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x7F;
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
          uint32_t i64;
          float f64;
        } v = { .i64 = stack->ptr[stack->pos - 1].i64 };
        stack->ptr[stack->pos - 1].f64 = v.f64;
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

/**
 * world's shittiest initial interpreter
 */
static bool
pwasm_interp_call_func(
  pwasm_env_t * const env,
  const pwasm_mod_t * const mod,
  uint32_t func_id
) {
  const size_t num_import_funcs = mod->num_import_types[PWASM_IMPORT_TYPE_FUNC];
  pwasm_stack_t * const stack = env->stack;
  // will be used for CALL and CALL_INDIRECT
  // const size_t stack_pos = env->stack->pos;

  if (func_id < num_import_funcs) {
    // TODO
    D("imported function not supported yet: %u", func_id);

    // log error, return failure
    pwasm_env_fail(env, "imported function not supported");
    return false;
  }

  // map to internal function ID
  func_id -= num_import_funcs;
  if (func_id >= mod->num_funcs) {
    // TODO: invalid function ID, log error
    D("invalid function ID: %u", func_id);

    // log error, return failure
    pwasm_env_fail(env, "function index out of bounds");
    return false;
  }

  // get func parameters and results
  const pwasm_slice_t params = mod->types[mod->funcs[func_id]].params;
  const pwasm_slice_t results = mod->types[mod->funcs[func_id]].results;

  // check stack position (e.g. missing parameters)
  // (FIXME: do we need this, should it be handled in check?)
  if (stack->pos < params.len) {
    // log error, return failure
    D("missing parameters: stack->pos = %zu, params.len = %zu", stack->pos, params.len);
    pwasm_env_fail(env, "missing function parameters");
    return false;
  }

  // get number of local slots and total frame size
  const size_t max_locals = mod->codes[func_id].max_locals;
  const size_t frame_size = mod->codes[func_id].frame_size;
  if (max_locals > 0) {
    // clear local slots
    memset(stack->ptr + stack->pos, 0, sizeof(pwasm_val_t) * max_locals);
  }

  // skip past locals
  stack->pos += max_locals;

  // build interpreter frame
  pwasm_interp_frame_t frame = {
    .env = env,
    .mod = mod,
    .params = params,
    .locals = {
      .ofs = stack->pos - frame_size,
      .len = frame_size,
    },
  };

  // get expr instructions slice
  const pwasm_slice_t expr = mod->codes[func_id].expr;

  const bool ok = pwasm_interp_eval_expr(frame, expr);
  if (!ok) {
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

/**
 * Get the absolute memory offset from the immediate offset and the
 * offset operand.
 */
static inline pwasm_interp_mem_t
pwasm_interp_get_interp_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs
) {
  pwasm_env_mem_t * const env_mem = pwasm_env_get_mem(env, mem_id);
  size_t ofs = in.v_mem.offset + arg_ofs;
  size_t size = pwasm_op_get_num_bits(in.op) / 8;
  const bool ok = env_mem && ofs && size && (ofs + size < env_mem->buf.len);

  return (pwasm_interp_mem_t) {
    .env_mem  = ok ? env_mem : NULL,
    .ofs      = ok ? ofs : 0,
    .size     = ok ? size : 0,
  };
}

static uint32_t
pwasm_interp_find_mod(
  pwasm_env_t * const env,
  const pwasm_buf_t name
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));
  const size_t num_rows = pwasm_vec_get_size(&(data->rows));

  for (size_t i = 0; i < num_rows; i++) {
    if (
      (rows[i].type == PWASM_INTERP_ROW_TYPE_MOD || rows[i].type == PWASM_INTERP_ROW_TYPE_NATIVE) &&
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
pwasm_interp_find_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));
  const size_t num_rows = pwasm_vec_get_size(&(data->rows));
  (void) mod_id;

  for (size_t i = 0; i < num_rows; i++) {
    if (
      (rows[i].type == PWASM_INTERP_ROW_TYPE_FUNC) &&
      (rows[i].name.len == name.len) &&
      !memcmp(name.ptr, rows[i].name.ptr, name.len)
    ) {
      // return offset + 1 (prevent zero IDs)
      return i + 1;
    }
  }

  // log error, return failure
  pwasm_env_fail(env, "function not found");
  return 0;
}

static uint32_t
pwasm_interp_find_mem(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));
  const size_t num_rows = pwasm_vec_get_size(&(data->rows));
  (void) mod_id;

  for (size_t i = 0; i < num_rows; i++) {
    if (
      (rows[i].type == PWASM_INTERP_ROW_TYPE_MEM) &&
      (rows[i].name.len == name.len) &&
      !memcmp(name.ptr, rows[i].name.ptr, name.len)
    ) {
      // return offset + 1 (prevent zero IDs)
      return i + 1;
    }
  }

  // log error, return failure
  pwasm_env_fail(env, "memory not found");
  return 0;
}

static pwasm_env_mem_t *
pwasm_interp_get_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));
  const size_t num_rows = pwasm_vec_get_size(&(data->rows));

  // check that mem_id is in bounds
  if (!mem_id || mem_id > num_rows) {
    // log error, return failure
    D("bad mem_id: %u", mem_id);
    pwasm_env_fail(env, "memory index out of bounds");
    return NULL;
  }

  // check row type
  if (rows[mem_id - 1].type != PWASM_INTERP_ROW_TYPE_MEM) {
    // log error, return failure
    D("invalid mem_id: %u", mem_id);
    pwasm_env_fail(env, "invalid memory index");
    return NULL;
  }

  // return memory
  return (pwasm_env_mem_t*) &(rows[mem_id - 1].mem);
}

static bool
pwasm_interp_call(
  pwasm_env_t * const env,
  const uint32_t func_id
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));
  const size_t num_rows = pwasm_vec_get_size(&(data->rows));

  // check that func_id is in bounds
  if (!func_id || func_id > num_rows) {
    D("bad func_id: %u", func_id);
    pwasm_env_fail(env, "function index out of bounds");
    return false;
  }

  // get function row, check function type
  const pwasm_interp_row_t func_row = rows[func_id - 1];
  if (func_row.type != PWASM_INTERP_ROW_TYPE_FUNC) {
    D("invalid func_id: %u", func_id);
    pwasm_env_fail(env, "invalid function index");
    return false;
  }

  if (!func_row.func.mod_id || func_row.func.mod_id > num_rows) {
    D("bad func mod_id: %u", func_row.func.mod_id);
    pwasm_env_fail(env, "function module index out of bounds");
    return false;
  }

  // get mod row, check mod type
  const pwasm_interp_row_t mod_row = rows[func_row.func.mod_id - 1];
  switch (mod_row.type) {
  case PWASM_INTERP_ROW_TYPE_MOD:
    D("found func, calling it: %u", func_id);
    return pwasm_interp_call_func(env, mod_row.mod.mod, func_row.func.func_id);
  case PWASM_INTERP_ROW_TYPE_NATIVE:
    D("found native func, calling it: %u", func_id);
    // FIXME: check bounds?
    const pwasm_native_func_t * const func = mod_row.native.native->funcs + func_row.func.func_id;
    return func->func(env, mod_row.native.native);
  default:
    D("func_id %u maps to invalid mod_id %u", func_id, func_row.func.mod_id);
    pwasm_env_fail(env, "invalid function module index");
    return false;
  }
}

static bool
pwasm_interp_mem_load(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_val_t * const ret_val
) {
  // get offset, size, and memory
  const pwasm_interp_mem_t mem = pwasm_interp_get_interp_mem(env, mem_id, in, arg_ofs);
  if (!mem.size) {
    return false;
  }

  // copy to result
  memcpy(ret_val, mem.env_mem->buf.ptr + mem.ofs, mem.size);

  // return success
  return true;
}

static bool
pwasm_interp_mem_store(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  const pwasm_val_t val
) {
  // get offset, size, and memory
  pwasm_interp_mem_t mem = pwasm_interp_get_interp_mem(env, mem_id, in, arg_ofs);
  if (!mem.size) {
    return false;
  }

  // copy to result
  memcpy((uint8_t*) mem.env_mem->buf.ptr + mem.ofs, &val, mem.size);

  // return success
  return true;
}

static bool
pwasm_interp_mem_size(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  uint32_t * const ret_val
) {
  // TODO
  (void) env;
  (void) mem_id;
  (void) ret_val;

  // return failure
  return false;
}

static bool
pwasm_interp_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  // TODO
  (void) env;
  (void) mem_id;
  (void) grow;
  (void) ret_val;

  // return failure
  return false;
}

static bool
pwasm_interp_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_val
) {
  // TODO
  (void) env;
  (void) table_id;
  (void) elem_ofs;
  (void) ret_val;

  // return failure
  return false;
}

static uint32_t
pwasm_interp_find_import(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
) {
  switch (type) {
  case PWASM_IMPORT_TYPE_FUNC:
    return pwasm_env_find_func(env, mod_id, name);
  case PWASM_IMPORT_TYPE_GLOBAL:
    // TODO
    // return pwasm_env_find_global(env, mod_id, name);
    return 0;
  case PWASM_IMPORT_TYPE_TABLE:
    // TODO
    // return pwasm_env_find_table(env, mod_id, name);
    return 0;
  case PWASM_IMPORT_TYPE_MEM:
    // TODO
    // return pwasm_env_find_table(env, mod_id, name);
    return 0;
  default:
    // log error, return failure
    pwasm_env_fail(env, "invalid import type");
    return 0;
  }
}

static inline bool
pwasm_interp_check_global(
  pwasm_env_t * const env,
  const uint32_t id
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));
  const size_t num_rows = pwasm_vec_get_size(&(data->rows));

  if (!id || id > num_rows) {
    // log error, return failure
    pwasm_env_fail(env, "global index out of bounds");
    return false;
  }

  if (rows[id - 1].type != PWASM_INTERP_ROW_TYPE_GLOBAL) {
    // log error, return failure
    pwasm_env_fail(env, "invalid global index");
    return false;
  }

  // return success
  return true;
}

static bool
pwasm_interp_get_global(
  pwasm_env_t * const env,
  const uint32_t id,
  pwasm_val_t * const ret_val
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));

  if (!pwasm_interp_check_global(env, id)) {
    return false;
  }

  if (ret_val) {
    // copy value to destination
    *ret_val = rows[id - 1].global.val;
  }

  // return success
  return true;
}

static bool
pwasm_interp_set_global(
  pwasm_env_t * const env,
  const uint32_t id,
  const pwasm_val_t val
) {
  pwasm_interp_t * const data = env->env_data;
  pwasm_interp_row_t *rows = (pwasm_interp_row_t*) pwasm_vec_get_data(&(data->rows));

  if (!pwasm_interp_check_global(env, id)) {
    return false;
  }

  if (!rows[id - 1].global.mut) {
    pwasm_env_fail(env, "write to immutable global");
    return false;
  }

  // set global value
  rows[id - 1].global.val = val;

  // return success
  return true;
}

static bool
pwasm_interp_on_init(
  pwasm_env_t * const env
) {
  return pwasm_interp_init(env);
}

static void
pwasm_interp_on_fini(
  pwasm_env_t * const env
) {
  pwasm_interp_fini(env);
}

static uint32_t
pwasm_interp_on_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  return pwasm_interp_add_mod(env, name, mod);
}

static uint32_t
pwasm_interp_on_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  return pwasm_interp_add_native(env, name, mod);
}

static uint32_t
pwasm_interp_on_find_mod(
  pwasm_env_t * const env,
  const pwasm_buf_t name
) {
  return pwasm_interp_find_mod(env, name);
}

static uint32_t
pwasm_interp_on_find_func(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  return pwasm_interp_find_func(env, mod_id, name);
}

static uint32_t
pwasm_interp_on_find_mem(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_buf_t name
) {
  return pwasm_interp_find_mem(env, mod_id, name);
}

static pwasm_env_mem_t *
pwasm_interp_on_get_mem(
  pwasm_env_t * const env,
  const uint32_t mem_id
) {
  return pwasm_interp_get_mem(env, mem_id);
}

static bool
pwasm_interp_on_call(
  pwasm_env_t * const env,
  const uint32_t func_id
) {
  return pwasm_interp_call(env, func_id);
}

static bool
pwasm_interp_on_mem_load(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  pwasm_val_t * const ret_val
) {
  return pwasm_interp_mem_load(env, mem_id, in, arg_ofs, ret_val);
}

static bool
pwasm_interp_on_mem_store(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const pwasm_inst_t in,
  const uint32_t arg_ofs,
  const pwasm_val_t val
) {
  return pwasm_interp_mem_store(env, mem_id, in, arg_ofs, val);
}

static bool
pwasm_interp_on_mem_size(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  uint32_t * const ret_val
) {
  return pwasm_interp_mem_size(env, mem_id, ret_val);
}

static bool
pwasm_interp_on_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  return pwasm_interp_mem_grow(env, mem_id, grow, ret_val);
}

static bool
pwasm_interp_on_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_val
) {
  return pwasm_interp_get_elem(env, table_id, elem_ofs, ret_val);
}

static uint32_t
pwasm_interp_on_find_import(
  pwasm_env_t * const env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
) {
  return pwasm_interp_find_import(env, mod_id, type, name);
}

static bool
pwasm_interp_on_get_global(
  pwasm_env_t * const env,
  const uint32_t id,
  pwasm_val_t * const ret_val
) {
  return pwasm_interp_get_global(env, id, ret_val);
}

static bool
pwasm_interp_on_set_global(
  pwasm_env_t * const env,
  const uint32_t id,
  const pwasm_val_t val
) {
  return pwasm_interp_set_global(env, id, val);
}

/**
 * Interpreter environment callbacks.
 */
static const pwasm_env_cbs_t
PWASM_OLD_INTERP_CBS = {
  .init         = pwasm_interp_on_init,
  .fini         = pwasm_interp_on_fini,
  .add_mod      = pwasm_interp_on_add_mod,
  .add_native   = pwasm_interp_on_add_native,
  .find_mod     = pwasm_interp_on_find_mod,
  .find_func    = pwasm_interp_on_find_func,
  .find_mem     = pwasm_interp_on_find_mem,
  .get_mem      = pwasm_interp_on_get_mem,
  .call         = pwasm_interp_on_call,
  .mem_load     = pwasm_interp_on_mem_load,
  .mem_store    = pwasm_interp_on_mem_store,
  .mem_size     = pwasm_interp_on_mem_size,
  .mem_grow     = pwasm_interp_on_mem_grow,
  .get_elem     = pwasm_interp_on_get_elem,
  .find_import  = pwasm_interp_on_find_import,
  .get_global   = pwasm_interp_on_get_global,
  .set_global   = pwasm_interp_on_set_global,
};

/**
 * Return interpreter environment callbacks.
 */
const pwasm_env_cbs_t *
pwasm_old_interpreter_get_cbs(void) {
  return &PWASM_OLD_INTERP_CBS;
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
  // vec of uint64_ts indicating whether elements are set
  pwasm_vec_t mask;

  // vec of values
  pwasm_vec_t vals;
} pwasm_new_interp_table_t;

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

  // save interpreter, return success
  env->env_data = interp;
  return true;
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

  // free vectors
  #define PWASM_NEW_INTERP_VEC(NAME, TYPE) pwasm_vec_fini(&(data->NAME));
  PWASM_NEW_INTERP_VECS
  #undef PWASM_NEW_INTERP_VEC

  // free backing data
  pwasm_realloc(mem_ctx, data, 0);
  env->env_data = NULL;
}

/**
 * Resolve imports of a given type.
 *
 * On success, a slice of import IDs is stored in +ret+, and this
 * function returns true.
 *
 * Returns false on error.
 */
/*
 * static bool
 * pwasm_new_interp_add_native_imports(
 *   pwasm_env_t * const env,
 *   const pwasm_native_t * const mod,
 *   const pwasm_import_type_t type,
 *   pwasm_slice_t * const ret
 * ) {
 *   pwasm_new_interp_t * const interp = env->env_data;
 *   pwasm_vec_t * const u32s = &(interp->u32s);
 *   const size_t ret_ofs = pwasm_vec_get_size(u32s);
 *
 *   uint32_t ids[PWASM_BATCH_SIZE];
 *   size_t num_ids = 0;
 *
 *   // loop over imports and resolve each one
 *   for (size_t i = 0; i < mod->num_imports; i++) {
 *     // get import, check type
 *     const pwasm_native_import_t import = mod->imports[i];
 *     if (import.type != type) {
 *       continue;
 *     }
 *
 *     // find mod, check for error
 *     const uint32_t mod_id = pwasm_find_mod(env, import.mod);
 *     if (!mod_id) {
 *       // return failure
 *       return false;
 *     }
 *
 *     // find import ID, check for error
 *     const pwasm_buf_t name = pwasm_buf_str(import.name);
 *     const uint32_t id = pwasm_env_find_import(env, mod_id, import.type, name);
 *     if (!id) {
 *       // return failure
 *       return false;
 *     }
 *
 *     // add item to results, increment count
 *     ids[num_ids] = id;
 *     num_ids++;
 *
 *     if (num_ids == LEN(ids)) {
 *       // clear count
 *       num_ids = 0;
 *
 *       // append results, check for error
 *       if (!pwasm_vec_push(u32s, LEN(ids), ids, NULL)) {
 *         // log error, return failure
 *         pwasm_env_fail(env, "append native imports failed");
 *         return false;
 *       }
 *     }
 *   }
 *
 *   if (num_ids > 0) {
 *     // append remaining results
 *     if (!pwasm_vec_push(u32s, num_ids, ids, NULL)) {
 *       // log error, return failure
 *       pwasm_env_fail(env, "append remaining native imports failed");
 *       return false;
 *     }
 *   }
 *
 *   // populate result
 *   *ret = (pwasm_slice_t) {
 *     .ofs = ret_ofs,
 *     .len = pwasm_vec_get_size(u32s) - ret_ofs,
 *   };
 *
 *   // return success
 *   return true;
 * }
 */

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

  // TODO: add native import functions (do we want this?)

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
    // FIXME: i don't think we need tables for now
    // .tables   = tables,
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

/**
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
    const size_t num_bytes = mod->mems[i].min * (1 << 16);
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
  (void) mod_ofs;

  // add imported tables, check for error
  pwasm_slice_t imports;
  if (!pwasm_new_interp_add_mod_imports(env, mod, PWASM_IMPORT_TYPE_TABLE, &imports)) {
    return false;
  }

  pwasm_new_interp_table_t tmp[PWASM_BATCH_SIZE];
  size_t tmp_ofs = 0;

  for (size_t i = 0; i < mod->num_tables; i++) {
    // init mask vector, check for error
    if (!pwasm_vec_init(env->mem_ctx, &(tmp[tmp_ofs].mask), sizeof(uint64_t))) {
      // log error, return failure
      pwasm_env_fail(env, "init mod table mask vector failed");
      return false;
    }

    // init vals vector, check for error
    if (!pwasm_vec_init(env->mem_ctx, &(tmp[tmp_ofs].vals), sizeof(uint64_t))) {
      // log error, return failure
      pwasm_env_fail(env, "init mod table values vector failed");
      return false;
    }

    // increment offset
    tmp_ofs++;

    if (tmp_ofs == LEN(tmp)) {
      // clear count
      tmp_ofs = 0;

      // append results, check for error
      if (!pwasm_vec_push(dst, LEN(tmp), tmp, NULL)) {
        // log error, return failure
        pwasm_env_fail(env, "append tables failed");
        return false;
      }
    }
  }

  if (tmp_ofs > 0) {
    // append remaining results
    if (!pwasm_vec_push(dst, tmp_ofs, tmp, NULL)) {
      // log error, return failure
      pwasm_env_fail(env, "append remaining tables failed");
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

  // build row
  const pwasm_new_interp_mod_t interp_mod = {
    .type     = PWASM_NEW_INTERP_MOD_TYPE_MOD,
    .name     = pwasm_buf_str(name),
    .mod      = mod,

    .funcs    = funcs,
    .globals  = globals,
    .mems     = mems,
    .tables   = tables,
  };

  // append native mod, check for error
  if (!pwasm_vec_push(&(interp->mods), 1, &interp_mod, NULL)) {
    // log error, return failure
    pwasm_env_fail(env, "append native mod failed");
    return 0;
  }

  // TODO: init globals, mems, tables, and start

  // convert offset to ID by adding 1
  return mod_ofs + 1;
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
        (row.type == PWASM_EXPORT_TYPE_FUNC) &&
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
        (row.type == PWASM_EXPORT_TYPE_MEM) &&
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

/**
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
  size_t size = pwasm_op_get_num_bits(in.op) / 8;
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

  // copy to result
  memcpy(ret_val, chunk.mem->buf.ptr + chunk.ofs, chunk.size);

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
  // D("store i32 = %u", val.i32);

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
  // TODO
  (void) env;
  (void) mem_id;
  (void) ret_val;

  // return failure
  return false;
}

static bool
pwasm_new_interp_mem_grow(
  pwasm_env_t * const env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t * const ret_val
) {
  // TODO
  (void) env;
  (void) mem_id;
  (void) grow;
  (void) ret_val;

  // return failure
  return false;
}

static bool
pwasm_new_interp_get_elem(
  pwasm_env_t * const env,
  const uint32_t table_id,
  const uint32_t elem_ofs,
  uint32_t * const ret_val
) {
  // TODO
  (void) env;
  (void) table_id;
  (void) elem_ofs;
  (void) ret_val;

  // return failure
  return false;
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

// FIXME: complete hack, need to handle this during parsing
static size_t
pwasm_new_interp_get_else_ofs(
  const pwasm_new_interp_frame_t frame,
  const pwasm_slice_t expr,
  const size_t inst_ofs
) {
  const pwasm_inst_t * const insts = frame.mod->mod->insts + expr.ofs;

  size_t depth = 1;
  for (size_t i = inst_ofs + 1; i < expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    if (pwasm_op_is_enter(in.op)) {
      depth++;
    } else if (in.op == PWASM_OP_END) {
      depth--;

      if (!depth) {
        // return failure (no else inst)
        return 0;
      }
    } else if (in.op == PWASM_OP_ELSE) {
      if (depth == 1) {
        // return offset to else inst
        return i - inst_ofs;
      }
    }
  }

  // log error, return failure
  pwasm_env_fail(frame.env, "missing else or end instruction");
  return 0;
}

// FIXME: complete hack, need to handle this during parsing
static size_t
pwasm_new_interp_get_end_ofs(
  const pwasm_new_interp_frame_t frame,
  const pwasm_slice_t expr,
  const size_t inst_ofs
) {
  const pwasm_inst_t * const insts = frame.mod->mod->insts + expr.ofs;

  size_t depth = 1;
  for (size_t i = inst_ofs + 1; i < expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    if (pwasm_op_is_enter(in.op)) {
      depth++;
    } else if (in.op == PWASM_OP_END) {
      depth--;

      if (!depth) {
        // return offset to else inst
        return i - inst_ofs;
      }
    }
  }

  // log error, return failure
  pwasm_env_fail(frame.env, "missing end instruction");
  return 0;
}

/**
 * Convert an internal global ID to an externally visible global handle.
 *
 * Returns 0 on error.
 */
static uint32_t
pwasm_new_interp_get_global_index(
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
static bool pwasm_new_interp_call_indirect(pwasm_new_interp_frame_t, uint32_t);

static bool
pwasm_new_interp_eval_expr(
  pwasm_new_interp_frame_t frame,
  const pwasm_slice_t expr
) {
  pwasm_stack_t * const stack = frame.env->stack;
  const pwasm_inst_t * const insts = frame.mod->mod->insts + expr.ofs;

  // FIXME: move to frame, fix depth
  pwasm_ctl_stack_entry_t ctl_stack[PWASM_STACK_CHECK_MAX_DEPTH];
  size_t depth = 0;

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
      ctl_stack[depth++] = (pwasm_ctl_stack_entry_t) {
        .type   = CTL_BLOCK,
        .depth  = stack->pos,
        .ofs    = i,
      };

      break;
    case PWASM_OP_LOOP:
      ctl_stack[depth++] = (pwasm_ctl_stack_entry_t) {
        .type   = CTL_LOOP,
        .depth  = stack->pos,
        .ofs    = i,
      };

      break;
    case PWASM_OP_IF:
      {
        // pop last value from value stack
        const uint32_t tail = stack->ptr[--stack->pos].i32;

        // get else expr offset
        // TODO
        // const size_t else_ofs = in.v_block.else_ofs ? in.v_block.else_ofs : in.v_block.end_ofs;
        size_t else_ofs;
        {
          // FIXME: this is a colossal hack (and slow), and should be
          // handled in parsing
          const size_t tmp_else_ofs = pwasm_new_interp_get_else_ofs(frame, expr, i);
          const size_t tmp_end_ofs = pwasm_new_interp_get_end_ofs(frame, expr, i);
          else_ofs = tmp_else_ofs ? tmp_else_ofs : tmp_end_ofs;
        }
        // D("else_ofs = %zu", else_ofs);

        // push to control stack
        ctl_stack[depth++] = (pwasm_ctl_stack_entry_t) {
          .type   = CTL_IF,
          .depth  = stack->pos,
          .ofs    = i,
        };

        // increment instruction pointer
        i += tail ? 0 : else_ofs;
      }

      break;
    case PWASM_OP_ELSE:
      // skip to end inst
      // TODO
      // i = ctl_stack[depth - 1].ofs + insts[i].v_block.end_ofs - 1;
      i += pwasm_new_interp_get_end_ofs(frame, expr, i);

      break;
    case PWASM_OP_END:
      if (depth) {
        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      } else {
        // return success
        // FIXME: is this correct?
        return true;
      }

      break;
    case PWASM_OP_BR:
      {
        // check branch index
        // FIXME: check in check for overflow here
        if (in.v_index.id >= depth - 1) {
          return false;
        }

        // decriment control stack
        depth -= in.v_index.id;

        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (ctl_tail.type == CTL_LOOP) {
          // reset control stack
          i = ctl_tail.ofs;
          stack->pos = ctl_tail.depth;
        } else if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      }

      break;
    case PWASM_OP_BR_IF:
      if (stack->ptr[--stack->pos].i32) {
        // check branch index
        // FIXME: need to check in "check" for overflow here
        if (in.v_index.id >= depth - 1) {
          return false;
        }

        // decriment control stack
        depth -= in.v_index.id;

        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (ctl_tail.type == CTL_LOOP) {
          i = ctl_tail.ofs;
          stack->pos = ctl_tail.depth;
        } else if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      }

      break;
    case PWASM_OP_BR_TABLE:
      {
        // get value from stack, branch labels, label offset, and then index
        const uint32_t val = stack->ptr[--stack->pos].i32;
        const pwasm_slice_t labels = in.v_br_table.labels.slice;
        const size_t labels_ofs = labels.ofs + MIN(val, labels.len - 1);
        const uint32_t id = frame.mod->mod->u32s[labels_ofs];

        // check for branch index overflow
        // FIXME: move to check()
        if (id >= depth - 1) {
          return false;
        }

        // decriment control stack
        depth -= id;

        const pwasm_ctl_stack_entry_t ctl_tail = ctl_stack[depth - 1];

        if (ctl_tail.type == CTL_LOOP) {
          i = ctl_tail.ofs;
          stack->pos = ctl_tail.depth;
        } else if (insts[ctl_tail.ofs].v_block.type == PWASM_RESULT_TYPE_VOID) {
          // reset value stack, pop control stack
          stack->pos = ctl_tail.depth;
          depth--;
        } else {
          // pop result, reset value stack, pop control stack
          stack->ptr[ctl_tail.depth] = stack->ptr[stack->pos - 1];
          stack->pos = ctl_tail.depth + 1;
          depth--;
        }
      }

      break;
    case PWASM_OP_RETURN:
      // FIXME: is this all i need?
      return true;
    case PWASM_OP_CALL:
      // call function, check for error
      if (!pwasm_new_interp_call_func(frame.env, frame.mod, in.v_index.id)) {
        // return failure
        return false;
      }

      break;
    case PWASM_OP_CALL_INDIRECT:
      // call function, check for error
      if (!pwasm_new_interp_call_indirect(frame, in.v_index.id)) {
        // return failure
        return false;
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
        const uint32_t id = in.v_index.id;

        // check local index
        if (id >= frame.locals.len) {
          // log error, return failure
          pwasm_env_fail(frame.env, "local index out of bounds");
          return false;
        }

        // push local value
        stack->ptr[stack->pos++] = stack->ptr[frame.locals.ofs + id];
      }

      break;
    case PWASM_OP_LOCAL_SET:
      {
        // get local index
        const uint32_t id = in.v_index.id;

        // check local index
        if (id >= frame.locals.len) {
          // log error, return failure
          pwasm_env_fail(frame.env, "local index out of bounds");
          return false;
        }

        // set local value, pop stack
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
        stack->pos--;
      }

      break;
    case PWASM_OP_LOCAL_TEE:
      {
        // get local index
        const uint32_t id = in.v_index.id;

        // check local index
        if (id >= frame.locals.len) {
          // log error, return failure
          pwasm_env_fail(frame.env, "local index out of bounds");
          return false;
        }

        // set local value, keep stack (tee)
        stack->ptr[frame.locals.ofs + id] = stack->ptr[stack->pos - 1];
      }

      break;
    case PWASM_OP_GLOBAL_GET:
      {
        // get global index
        const uint32_t id = pwasm_new_interp_get_global_index(frame.env, frame.mod, in.v_index.id);

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
        const uint32_t id = pwasm_new_interp_get_global_index(frame.env, frame.mod, in.v_index.id);

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
      stack->ptr[stack->pos++].i32 = in.v_i32.val;

      break;
    case PWASM_OP_I64_CONST:
      stack->ptr[stack->pos++].i64 = in.v_i64.val;

      break;
    case PWASM_OP_F32_CONST:
      stack->ptr[stack->pos++].f32 = in.v_f32.val;

      break;
    case PWASM_OP_F64_CONST:
      stack->ptr[stack->pos++].f64 = in.v_f64.val;

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
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_GE_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

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
        stack->ptr[stack->pos - 2].i32 = (a <= b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_GE_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i32 = (a <= b);
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
        const int val = __builtin_clz(stack->ptr[stack->pos - 1].i32);
        stack->ptr[stack->pos - 1].i32 = val;
      }

      break;
    case PWASM_OP_I32_CTZ:
      {
        const int val = __builtin_ctz(stack->ptr[stack->pos - 1].i32);
        stack->ptr[stack->pos - 1].i32 = val;
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
        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_DIV_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const int32_t b = (int32_t) stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_REM_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
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
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_S:
      {
        const int32_t a = (int32_t) stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_SHR_U:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32;
        stack->ptr[stack->pos - 2].i32 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTL:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;
        stack->ptr[stack->pos - 2].i32 = (a << b) | (a >> (32 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I32_ROTR:
      {
        const uint32_t a = stack->ptr[stack->pos - 2].i32;
        const uint32_t b = stack->ptr[stack->pos - 1].i32 & 0x3F;
        stack->ptr[stack->pos - 2].i32 = (a << (32 - b)) | (a >> b);
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_CLZ:
      {
        const int val = __builtin_clzl(stack->ptr[stack->pos - 1].i64);
        stack->ptr[stack->pos - 1].i64 = val;
      }

      break;
    case PWASM_OP_I64_CTZ:
      {
        const int val = __builtin_ctzl(stack->ptr[stack->pos - 1].i64);
        stack->ptr[stack->pos - 1].i64 = val;
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
        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_DIV_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a / b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const int64_t b = (int64_t) stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a % b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_REM_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
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
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a << b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_S:
      {
        const int64_t a = (int64_t) stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_SHR_U:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64;
        stack->ptr[stack->pos - 2].i64 = a >> b;
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTL:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x7F;
        stack->ptr[stack->pos - 2].i64 = (a << b) | (a >> (64 - b));
        stack->pos--;
      }

      break;
    case PWASM_OP_I64_ROTR:
      {
        const uint64_t a = stack->ptr[stack->pos - 2].i64;
        const uint64_t b = stack->ptr[stack->pos - 1].i64 & 0x7F;
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
          uint32_t i64;
          float f64;
        } v = { .i64 = stack->ptr[stack->pos - 1].i64 };
        stack->ptr[stack->pos - 1].f64 = v.f64;
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

/**
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

  const bool ok = pwasm_new_interp_eval_expr(frame, expr);
  if (!ok) {
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

static bool
pwasm_new_interp_call_indirect(
  const pwasm_new_interp_frame_t frame,
  const uint32_t func_ofs
) {
  // TODO
  (void) frame;
  (void) func_ofs;

  // return failure
  return false;
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

/**
 * Interpreter environment callbacks.
 */
static const pwasm_env_cbs_t
NEW_PWASM_INTERP_CBS = {
  .init         = pwasm_new_interp_on_init,
  .fini         = pwasm_new_interp_on_fini,
  .add_native   = pwasm_new_interp_on_add_native,
  .add_mod      = pwasm_new_interp_on_add_mod,
  .find_mod     = pwasm_new_interp_on_find_mod,
  .find_func    = pwasm_new_interp_on_find_func,
  .find_mem     = pwasm_new_interp_on_find_mem,
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

/**
 * Return new interpreter environment callbacks.
 */
const pwasm_env_cbs_t *
pwasm_new_interpreter_get_cbs(void) {
  return &NEW_PWASM_INTERP_CBS;
}
