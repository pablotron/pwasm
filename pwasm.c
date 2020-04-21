#include <stdbool.h> // bool
#include <string.h> // memcmp()
#include <stdlib.h> // realloc()
#include <unistd.h> // sysconf()
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
 */
#define PWASM_STACK_CHECK_MAX_DEPTH 512

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define LEN(ary) (sizeof(ary) / sizeof((ary)[0]))

/**
 * Call error callback (if defined), and then return 0.
 *
 * Note: used for functions that return both bool and size_t.
 */
#define FAIL(msg) do { \
  if (cbs && cbs->on_error) { \
    cbs->on_error(msg, cb_data); \
  } \
  return 0; \
} while (0)

#define INST_FAIL(msg) do { \
  if (ctx.on_error) { \
    ctx.on_error(msg, ctx.cb_data); \
  } \
  return 0; \
} while (0)

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

static inline size_t
pwasm_u32_scan(
  const pwasm_buf_t src
) {
  return pwasm_u32_decode(NULL, src);
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

#define PWASM_SECTION_TYPE(a, b) #b,
static const char *PWASM_SECTION_TYPE_NAMES[] = {
PWASM_SECTION_TYPES
};
#undef PWASM_SECTION_TYPE

const char *
pwasm_section_type_get_name(
  const pwasm_section_type_t type
) {
  const size_t ofs = MIN(PWASM_SECTION_TYPE_LAST, type);
  return PWASM_SECTION_TYPE_NAMES[ofs];
}

#define PWASM_IMPORT_TYPE(a, b, c) b,
static const char *PWASM_IMPORT_TYPE_NAMES[] = {
PWASM_IMPORT_TYPES
};
#undef PWASM_IMPORT_TYPE

const char *
pwasm_import_type_get_name(
  const pwasm_import_type_t v
) {
  const size_t ofs = MIN(PWASM_IMPORT_TYPE_LAST, v);
  return PWASM_IMPORT_TYPE_NAMES[ofs];
}

#define PWASM_EXPORT_TYPE(a, b) b,
static const char *PWASM_EXPORT_TYPE_NAMES[] = {
PWASM_EXPORT_TYPES
};
#undef PWASM_EXPORT_TYPE

const char *
pwasm_export_type_get_name(
  const pwasm_export_type_t v
) {
  const size_t ofs = MIN(PWASM_EXPORT_TYPE_LAST, v);
  return PWASM_EXPORT_TYPE_NAMES[ofs];
}

static inline bool
pwasm_is_valid_export_type(
  const uint8_t v
) {
  return v < PWASM_EXPORT_TYPE_LAST;
}

static const char *PWASM_VALUE_TYPE_NAMES[] = {
#define PWASM_VALUE_TYPE(a, b, c) c,
PWASM_VALUE_TYPE_DEFS
#undef PWASM_VALUE_TYPE
};

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
  const size_t last_ofs = LEN(PWASM_VALUE_TYPE_NAMES) - 1;
  const size_t ofs = ((v >= 0x7C) && (v <= 0x7F)) ? (0x7F - v) : last_ofs;
  return PWASM_VALUE_TYPE_NAMES[ofs];
}

const char *
pwasm_result_type_get_name(
  const pwasm_result_type_t v
) {
  return (v == 0x40) ? "void" : pwasm_value_type_get_name(v);
}

/**
 * Is this value a valid result type?
 *
 * From section 5.3.2 of the WebAssembly documentation.
 */
static inline bool
pwasm_is_valid_result_type(
  const uint8_t v
) {
  return ((v == 0x40) || pwasm_is_valid_value_type(v));
}

static const char *PWASM_IMM_NAMES[] = {
#define PWASM_IMM(a, b) b,
PWASM_IMM_DEFS
#undef PWASM_IMM
};

const char *
pwasm_imm_get_name(
  const pwasm_imm_t v
) {
  return PWASM_IMM_NAMES[MIN(v, PWASM_IMM_LAST)];
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

#define PWASM_OP_CONTROL(a, b, c) { \
  .name = (b), \
  .is_valid = true, \
  .is_control = true, \
  .imm = PWASM_IMM_##c, \
},

#define PWASM_OP_RESERVED(a, b) { \
  .name = ("reserved." b), \
  .imm = PWASM_IMM_LAST, \
},

static const struct {
  const char * name;
  bool is_control;
  bool is_valid;
  bool is_const;
  pwasm_imm_t imm;
} PWASM_OPS[] = {
PWASM_OP_DEFS
};
#undef PWASM_OP
#undef PWASM_OP_CONTROL
#undef PWASM_OP_RESERVED

const char *
pwasm_op_get_name(
  const pwasm_op_t op
) {
  return PWASM_OPS[op].name;
}

static inline bool
pwasm_op_is_valid(
  const uint8_t byte
) {
  return PWASM_OPS[byte].is_valid;
}

static inline pwasm_imm_t
pwasm_op_get_imm(
  const pwasm_op_t op
) {
  return PWASM_OPS[op].imm;
}

static inline bool
pwasm_op_is_control(
  const pwasm_op_t op
) {
  return PWASM_OPS[op].is_control;
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
  const pwasm_op_t op
) {
  return PWASM_OPS[op].is_const;
}

static const size_t
PWASM_OP_NUM_BITS[] = {
  // loads
  32, // i32.load
  64, // i64.load
  32, // F32.LOAD
  64, // F64.LOAD
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
 * Get the number of bits of the target for the given instruction.
 */
static inline uint32_t
pwasm_op_get_num_bits(
  const pwasm_op_t op
) {
  const size_t max_ofs = LEN(PWASM_OP_NUM_BITS) - 1;
  const size_t ofs = MIN(op - PWASM_OP_I32_LOAD, max_ofs);
  return PWASM_OP_NUM_BITS[ofs];
}

static void
pwasm_null_on_error(
  const char * const text,
  void * const cb_data
) {
  (void) text;
  (void) cb_data;
}

typedef struct {
  void (*on_error)(const char *, void *);
} pwasm_parse_buf_cbs_t;

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

static void
pwasm_parse_u32s_null_on_count(
  const uint32_t count,
  void *cb_data
) {
  (void) count;
  (void) cb_data;
}

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
 * Parse a vector of u32s.
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
  pwasm_slice_t slices[2] = {};
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
  void (*on_custom_section)(const pwasm_new_custom_section_t *, void *);
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
  const pwasm_new_custom_section_t section = {
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
  const pwasm_table_elem_type_t elem_type = curr.ptr[0];
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

  D("src.ptr = %p, src.len = %zu", src.ptr, src.len);

  // check source length
  if (src.len < 1) {
    cbs->on_error("short instruction", cb_data);
    return 0;
  }

  // get op, check for error
  const pwasm_op_t op = curr.ptr[0];
  if (!pwasm_op_is_valid(op)) {
    cbs->on_error("invalid op", cb_data);
    return 0;
  }

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
        uint8_t u8[len];
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
        uint8_t u8[len];
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
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);
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

  size_t depth = 1;
  size_t ofs = 0;
  while (depth && curr.len) {
    // parse instruction, check for error
    pwasm_inst_t in;
    const size_t len = pwasm_parse_inst(&in, curr, &in_cbs, cb_data);
    if (!len) {
      return 0;
    }

/* 
 *     // check op
 *     if (!pwasm_op_is_const(in.op)) {
 *       D("in.op = %u", in.op);
 *       FAIL("non-constant instruction in expr");
 *     }
 */ 

    // update depth (FIXME: check for underflow?)
    depth += pwasm_op_is_control(in.op) ? 1 : 0;
    depth -= (in.op == PWASM_OP_END) ? 1 : 0;

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;

    insts[ofs++] = in;
    if (ofs == PWASM_BATCH_SIZE) {
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

  // check for error
  if (depth > 0) {
    cbs->on_error("unterminated const expression", cb_data);
    return 0;
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
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  D("parsing expr, dst = %p", dst);

  // check source length
  if (src.len < 1) {
    cbs->on_error("invalid const expr", cb_data);
    return 0;
  }

  // build instruction parser callbacks
  const pwasm_parse_inst_cbs_t in_cbs = {
    .on_labels  = cbs->on_labels,
    .on_error   = cbs->on_error,
  };

  D("on_labels = %p, on_error = %p", in_cbs.on_labels, in_cbs.on_error);

  pwasm_inst_t insts[PWASM_BATCH_SIZE];
  pwasm_slice_t in_slice = { 0, 0 };

  size_t depth = 1;
  size_t ofs = 0;
  D("curr.len = %zu (pre)", curr.len);
  while (depth && curr.len) {
    D("curr.len = %zu", curr.len);

    // parse instruction, check for error
    pwasm_inst_t in;
    const size_t len = pwasm_parse_inst(&in, curr, &in_cbs, cb_data);
    if (!len) {
      return 0;
    }

/* 
 *     // check op
 *     if (!pwasm_op_is_const(in.op)) {
 *       D("in.op = %u", in.op);
 *       FAIL("non-constant instruction in expr");
 *     }
 */ 

    // update depth (FIXME: check for underflow?)
    depth += pwasm_op_is_control(in.op) ? 1 : 0;
    depth -= (in.op == PWASM_OP_END) ? 1 : 0;

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;

    ofs++;
    if (ofs == PWASM_BATCH_SIZE) {
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
    D("flush (final), cbs->on_insts = %p", cbs->on_insts);
    // flush batch, check for error
    const pwasm_slice_t slice = cbs->on_insts(insts, ofs, cb_data);
    if (!slice.len) {
      return 0;
    }
    D("flush (final, post), cbs->on_insts = %p", cbs->on_insts);

    if (in_slice.len) {
      // keep offset, increment slice length (not first batch)
      in_slice.len += slice.len;
    } else {
      // set offset and length (first batch)
      in_slice = slice;
    }
  }

  D("flush (after), depth = %zu", depth);

  // check for error
  if (depth > 0) {
    cbs->on_error("unterminated const expression", cb_data);
    return 0;
  }

  // save result to destination
  *dst = in_slice;

  // return number of bytes consumed
  return num_bytes;
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
  pwasm_new_global_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_expr_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  D("parsing global, src.ptr = %p, src.len = %zu", src.ptr, src.len);

  // check source length
  if (src.len < 3) {
    cbs->on_error("incomplete global", cb_data);
    return 0;
  }

  D("parsing global type dst = %p", dst);
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
    D("parsing expr (before), dst = %p", dst);
    // parse expr, check for error
    const size_t len = pwasm_parse_const_expr(&expr, curr, cbs, cb_data);
    if (!len) {
      return 0;
    }
    D("parsing expr (after), dst = %p", dst);

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  *dst = (pwasm_new_global_t) {
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
  pwasm_new_import_t * const dst,
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
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_parse_table(&(dst->table), src, cbs->on_error, cb_data);
}

static inline size_t
pwasm_parse_import_memory( // FIXME: rename to "mem"
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_parse_limits(&(dst->mem), src, cbs->on_error, cb_data);
}

static inline size_t
pwasm_parse_import_global(
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_parse_global_type(&(dst->global), src, cbs->on_error, cb_data);
}

static inline size_t
pwasm_parse_import_invalid(
  pwasm_new_import_t * const dst,
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
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_import_cbs_t * const cbs,
  void *cb_data
) {
  D("type = %u:%s, src.ptr = %p, src.len = %zu", type, pwasm_import_type_get_name(type), src.ptr, src.len);
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
  pwasm_new_import_t * const dst,
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
  pwasm_slice_t names[2] = {};
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

    D("curr.ptr = %p, curr.len = %zu", curr.ptr, curr.len);
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
  pwasm_new_import_t tmp = {
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

  D("dst = %p, num_bytes = %zu", dst, num_bytes);

  // save result to destination
  *dst = tmp;

  D("dst = %p, num_bytes = %zu (tmp written)", dst, num_bytes);

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
  pwasm_new_export_t * const dst,
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

  *dst = (pwasm_new_export_t) {
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
  const pwasm_buf_t src,
  const pwasm_parse_code_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;
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

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;

    // increment offset
    ofs++;
    if (ofs == PWASM_BATCH_SIZE) {
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

  // copy result to destination
  *dst = local;

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
  {
    // parse locals, check for error
    const size_t len = pwasm_parse_code_locals(&locals, curr, cbs, cb_data);
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

#define BUILDER_VECS \
  BUILDER_VEC(u32, uint32_t, dummy) \
  BUILDER_VEC(section, uint32_t, u32) \
  BUILDER_VEC(custom_section, pwasm_new_custom_section_t, section) \
  BUILDER_VEC(type, pwasm_type_t, custom_section) \
  BUILDER_VEC(import, pwasm_new_import_t, type) \
  BUILDER_VEC(inst, pwasm_inst_t, import) \
  BUILDER_VEC(global, pwasm_new_global_t, inst) \
  BUILDER_VEC(func, uint32_t, global) \
  BUILDER_VEC(table, pwasm_table_t, func) \
  BUILDER_VEC(mem, pwasm_limits_t, table) \
  BUILDER_VEC(export, pwasm_new_export_t, mem) \
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
  if (!ptr) {
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

DEF_VEC_PARSER(type, pwasm_type_t);
DEF_VEC_PARSER(import, pwasm_new_import_t);
DEF_VEC_PARSER(func, uint32_t);
DEF_VEC_PARSER(table, pwasm_table_t);
DEF_VEC_PARSER(mem, pwasm_limits_t);
DEF_VEC_PARSER(global, pwasm_new_global_t);
DEF_VEC_PARSER(export, pwasm_new_export_t);
DEF_VEC_PARSER(elem, pwasm_elem_t);
DEF_VEC_PARSER(code, pwasm_func_t);
DEF_VEC_PARSER(segment, pwasm_segment_t);

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
  pwasm_new_import_t * const dst,
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
  pwasm_new_global_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void * const cb_data
) {
  D("src_cbs = %p", src_cbs);

  const pwasm_parse_expr_cbs_t cbs = {
    .on_labels  = src_cbs->on_labels,
    .on_insts   = src_cbs->on_insts,
    .on_error   = src_cbs->on_error,
  };
  D("(after cbs) src_cbs->on_insts = %p, cbs.on_insts = %p", src_cbs->on_insts, cbs.on_insts);

  return pwasm_parse_global(dst, src, &cbs, cb_data);
}

static size_t
pwasm_mod_parse_export(
  pwasm_new_export_t * const dst,
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
  D("type = src.ptr = %p, type = %u:%s", src.ptr, type, pwasm_section_type_get_name(type));
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

#if 0
static pwasm_slice_t
pwasm_mod_parse_null_on_u32s(
  const uint32_t * const ptr,
  const size_t len,
  void *cb_data
) {
  (void) ptr;
  (void) len;
  (void) cb_data;
  return (pwasm_slice_t) { 0, len };
}

static pwasm_slice_t
pwasm_mod_parse_null_on_bytes(
  const uint8_t * const ptr,
  const size_t len,
  void *cb_data
) {
  (void) ptr;
  (void) len;
  (void) cb_data;
  return (pwasm_slice_t) { 0, len };
}

static void
pwasm_mod_parse_null_on_section(
  const pwasm_header_t * const header,
  void *cb_data
) {
  (void) header;
  (void) cb_data;
}

static void
pwasm_mod_parse_null_on_custom_section(
  const pwasm_new_custom_section_t * const section,
  void *cb_data
) {
  (void) section;
  (void) cb_data;
}

static inline pwasm_mod_parse_cbs_t
pwasm_mod_parse_get_cbs(
  const pwasm_mod_parse_cbs_t * const cbs
) {
  // FIXME: not working right
  // return callbacks
  return (pwasm_mod_parse_cbs_t) {
    .on_error = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error,
    .on_bytes = (cbs && cbs->on_bytes) ? cbs->on_bytes : pwasm_mod_parse_null_on_bytes,
    .on_u32s = (cbs && cbs->on_u32s) ? cbs->on_u32s : pwasm_mod_parse_null_on_u32s,
    // .on_insts = (cbs && cbs->on_insts) ? cbs->on_insts : pwasm_mod_parse_null_on_insts,
    // .on_labels = (cbs && cbs->on_labels) ? cbs->on_labels : pwasm_mod_parse_null_on_labels,
    .on_section = (cbs && cbs->on_section) ? cbs->on_section : pwasm_mod_parse_null_on_section,
    .on_custom_section = (cbs && cbs->on_custom_section) ? cbs->on_custom_section : pwasm_mod_parse_null_on_custom_section,
  };
}
#endif /* 0 */

size_t
pwasm_mod_parse(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void *cb_data
) {
  D("src_cbs = %p", src_cbs);

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

  D("bytes = %p, num = %zu", bytes, num);

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
  const pwasm_new_custom_section_t * const section,
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
  const pwasm_new_import_t * const rows,
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
        const pwasm_new_global_t global = {
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

  for (size_t i = 0; i < num; i++) {
    D("inst[%zu].op = %u", i, rows[i].op);
  }

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
  const pwasm_new_global_t * const rows,
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
  const pwasm_new_export_t * const rows,
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
  if (!pwasm_builder_push_codes(data->builder, rows, num).len) {
    pwasm_mod_init_unsafe_on_error("push codes failed", data);
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
  // TODO add checks
  return pwasm_mod_init_unsafe(mem_ctx, mod, src);
}

void
pwasm_mod_fini(
  pwasm_mod_t * const mod
) {
  if (mod->mem.ptr) {
    return; // FIXME: we're crashing here
    pwasm_realloc(mod->mem_ctx, (void*) mod->mem.ptr, 0);
    mod->mem.ptr = 0;
    mod->mem.len = 0;
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
pwasm_env_call(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name
) {
  const pwasm_env_cbs_t * const cbs = env->cbs;
  D("env = %p, mod = \"%s\", name = \"%s\"", env, mod, name);
  return (cbs && cbs->call) ? cbs->call(env, mod, name) : false;
}

typedef enum {
  PWASM_INTERP_ROW_TYPE_MOD,
  PWASM_INTERP_ROW_TYPE_NATIVE,
  PWASM_INTERP_ROW_TYPE_GLOBAL,
  PWASM_INTERP_ROW_TYPE_MEM,
  PWASM_INTERP_ROW_TYPE_FUNC,
  PWASM_INTERP_ROW_TYPE_LAST,
} pwasm_interp_row_type_t;

typedef struct {
  pwasm_interp_row_type_t type;

  pwasm_buf_t name;

  union {
    const pwasm_mod_t *mod;
    const pwasm_native_t *native;
    pwasm_buf_t mem;
    pwasm_val_t global;

    struct {
      size_t mod_ofs;
      uint32_t func_id;
    } func;
  };
} pwasm_interp_row_t;

typedef struct {
  pwasm_vec_t rows;
} pwasm_interp_t;

static bool
pwasm_interp_on_init(
  pwasm_env_t * const env
) {
  const size_t data_size = sizeof(pwasm_interp_t);
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // allocate interpreter data store
  pwasm_interp_t *data = pwasm_realloc(mem_ctx, NULL, data_size);
  if (!data) {
    D("pwasm_realloc() failed (size = %zu)", data_size);
    return false;
  }

  // allocate rows
  if (!pwasm_vec_init(mem_ctx, &(data->rows), sizeof(pwasm_interp_row_t))) {
    D("pwasm_vec_init() failed (stride = %zu)", sizeof(pwasm_interp_row_t));
    return false;
  }

  // save data, return success
  env->env_data = data;
  return true;
}

static void
pwasm_interp_on_fini(
  pwasm_env_t * const env
) {
  pwasm_mem_ctx_t * const mem_ctx = env->mem_ctx;

  // get interpreter data
  pwasm_interp_t *data = env->env_data;
  if (!data) {
    return;
  }

  // free rows
  pwasm_vec_fini(&(data->rows));

  // free backing data
  pwasm_realloc(mem_ctx, data, 0);
  env->env_data = NULL;
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
    return 0;
  }

  // add row, get offset
  size_t ofs = 0;
  if (!pwasm_vec_push(&(data->rows), 1, row, &ofs)) {
    D("pwasm_vec_init() failed %s", "");
    return 0;
  }

  // return offset + 1 (to prevent zero IDs)
  return ofs + 1;
}

static uint32_t
pwasm_interp_on_add_mod(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_mod_t * const mod
) {
  // build row
  const pwasm_interp_row_t row = {
    .type = PWASM_INTERP_ROW_TYPE_MOD,

    .name = {
      .ptr = (uint8_t*) name,
      .len = strlen(name),
    },

    .mod  = mod,
  };

  return pwasm_interp_add_row(env, &row);
}

static uint32_t
pwasm_interp_on_add_native(
  pwasm_env_t * const env,
  const char * const name,
  const pwasm_native_t * const mod
) {
  // build row
  const pwasm_interp_row_t row = {
    .type     = PWASM_INTERP_ROW_TYPE_NATIVE,

    .name = {
      .ptr = (uint8_t*) name,
      .len = strlen(name),
    },

    .native   = mod,
  };

  return pwasm_interp_add_row(env, &row);
}

/**
 * world's shittiest initial interpreter
 */
static bool
pwasm_interp_call(
  pwasm_env_t * const env,
  const pwasm_mod_t * const mod,
  uint32_t func_id
) {
  const size_t num_import_funcs = mod->num_import_types[PWASM_IMPORT_TYPE_FUNC];
  pwasm_stack_t * const stack = env->stack;

  if (func_id < num_import_funcs) {
    // TODO
    D("imported function not supported yet: %u", func_id);
    return false;
  }

  // map to internal function ID
  func_id -= num_import_funcs;
  if (func_id >= mod->num_funcs) {
    // TODO: invalid function ID, log error
    D("invalid function ID: %u", func_id);
    return false;
  }

  const pwasm_slice_t params = mod->types[mod->funcs[func_id]].params;
  const pwasm_slice_t results = mod->types[mod->funcs[func_id]].results;
  if (stack->pos < params.len) {
    // TODO: missing parameters
    D("missing parameters: stack->pos = %zu, params.len = %zu", stack->pos, params.len);
    return false;
  }

  const pwasm_slice_t expr = mod->codes[func_id].expr;
  const pwasm_inst_t * const insts = mod->insts + expr.ofs;
  D("expr = { .ofs = %zu, .len = %zu }, num_insts = %zu", expr.ofs, expr.len, mod->num_insts);
  for (size_t i = 0; i < expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    D("in.op = %d", in.op);
    switch (in.op) {
    case PWASM_OP_END:
      // FIXME: need to check results here better
      return (stack->pos == results.len);
    case PWASM_OP_I32_CONST:
      stack->ptr[stack->pos++].i32 = in.v_i32.val;

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
    default:
      // TODO: unsupported inst
      return false;
    }
  }

  // return failure
  return false;
}

static bool
pwasm_interp_on_call(
  pwasm_env_t * const env,
  const char * const mod,
  const char * const name
) {
  pwasm_interp_t * const data = env->env_data;
  const pwasm_interp_row_t *rows = pwasm_vec_get_data(&(data->rows));
  const size_t num_rows = pwasm_vec_get_size(&(data->rows));
  const size_t mod_len = strlen(mod);
  const size_t name_len = strlen(name);

  // FIXME: everything about this is a horrific slow mess, so fix it
  for (size_t i = 0; i < num_rows; i++) {
    switch (rows[i].type) {
    case PWASM_INTERP_ROW_TYPE_MOD:
      if (
          (rows[i].name.len == mod_len) &&
          !memcmp(mod, rows[i].name.ptr, mod_len)
        ) {
        for (uint32_t j = 0; j < rows[i].mod->num_exports; j++) {
          const pwasm_new_export_t export = rows[i].mod->exports[j];
          if (
            (export.type == PWASM_EXPORT_TYPE_FUNC) &&
            (export.name.len == name_len) &&
            !memcmp(rows[i].mod->bytes + export.name.ofs, name, name_len)
          ) {
            D("found func, calling it: %u", export.id);
            return pwasm_interp_call(env, rows[i].mod, export.id);
          }
        }

        // return failure
        return false;
      }

      break;
    case PWASM_INTERP_ROW_TYPE_NATIVE:
      if (
          (rows[i].name.len == mod_len) &&
          !memcmp(mod, rows[i].name.ptr, mod_len)
      ) {
        for (size_t j = 0; j < rows[i].native->num_funcs; j++) {
          const pwasm_native_func_t * const func = rows[i].native->funcs + j;
          if (!strncmp(name, func->name, name_len)) {
            return func->func(env, env->stack);
          }
        }

        // return failure
        return false;
      }

      break;
    default:
      break;
    }
  }

  // return failure
  return false;
}

static const pwasm_env_cbs_t
PWASM_INTERP_CBS = {
  .init       = pwasm_interp_on_init,
  .fini       = pwasm_interp_on_fini,
  .add_mod    = pwasm_interp_on_add_mod,
  .add_native = pwasm_interp_on_add_native,
  .call       = pwasm_interp_on_call,
};

const pwasm_env_cbs_t *
pwasm_interpreter_get_cbs(void) {
  return &PWASM_INTERP_CBS;
}
