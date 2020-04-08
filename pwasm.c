#include <stdbool.h> // bool
#include <string.h> // memcmp()
#include <stdlib.h> // realloc()
#include <unistd.h> // sysconf()
#include "pwasm.h"

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

typedef struct {
  void (*on_error)(const char *, void *);
  void *cb_data;
} pwasm_old_parse_inst_ctx_t;

typedef struct {
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_inst_cbs_t;

/**
 * Batch size.
 *
 * Used to batch up function types, imports, functions,
 * etc, when dispatching to parsing callbacks.
 *
 * Note: must be a power of two.
 */
#define PWASM_BATCH_SIZE 128

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
  static size_t \
  pwasm_mod_parse_ ## NAME ## s ( \
    const pwasm_buf_t src, \
    const pwasm_mod_parse_cbs_t * const cbs, \
    void *cb_data \
  ) { \
    /* get count, check for error */ \
    uint32_t count = 0; \
    size_t num_bytes = pwasm_u32_decode(&count, src); \
    if (!num_bytes) { \
      cbs->on_error(#NAME "s: invalid count", cb_data); \
      return 0; \
    } \
    \
    pwasm_buf_t curr = pwasm_buf_step(src, num_bytes); \
    \
    /* element buffer and offset */ \
    TYPE dst[PWASM_BATCH_SIZE]; \
    size_t ofs = 0; \
    \
    /* parse items */ \
    for (size_t left = count; left > 0; left--) { \
      /* check for underflow */ \
      if (!curr.len) { \
        cbs->on_error(#NAME "s: underflow", cb_data); \
        return 0; \
      } \
      /* parse element, check for error (FIXME: remove NULLs) */ \
      const size_t len = pwasm_mod_parse_ ## NAME (dst + ofs, curr, NULL, NULL); \
      if (!len) { \
        return 0; \
      } \
      \
      /* increment num_bytes, increment offset, advance buffer */ \
      num_bytes += len; \
      ofs++; \
      curr = pwasm_buf_step(curr, len); \
      \
      if (ofs == PWASM_BATCH_SIZE) { \
        /* flush batch */ \
        cbs->on_ ## NAME ## s(dst, PWASM_BATCH_SIZE, cb_data); \
        ofs = 0; \
      } \
    } \
    \
    if (ofs > 0) { \
      /* flush remaining items */ \
      cbs->on_ ## NAME ## s(dst, ofs, cb_data); \
    } \
    \
    /* return number of bytes consumed */ \
    return num_bytes; \
  }

#define DEF_VEC_PARSE_FN(FN_NAME, TEXT, EL_TYPE, CBS_TYPE, PARSE_FN, FLUSH_CB) \
static size_t FN_NAME ( \
  const pwasm_buf_t src, \
  const CBS_TYPE * const cbs, \
  void * const cb_data \
) { \
  /* get count, check for error */ \
  uint32_t num_els = 0; \
  size_t src_ofs = pwasm_u32_decode(&num_els, src); \
  if (!src_ofs) { \
    FAIL(TEXT ": invalid vector length"); \
  } \
  \
  /* element buffer */ \
  EL_TYPE dst[PWASM_BATCH_SIZE]; \
  \
  for (size_t i = 0; i < num_els; i++) { \
    const size_t dst_ofs = (i & (PWASM_BATCH_SIZE - 1)); \
    \
    /* parse element, check for error */ \
    const size_t used = PARSE_FN(dst + dst_ofs, cbs, src.ptr + src_ofs, src.len - src_ofs, cb_data); \
    if (!used) { \
      return 0; \
    } \
    \
    /* increment offset, check for error */ \
    src_ofs += used; \
    if (src_ofs > src.len) { \
      FAIL(TEXT ": source buffer length overflow"); \
    } \
    \
    if ((dst_ofs == (PWASM_BATCH_SIZE - 1)) && cbs && cbs->FLUSH_CB) { \
      /* flush batch */ \
      cbs->FLUSH_CB(dst, PWASM_BATCH_SIZE, cb_data); \
    } \
  } \
  \
  /* count remaining entries */ \
  const size_t num_left = num_els & (PWASM_BATCH_SIZE - 1); \
  if (num_left && cbs && cbs->FLUSH_CB) { \
    /* flush remaining entries */ \
    cbs->FLUSH_CB(dst, num_left, cb_data); \
  } \
  \
  /* return number of bytes consumed */ \
  return src_ofs; \
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
  const pwasm_parse_buf_cbs_t cbs = {
    .on_error = (src_cbs && src_cbs->on_error) ?
                src_cbs->on_error :
                pwasm_null_on_error,
  };

  // check source length
  if (!src.len) {
    cbs.on_error("empty name", cb_data);
    return 0;
  }

  // decode count, check for error
  uint32_t count = 0;
  const size_t len = pwasm_u32_decode(&count, src);
  if (!len) {
    cbs.on_error("bad name length", cb_data);
    return 0;
  }

  // D("src: %p, src_len = %zu, len = %u, len_ofs = %zu", src, src_len, len, len_ofs);

  // calculate total number of bytes, check for overflow
  const size_t num_bytes = count + len;
  if (num_bytes > src.len) {
    cbs.on_error("truncated name", cb_data);
    return 0;
  }

  if (dst) {
    // build result, save result to destination
    *dst = (pwasm_buf_t) { src.ptr + len, count };
  }

  // return number of bytes consumed
  return num_bytes;
}

static size_t
pwasm_parse_name(
  pwasm_buf_t * const dst,
  const pwasm_parse_module_cbs_t * const mod_cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  const pwasm_parse_buf_cbs_t cbs = {
    .on_error = mod_cbs ? mod_cbs->on_error : NULL,
  };

  return pwasm_parse_buf(dst, src, &cbs, cb_data);
}

static size_t
pwasm_parse_value_type_list(
  pwasm_buf_t * const ret_buf,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (!src_len) {
    FAIL("empty value type list");
  }

  // decode buffer length, check for error
  uint32_t len = 0;
  const size_t len_ofs = pwasm_u32_decode(&len, (pwasm_buf_t) { src, src_len });
  if (!len_ofs) {
    FAIL("bad value type list length");
  }

  // calculate total number of bytes, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src_len) {
    FAIL("value type list length too long");
  }

  // build result
  const pwasm_buf_t buf = {
    .ptr = src + len_ofs,
    .len = len,
  };

  // check value types
  for (size_t i = 0; i < buf.len; i++) {
    // D("buf[%zu] = %02x", i, buf.ptr[i]);
    if (!pwasm_is_valid_value_type(buf.ptr[i])) {
      // invalid value type, return error
      FAIL("bad value type list entry");
    }
  }

  if (ret_buf) {
    // save to result
    *ret_buf = buf;
  }

  // return section length, in bytes
  return num_bytes;
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
      cbs.on_items(items, PWASM_BATCH_SIZE, cb_data);
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
  const pwasm_parse_u32s_cbs_t * const cbs;
  void *cb_data;
} pwasm_old_parse_labels_t;

static void
pwasm_old_parse_labels_on_count(
  const uint32_t count,
  void *cb_data
) {
  pwasm_old_parse_labels_t * const data = cb_data;

  if (data->cbs && data->cbs->on_count) {
    data->cbs->on_count(count + 1, data->cb_data);
  }
}

static void
pwasm_old_parse_labels_on_items(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_old_parse_labels_t * const data = cb_data;

  if (data->cbs && data->cbs->on_items) {
    data->cbs->on_items(rows, num, data->cb_data);
  }
}

static void
pwasm_old_parse_labels_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_old_parse_labels_t * const data = cb_data;

  if (data->cbs && data->cbs->on_error) {
    data->cbs->on_error(text, data->cb_data);
  }
}

static const pwasm_parse_u32s_cbs_t
PWASM_OLD_PARSE_LABELS_CBS = {
  .on_count = pwasm_old_parse_labels_on_count,
  .on_items = pwasm_old_parse_labels_on_items,
  .on_error = pwasm_old_parse_labels_on_error,
};

static size_t
pwasm_old_parse_labels(
  const pwasm_buf_t src,
  const pwasm_parse_u32s_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_old_parse_labels_t data = {
    .cbs = cbs,
    .cb_data = cb_data,
  };

  // parse labels, check for error
  const size_t len = pwasm_parse_u32s(src, &PWASM_OLD_PARSE_LABELS_CBS, &data);
  if (!len) {
    return 0;
  }

  const pwasm_buf_t buf = pwasm_buf_step(src, len);

  // get default label, check for error
  uint32_t label;
  const size_t label_len = pwasm_u32_decode(&label, buf);
  if (!label_len) {
    const char * const text = "br_table: bad default label";
    pwasm_old_parse_labels_on_error(text, &data);
    return 0;
  }

  // pass default label to callback
  pwasm_old_parse_labels_on_items(&label, 1, &data);

  // return total number of bytes consumed
  return len + label_len;
}

static void
pwasm_count_labels_on_count(
  const uint32_t count,
  void *cb_data
) {
  uint32_t * const ret = cb_data;
  *ret = count;
}

static const pwasm_parse_u32s_cbs_t
PWASM_COUNT_LABELS_CBS = {
  .on_count = pwasm_count_labels_on_count,
};

/**
 * Count number of labels in a buffer containing the labels for a
 * br_table instruction.
 *
 * Note: The count includes the default value, so a successful result
 * will always be greater than zero.
 *
 * Returns 0 on error.
 */
static size_t
pwasm_count_labels(
  const pwasm_buf_t src
) {
  uint32_t count = 0;

  const size_t len = pwasm_old_parse_labels(src, &PWASM_COUNT_LABELS_CBS, &count);
  return (len > 0) ? count : 0;
}

static bool
pwasm_parse_custom_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  // parse name, check for error
  pwasm_buf_t name;
  const size_t ofs = pwasm_parse_name(&name, cbs, src, cb_data);
  if (!ofs) {
    return false;
  }

  // build custom section
  const pwasm_custom_section_t section = {
    .name = {
      .ptr = name.ptr,
      .len = name.len,
    },

    .data = {
      .ptr = src.ptr + ofs,
      .len = src.len - ofs,
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
pwasm_parse_function_type(
  pwasm_function_type_t * const dst_func_type,
  const pwasm_parse_module_cbs_t * const cbs,
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
  pwasm_buf_t params;
  const size_t params_len = pwasm_parse_value_type_list(&params, cbs, src + 1, src_len - 1, cb_data);
  if (!params_len) {
    return 0;
  }

  // build results offset, check for error
  const size_t results_ofs = 1 + params_len;
  if (results_ofs >= src_len) {
    FAIL("bad function type: missing results");
  }

  // parse results, check for error
  pwasm_buf_t results;
  const size_t results_len = pwasm_parse_value_type_list(&results, cbs, src + results_ofs, src_len - results_ofs, cb_data);
  if (!results_len) {
    return 0;
  }

  // build result
  const pwasm_function_type_t src_func_type = {
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

DEF_VEC_PARSE_FN(
  pwasm_parse_types,
  "parse types",
  pwasm_function_type_t,
  pwasm_parse_module_cbs_t,
  pwasm_parse_function_type,
  on_function_types
);

static bool
pwasm_parse_type_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_parse_types(src, cbs, cb_data);
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
 * Parse limits into +dst+ from buffer +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_old_parse_limits(
  pwasm_limits_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src_ptr,
  const size_t src_len,
  void * const cb_data
) {
  const pwasm_buf_t src = { src_ptr, src_len };
  const void (*cb) = cbs && cbs->on_error ? cbs->on_error : pwasm_null_on_error;
  return pwasm_parse_limits(dst, src, cb, cb_data);
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
/**
 * Parse table into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_old_parse_table(
  pwasm_table_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src_ptr,
  const size_t src_len,
  void * const cb_data
) {
  const pwasm_buf_t src = { src_ptr, src_len };
  const void (*cb) = cbs && cbs->on_error ? cbs->on_error : pwasm_null_on_error;
  return pwasm_parse_table(dst, src, cb, cb_data);
}

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
      const size_t len = 4;

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
      const size_t len = 8;

      // check length
      if (curr.len < len) {
        cbs->on_error("incomplete f64", cb_data);
        return 0;
      }

      union {
        uint8_t u8[len];
        float f64;
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

/**
 * Parse inst into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_old_parse_inst(
  pwasm_inst_t * const dst,
  const pwasm_old_parse_inst_ctx_t ctx,
  const pwasm_buf_t src
) {
  // check source length
  if (src.len < 1) {
    INST_FAIL("short instruction");
  }

  // get op, check for error
  const pwasm_op_t op = src.ptr[0];
  if (!pwasm_op_is_valid(op)) {
    INST_FAIL("invalid op");
  }

  // build length and instruction
  size_t len = 1;
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
      if (src.len < 2) {
        INST_FAIL("missing result type immediate");
      }

      // get block result type, check for error
      const uint8_t type = src.ptr[1];
      if (!pwasm_is_valid_result_type(type)) {
        INST_FAIL("invalid result type");
      }

      // save result type, increment length
      in.v_block.type = type;
      len += 1;
    }

    break;
  case PWASM_IMM_BR_TABLE:
    {
      // parse labels immediate, check for error
      const pwasm_buf_t l_buf = pwasm_buf_step(src, 1);
      const size_t l_len = pwasm_old_parse_labels(l_buf, NULL, NULL);
      if (!l_len) {
        INST_FAIL("bad br_table labels immediate");
      }

      // save labels buffer, increment length
      in.v_br_table.labels.buf.ptr = src.ptr + 1;
      in.v_br_table.labels.buf.len = l_len;
      len += l_len;
    }

    break;
  case PWASM_IMM_INDEX:
    {
      // get index, check for error
      uint32_t id = 0;
      const size_t id_len = pwasm_u32_decode(&id, pwasm_buf_step(src, 1));
      if (!id_len) {
        INST_FAIL("bad immediate index value");
      }

      // save index, increment length
      in.v_index.id = id;
      len += id_len;
    }

    break;
  case PWASM_IMM_CALL_INDIRECT:
    {
      // get index, check for error
      uint32_t id = 0;
      const size_t id_len = pwasm_u32_decode(&id, pwasm_buf_step(src, 1));
      if (!id_len) {
        INST_FAIL("bad immediate index value");
      }

      // check remaining length
      if (len + id_len >= src.len) {
        INST_FAIL("truncated call_immediate");
      }

      // check call_indirect table index
      if (src.ptr[len + id_len] != 0) {
        INST_FAIL("invalid call_indirect table index");
      }

      // save index, increment length
      in.v_index.id = id;
      len += id_len + 1;
    }

    break;
  case PWASM_IMM_MEM:
    {
      // get align value, check for error
      uint32_t align = 0;
      const size_t a_len = pwasm_u32_decode(&align, pwasm_buf_step(src, 1));
      if (!a_len) {
        INST_FAIL("bad align value");
      }

      // get offset value, check for error
      uint32_t offset = 0;
      const size_t o_len = pwasm_u32_decode(&offset, pwasm_buf_step(src, 1 + a_len));
      if (!o_len) {
        INST_FAIL("bad offset value");
      }

      // save alignment and offset, increment length
      in.v_mem.align = align;
      in.v_mem.offset = offset;
      len += a_len + o_len;
    }

    break;
  case PWASM_IMM_I32_CONST:
    {
      // get value, check for error
      uint32_t val = 0;
      const size_t v_len = pwasm_u32_decode(&val, pwasm_buf_step(src, 1));
      if (!v_len) {
        INST_FAIL("bad align value");
      }

      // save value, increment length
      in.v_i32.val = val;
      len += v_len;
    }

    break;
  case PWASM_IMM_I64_CONST:
    {
      // get value, check for error
      uint64_t val = 0;
      const size_t v_len = pwasm_u64_decode(&val, pwasm_buf_step(src, 1));
      if (!v_len) {
        INST_FAIL("bad align value");
      }

      // save value, increment length
      in.v_i64.val = val;
      len += v_len;
    }

    break;
  case PWASM_IMM_F32_CONST:
    {
      // immediate size, in bytes
      const size_t imm_len = 4;

      // check length
      if (src.len - 1 < imm_len) {
        INST_FAIL("incomplete f32");
      }

      union {
        uint8_t u8[imm_len];
        float f32;
      } u;
      memcpy(u.u8, src.ptr + 1, imm_len);

      // save value, increment length
      in.v_f32.val = u.f32;
      len += imm_len;
    }

    break;
  case PWASM_IMM_F64_CONST:
    {
      // immediate size, in bytes
      const size_t imm_len = 8;

      // check length
      if (src.len - 1 < imm_len) {
        INST_FAIL("incomplete f64");
      }

      union {
        uint8_t u8[imm_len];
        double f64;
      } u;
      memcpy(u.u8, src.ptr + 1, imm_len);

      // save value, increment length
      in.v_f64.val = u.f64;
      len += imm_len;
    }

    break;
  default:
    // never reached
    INST_FAIL("invalid immediate type");
  }

  if (dst) {
    *dst = in;
  }

  // return number of bytes consumed
  return len;
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
pwasm_old_parse_const_expr(
  pwasm_expr_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (src_len < 1) {
    FAIL("invalid const expr");
  }

  // build instruction parser context
  const pwasm_old_parse_inst_ctx_t in_ctx = {
    .on_error = cbs->on_error,
    .cb_data  = cb_data,
  };

  size_t depth = 1;
  size_t ofs = 0;
  while ((depth > 0) && (ofs < src_len)) {
    // pack instruction source buffer
    const pwasm_buf_t in_src = {
      .ptr = src + ofs,
      .len = src_len - ofs,
    };

    // parse instruction, check for error
    pwasm_inst_t in;
    const size_t len = pwasm_old_parse_inst(&in, in_ctx, in_src);
    if (!len) {
      return 0;
    }

    // check op
    if (!pwasm_op_is_const(in.op)) {
      D("in.op = %u", in.op);
      FAIL("non-constant instruction in expr");
    }

    // update depth
    depth += pwasm_op_is_control(in.op) ? 1 : 0;
    depth -= (in.op == PWASM_OP_END) ? 1 : 0;

    // increment offset
    ofs += len;
  }

  // check for error
  if (depth > 0) {
    FAIL("unterminated const expression");
  }

  const pwasm_expr_t tmp = {
    .buf = {
      .ptr = src,
      .len = ofs,
    },
  };

  if (dst) {
    // copy result
    *dst = tmp;
  }

  // return number of bytes consumed
  return ofs;
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
 * Parse global type into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_old_parse_global_type(
  pwasm_global_type_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src_ptr,
  const size_t src_len,
  void * const cb_data
) {
  const pwasm_buf_t src = { src_ptr, src_len };
  const void (*cb) = cbs && cbs->on_error ? cbs->on_error : pwasm_null_on_error;
  return pwasm_parse_global_type(dst, src, cb, cb_data);
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

  // check source length
  if (src.len < 3) {
    cbs->on_error("incomplete global", cb_data);
    return 0;
  }

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
    // parse expr, check for error
    const size_t len = pwasm_parse_const_expr(&expr, curr, cbs, cb_data);
    if (!len) {
      return 0;
    }

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

/**
 * Parse global into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_old_parse_global(
  pwasm_global_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (src_len < 3) {
    FAIL("incomplete global");
  }

  // parse type, check for error
  pwasm_global_type_t type;
  const size_t type_len = pwasm_old_parse_global_type(&type, cbs, src, src_len, cb_data);
  if (!type_len) {
    return 0;
  }

  // parse expr, check for error
  pwasm_expr_t expr;
  const size_t expr_len = pwasm_old_parse_const_expr(&expr, cbs, src + type_len, src_len - type_len, cb_data);
  if (!expr_len) {
    return 0;
  }

  const pwasm_global_t tmp = {
    .type = type,
    .expr = expr,
  };

  if (dst) {
    // copy to result
    *dst = tmp;
  }

  // return total number of bytes consumed
  return type_len + expr_len;
}

/**
 * Parse import into +dst_import+ from source buffer +src+, consuming a
 * maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_parse_import(
  pwasm_import_t * const dst_import,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  const pwasm_buf_t mod_src = {
    .ptr = src,
    .len = src_len,
  };

  // parse module name, check for error
  pwasm_buf_t mod;
  const size_t mod_len = pwasm_parse_name(&mod, cbs, mod_src, cb_data);
  if (!mod_len) {
    return false;
  }

  const pwasm_buf_t name_src = {
    .ptr = mod_src.ptr + mod_len,
    .len = mod_src.len - mod_len,
  };

  // parse name, check for error
  pwasm_buf_t name;
  const size_t name_len = pwasm_parse_name(&name, cbs, name_src, cb_data);
  if (!name_len) {
    return false;
  }

  // get import type
  const pwasm_import_type_t type = src[mod_len + name_len];

  pwasm_import_t tmp = {
    .module = mod,
    .name = name,
    .type = type,
  };

  // calculate number of bytes consumed so far
  size_t num_bytes = mod_len + name_len + 1;

  // check length
  if (num_bytes >= src_len) {
    FAIL("incomplete import descriptor");
  }

  const uint8_t * const data_ptr = src + num_bytes;
  const size_t data_len = src_len - num_bytes;

  switch (type) {
  case PWASM_IMPORT_TYPE_FUNC:
    {
      const size_t len = pwasm_u32_decode(&(tmp.func.id), (pwasm_buf_t) {
        data_ptr,
        data_len
      });
      if (!len) {
        FAIL("invalid function import type");
      }

      // add length to result
      num_bytes += len;
    }

    break;
  case PWASM_IMPORT_TYPE_TABLE:
    {
      // parse table, check for error
      const size_t len = pwasm_old_parse_table(&(tmp.table), cbs, data_ptr, data_len, cb_data);
      if (!len) {
        return false;
      }

      // add length to result
      num_bytes += len;
    }

    break;
  case PWASM_IMPORT_TYPE_MEM:
    {
      // parse memory limits, check for error
      const size_t len = pwasm_old_parse_limits(&(tmp.mem.limits), cbs, data_ptr, data_len, cb_data);
      if (!len) {
        return false;
      }

      // add length to result
      num_bytes += len;
    }

    break;
  case PWASM_IMPORT_TYPE_GLOBAL:
    {
      // parse global, check for error
      const size_t len = pwasm_old_parse_global_type(&(tmp.global), cbs, data_ptr, data_len, cb_data);
      if (!len) {
        return false;
      }

      // add length to result
      num_bytes += len;
    }

    break;
  default:
    FAIL("bad import descriptor");
  }

  if (dst_import) {
    // save result to destination
    *dst_import = tmp;
  }

  // return number of bytes consumed
  return num_bytes;
}

DEF_VEC_PARSE_FN(
  pwasm_parse_imports,
  "parse imports",
  pwasm_import_t,
  pwasm_parse_module_cbs_t,
  pwasm_parse_import,
  on_imports
);

static bool
pwasm_parse_import_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_parse_imports(src, cbs, cb_data) > 0;
}

static inline size_t
pwasm_function_section_parse_fn(
  uint32_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // parse index, check for error
  const size_t len = pwasm_u32_decode(dst, (pwasm_buf_t) { src, src_len });
  if (!len) {
    FAIL("invalid function index");
  }

  // return number of bytes consumed
  return len;
}

DEF_VEC_PARSE_FN(
  pwasm_parse_functions,
  "parse tables",
  uint32_t,
  pwasm_parse_module_cbs_t,
  pwasm_function_section_parse_fn,
  on_functions
);

static bool
pwasm_parse_func_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_parse_functions(src, cbs, cb_data) > 0;
}

DEF_VEC_PARSE_FN(
  pwasm_parse_tables,
  "parse tables",
  pwasm_table_t,
  pwasm_parse_module_cbs_t,
  pwasm_old_parse_table,
  on_tables
);

static bool
pwasm_parse_table_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_parse_tables(src, cbs, cb_data) > 0;
}

DEF_VEC_PARSE_FN(
  pwasm_parse_memories,
  "parse memories",
  pwasm_limits_t,
  pwasm_parse_module_cbs_t,
  pwasm_old_parse_limits,
  on_memories
);

static bool
pwasm_parse_mem_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_parse_memories(src, cbs, cb_data) > 0;
}

DEF_VEC_PARSE_FN(
  pwasm_parse_globals,
  "parse globals",
  pwasm_global_t,
  pwasm_parse_module_cbs_t,
  pwasm_old_parse_global,
  on_globals
);

static bool
pwasm_parse_global_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_parse_globals(src, cbs, cb_data) > 0;
}

typedef struct {
  pwasm_slice_t (*on_bytes)(const uint8_t *, size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_parse_export_cbs_t;

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

    // advance
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  // get name slice
  pwasm_slice_t name = cbs->on_bytes(name_buf.ptr, name_buf.len, cb_data);
  if (!name.len) {
    return 0;
  }

  // get export type, check for error
  const pwasm_export_type_t type = curr.ptr[0];
  if (!pwasm_is_valid_export_type(type)) {
    cbs->on_error("bad export type", cb_data);
  }

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

  *dst = (pwasm_new_export_t) {
    .name = name,
    .type = type,
    .id   = id,
  };

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
pwasm_old_parse_export(
  pwasm_export_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  const pwasm_buf_t name_buf = {
    .ptr = src,
    .len = src_len,
  };

  // parse export name, check for error
  pwasm_buf_t name;
  const size_t n_len = pwasm_parse_name(&name, cbs, name_buf, cb_data);
  if (!n_len) {
    return 0;
  }

  // check src length
  if (n_len + 2 > src_len) {
    FAIL("truncated export");
  }

  // get export type, check for error
  const pwasm_export_type_t type = src[n_len];
  if (!pwasm_is_valid_export_type(type)) {
    FAIL("bad export type");
  }

  // parse id, check for error
  uint32_t id;
  const size_t id_len = pwasm_u32_decode(&id, pwasm_buf_step(name_buf, 1 + n_len));
  if (!id_len) {
    FAIL("bad export index");
  }

  // build result
  const pwasm_export_t tmp = {
    .name = name,
    .type = type,
    .id   = id
  };

  if (dst) {
    // copy to destination
    *dst = tmp;
  }

  // return number of bytes consumed
  return n_len + 1 + id_len;
}

DEF_VEC_PARSE_FN(
  pwasm_old_parse_exports,
  "parse exports",
  pwasm_export_t,
  pwasm_parse_module_cbs_t,
  pwasm_old_parse_export,
  on_exports
);

static bool
pwasm_parse_export_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_old_parse_exports(src, cbs, cb_data) > 0;
}

static bool
pwasm_parse_start_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  // check source length
  if (!src.len) {
    FAIL("empty start section");
  }

  // get id, check for error
  uint32_t id = 0;
  const size_t len = pwasm_u32_decode(&id, src);
  if (!len) {
    FAIL("bad start section function index");
  }

  if (cbs && cbs->on_start) {
    // send callback
    cbs->on_start(id, cb_data);
  }

  // return success
  return true;
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

/**
 * Parse element into +dst+ from source buffer +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_old_parse_element(
  pwasm_element_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  const pwasm_buf_t src_buf = { src, src_len };

  // get table_id, check for error
  uint32_t t_id = 0;
  const size_t t_len = pwasm_u32_decode(&t_id, src_buf);
  if (!t_len) {
    FAIL("bad element table id");
  }

  // parse expr, check for error
  pwasm_expr_t expr;
  const size_t expr_len = pwasm_old_parse_const_expr(&expr, cbs, src + t_len, src_len - t_len, cb_data);
  if (!expr_len) {
    return 0;
  }

  // build offset
  size_t ofs = t_len + expr_len;

  // get function index count, check for error
  uint32_t num_fns;
  const size_t n_len = pwasm_u32_decode(&num_fns, pwasm_buf_step(src_buf, ofs));
  if (!n_len) {
    FAIL("bad element function index count");
  }

  // increment offset
  ofs += n_len;

  size_t data_len = 0;
  for (size_t i = 0; i < num_fns; i++) {
    uint32_t id = 0;
    const size_t len = pwasm_u32_decode(&id, pwasm_buf_step(src_buf, ofs));
    if (!len) {
      FAIL("bad element function index");
    }

    // increment offset
    data_len += len;
  }

  // build result
  const pwasm_element_t tmp = {
    .table_id = t_id,
    .expr = expr,
    .num_func_ids = num_fns,
    .func_ids = {
      .ptr = src + ofs,
      .len = data_len,
    },
  };

  if (dst) {
    // copy result to destination
    *dst = tmp;
  }

  // return number of bytes consumed
  return ofs + data_len;
}

DEF_VEC_PARSE_FN(
  pwasm_old_parse_elements,
  "parse elements",
  pwasm_element_t,
  pwasm_parse_module_cbs_t,
  pwasm_old_parse_element,
  on_elements
);

static bool
pwasm_parse_elem_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_old_parse_elements(src, cbs, cb_data) > 0;
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


/**
 * Parse function code into +dst+ from source buffer +src+, consuming a
 * maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pwasm_old_parse_fn_code(
  pwasm_buf_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check length
  if (!src_len) {
    FAIL("empty code section entry");
  }

  // get size, check for error
  uint32_t size = 0;
  const size_t size_len = pwasm_u32_decode(&size, (pwasm_buf_t) { src, src_len });
  if (!size_len) {
    FAIL("bad code size");
  }

  // check size
  if (size > src_len - size_len) {
    FAIL("truncated code");
  }

  // populate result
  const pwasm_buf_t tmp = {
    .ptr = src + size_len,
    .len = size,
  };

  if (dst) {
    // copy result to destination
    *dst = tmp;
  }

  // return number of bytes consumed
  return size_len + size;
}

DEF_VEC_PARSE_FN(
  pwasm_old_parse_codes,
  "parse function codes",
  pwasm_buf_t,
  pwasm_parse_module_cbs_t,
  pwasm_old_parse_fn_code,
  on_function_codes
);

static bool
pwasm_parse_code_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_old_parse_codes(src, cbs, cb_data) > 0;
}

static size_t
pwasm_parse_data_segment(
  pwasm_data_segment_t * const dst,
  const pwasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  const pwasm_buf_t src_buf = { src, src_len };

  // get memory index, check for error
  uint32_t id = 0;
  const size_t id_len = pwasm_u32_decode(&id, src_buf);
  if (!id_len) {
    FAIL("bad data section memory index");
  }

  // parse expr, check for error
  pwasm_expr_t expr;
  const size_t expr_len = pwasm_old_parse_const_expr(&expr, cbs, src + id_len, src_len - id_len, cb_data);
  if (!expr_len) {
    return 0;
  }

  const size_t data_ofs = id_len + expr_len;
  if (data_ofs >= src_len) {
    FAIL("missing data section data");
  }

  // get size, check for error
  uint32_t size = 0;
  const size_t size_len = pwasm_u32_decode(&size, pwasm_buf_step(src_buf, data_ofs));
  if (!size_len) {
    FAIL("bad data section data size");
  }

  // build result
  pwasm_data_segment_t tmp = {
    .mem_id = id,
    .expr = expr,
    .data = {
      .ptr = src + data_ofs + size_len,
      .len = size,
    },
  };

  if (dst) {
    // copy result to destination
    *dst = tmp;
  }

  // return number of bytes consumed
  return data_ofs + size_len + size;
}

DEF_VEC_PARSE_FN(
  pwasm_parse_data_segments,
  "parse data segments",
  pwasm_data_segment_t,
  pwasm_parse_module_cbs_t,
  pwasm_parse_data_segment,
  on_data_segments
);

static bool
pwasm_parse_segment_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  return pwasm_parse_data_segments(src, cbs, cb_data) > 0;
}

static bool
pwasm_parse_invalid_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
) {
  (void) src;
  FAIL("unknown section type");
}

typedef bool (*pwasm_parse_section_fn_t)(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_buf_t src,
  void * const cb_data
);

#define PWASM_SECTION_TYPE(a, b) pwasm_parse_ ## b ## _section,
static const pwasm_parse_section_fn_t
PWASM_SECTION_PARSERS[] = {
PWASM_SECTION_TYPES
};
#undef PWASM_SECTION_TYPE

static bool
pwasm_parse_section(
  const pwasm_parse_module_cbs_t * const cbs,
  const pwasm_section_type_t sec_type,
  const pwasm_buf_t src,
  void * const cb_data
) {
  const size_t ofs = MIN(sec_type, PWASM_SECTION_TYPE_LAST);
  return PWASM_SECTION_PARSERS[ofs](cbs, src, cb_data);
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

#if 0
typedef struct {
  void (*on_error)(const char *, void *);
} pwasm_scan_cbs_t;

typedef struct {
  const pwasm_scan_cbs_t *cbs;
  void *cb_data;
} pwasm_scan_ctx_t;

static inline size_t
pwasm_scan_u32s(
  const pwasm_scan_ctx_t * const ctx
  uint32_t * const ret,
  const pwasm_buf_t src
) {
  // get size, check for error
  uint32_t size = 0;
  const size_t size_len = pwasm_u32_decode(&size, src);
  if (!size_len) {
    FAIL("bad u32 vector size");
  }

  size_t num_bytes = size_len;
  pwasm_buf_t curr = pwasm_buf_step(src, size_len);
  uint32_t left = size;
  while (left > 0) {
    if (!curr.len) {
      FAIL("incomplete u32 vector");
    }

    // get value, check for error
    const size_t len = pwasm_u32_scan(curr);
    if (!val_len) {
      FAIL("bad u32 vector value");
    }

    // advance buffer, increment byte count, decriment item count
    curr = pwasm_buf_step(curr, val_len);
    num_bytes += val_len;
    left--;
  }

  if (ret) {
    // copy number of items to destination
    *ret = size;
  }

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_name(
  const pwasm_buf_t src
) {
  // get size, check for error
  uint32_t size = 0;
  const size_t size_len = pwasm_u32_decode(&size, src);
  if (!size_len) {
    return 0;
  }

  // return total number of bytes consumed
  return size_len + size;
}

typedef struct {
} pwasm_mod_stats_t;

static inline size_t
pwasm_scan_type(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  if (!src.len < 3) {
    FAIL("incomplete type");
  }

  if (src.ptr[0] != 0x60) {
    FAIL("bad function type");
  }

  // scan parameters
  pwasm_buf_t curr = pwasm_buf_step(src, 1);
  uint32_t num_params = 0;
  const size_t params_len = pwasm_scan_u32s(ctx, &num_params, curr);
  if (!params_len) {
    FAIL("bad function type parameters");
  }

  // scan results
  curr = pwasm_buf_step(curr, params_len);
  uint32_t num_results = 0;
  const size_t results_len = pwasm_scan_u32s(ctx, &num_results, curr);
  if (!results_len) {
    FAIL("bad function type results");
  }

  // increment counts
  dst->num_params += num_params;
  dst->num_results += num_results;
  dst->num_types++;

  // return number of bytes consumed
  return 1 + params_len + results_len;
}

static inline size_t
pwasm_scan_limits(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  size_t num_bytes = 0;

  if (src.len < 2) {
    FAIL("empty limits");
  }

  const uint8_t max_flag = src.ptr[0];
  pwasm_buf_t curr = pwasm_buf_step(src, 1);
  num_bytes += 1;

  if (max_flag > 1) {
    FAIL("bad limits flag");
  }

  // scan min, check for error
  const size_t min_len = pwasm_u32_scan(curr);
  if (!min_len) {
    FAIL("bad limits min");
  }
  curr = pwasm_buf_step(curr, min_len);
  num_bytes += min_len;


  if (max_flag) {
    // scan max, check for error
    const size_t max_len = pwasm_u32_scan(curr);
    if (!min_len) {
      FAIL("bad limits min");
    }
    num_bytes += max_len;
  }

  dst->num_limits++;

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_table_type(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  if (!src.len) {
    FAIL("missing element type");
  }

  if (src.ptr[0] != 0x70) {
    FAIL("bad table element type");
  }

  return 1 + pwasm_scan_limits(ctx, dst, pwasm_buf_step(src, 1));
}

static inline size_t
pwasm_scan_global_type(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  if (src.len < 2) {
    FAIL("incomplete global type");
  }

  return 2;
}

static inline size_t
pwasm_scan_global(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  if (src.len < 2) {
    FAIL("incomplete global type");
  }

  return 2;
}

static inline size_t
pwasm_scan_function_import(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  (void) dst;
  (void) ctx;
  return pwasm_u32_scan(src);
}

static inline size_t
pwasm_scan_table_import(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  return pwasm_scan_table_type(ctx, dst, src);
}

static inline size_t
pwasm_scan_memory_import(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  return pwasm_scan_limits(ctx, dst, src);
}

static inline size_t
pwasm_scan_global_import(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  return pwasm_scan_global_type(ctx, dst, src);
}

static inline size_t
pwasm_scan_import(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  pwasm_buf_t curr = pwasm_buf_step(src, 0);
  size_t num_bytes = 0;

  if (!src.len < 4) {
    FAIL("incomplete import");
  }

  // get module name, check for error
  const size_t mod_len = pwasm_scan_name(curr);
  if (!mod_len) {
    FAIL("bad import module name");
  }
  curr = pwasm_buf_step(curr, mod_len);
  num_bytes += mod_len;

  // get import name, check for error
  const size_t name_len = pwasm_scan_name(name_buf);
  if (!name_len) {
    FAIL("bad import module name");
  }
  curr = pwasm_buf_step(curr, name_len);
  num_bytes += name_len;

  // get type, check for error
  if (!curr.len) {
    FAIL("missing import type");
  }
  const uint8_t type = curr.ptr[0];
  curr = pwasm_buf_step(curr, 1);
  num_bytes++;

  switch (type) {
#define PWASM_IMPORT_TYPE(a, b, c) \
  case PWASM_IMPORT_TYPE ## a: \
    { \
      const size_t len = pwasm_scan_ ## c ## _import(ctx, dst, curr); \
      if (!len) { \
        return 0; \
      } \
      num_bytes += len; \
    } \
    break;
  default:
    FAIL("invalid import type");
  }

  // increment counts
  dst->num_import_types[type]++;
  dst->num_imports++;

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_custom_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  (void) ctx;
  (void) dst;
  return src.len;
}

static inline size_t
pwasm_scan_type_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  // get count, check for error
  uint32_t count = 0;
  size_t num_bytes = pwasm_u32_decode(&count, src);
  if (!num_bytes) {
    FAIL("bad vector size");
  }

  uint32_t left = count;
  pwasm_buf_t curr = pwasm_buf_step(src, num_bytes);
  while (left > 0) {
    // scan item, check for error
    const size_t len = pwasm_scan_type(ctx, dst, curr);
    if (!len) {
      return 0;
    }

    // advance buffer
    num_bytes += len;
    curr = pwasm_buf_step(curr, len);
  }

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_import_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  // get count, check for error
  uint32_t count = 0;
  size_t num_bytes = pwasm_u32_decode(&count, src);
  if (!num_bytes) {
    FAIL("bad vector size");
  }

  uint32_t left = count;
  pwasm_buf_t curr = pwasm_buf_step(src, num_bytes);
  while (left > 0) {
    // scan item, check for error
    const size_t len = pwasm_scan_import(ctx, dst, curr);
    if (!len) {
      return 0;
    }

    // advance buffer
    num_bytes += len;
    curr = pwasm_buf_step(curr, len);
  }

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_function_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  uint32_t num = 0;

  const size_t len = pwasm_scan_u32s(ctx, &num, curr);
  if (!len) {
    return 0;
  }

  // FIXME: += or =?
  dst->num_funcs = num;

  // return number of bytes consumed
  return len;
}

static inline size_t
pwasm_scan_table_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  // get count, check for error
  uint32_t count = 0;
  size_t num_bytes = pwasm_u32_decode(&count, src);
  if (!num_bytes) {
    FAIL("bad vector size");
  }

  uint32_t left = count;
  pwasm_buf_t curr = pwasm_buf_step(src, num_bytes);
  while (left > 0) {
    // scan item, check for error
    const size_t len = pwasm_scan_table_type(ctx, dst, curr);
    if (!len) {
      return 0;
    }

    // advance buffer
    num_bytes += len;
    curr = pwasm_buf_step(curr, len);
  }

  // FIXME: += or =?
  dst->num_tables = count;

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_memory_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  // get count, check for error
  uint32_t count = 0;
  size_t num_bytes = pwasm_u32_decode(&count, src);
  if (!num_bytes) {
    FAIL("bad vector size");
  }

  uint32_t left = count;
  pwasm_buf_t curr = pwasm_buf_step(src, num_bytes);
  while (left > 0) {
    // scan item, check for error
    const size_t len = pwasm_scan_limits(ctx, dst, curr);
    if (!len) {
      return 0;
    }

    // advance buffer
    num_bytes += len;
    curr = pwasm_buf_step(curr, len);
  }

  // FIXME: += or =?
  dst->num_mems = count;

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_global_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  // get count, check for error
  uint32_t count = 0;
  size_t num_bytes = pwasm_u32_decode(&count, src);
  if (!num_bytes) {
    FAIL("bad vector size");
  }

  uint32_t left = count;
  pwasm_buf_t curr = pwasm_buf_step(src, num_bytes);
  while (left > 0) {
    // scan item, check for error
    const size_t len = pwasm_scan_global(ctx, dst, curr);
    if (!len) {
      return 0;
    }

    // advance buffer
    num_bytes += len;
    curr = pwasm_buf_step(curr, len);
  }

  // FIXME: += or =?
  dst->num_globals = count;

  // return number of bytes consumed
  return num_bytes;
}

static inline size_t
pwasm_scan_section(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_section_type_t type,
  const pwasm_buf_t src
) {
  switch (type) {
#define PWASM_SECTION_TYPE(a, b) \
  case PWASM_SECTION_TYPE ## a: \
    return pwasm_ scan_ ## b ## _section(dst, src);
PWASM_SECTION_TYPES
#undef PWASM_SECTION_TYPE
  default:
    return pwasm_scan_invalid_section(ctx, dst, src);
  }
}

static bool
pwasm_mod_scan(
  const pwasm_scan_ctx_t * const ctx,
  pwasm_mod_stats_t * const dst,
  const pwasm_buf_t src
) {
  // cache callbacks locally
  // FIXME: replace FAIL
  const pwasm_scan_cbs_t cbs = {
    .on_error = (ctx->cbs && ctx->cbs->on_error) ? ctx->cbs->on_error : pwasm_scan_on_error,
  };
  // check source length
  if (src.len < 8) {
    FAIL("source too small");
  }

  // check magic and version
  if (memcmp(src, PWASM_HEADER, sizeof(PWASM_HEADER))) {
    FAIL("invalid module header");
  }

  pwasm_mod_stats_t stats = {};
  pwasm_section_type_t max_type = 0;

  pwasm_buf_t curr = pwasm_buf_step(src, 8);
  while (curr.len > 0) {
    // get section header, check for error
    pwasm_header_t head;
    const size_t hd_len = pwasm_header_parse(&head, curr);
    if (!head_len) {
      FAIL("invalid section header");
    }

    // check section type
    if (head.type >= PWASM_SECTION_TYPE_LAST) {
      FAIL("invalid section type");
    }

    // check section order for non-custom sections
    if (head.type != PWASM_SECTION_TYPE_CUSTOM) {
      if (head.type < max_type) {
        FAIL("invalid section order");
      } else if (head.type == max_type) {
        FAIL("duplicate section");
      }

      // update maximum section type
      max_type = head.type;
    }

    if (head.len > 0) {
      // build body buffer
      const pwasm_buf_t body = { curr.ptr + head_len, head.len };

      const size_t bd_len = pwasm_scan_section(ctx, &stats, head.type, body);
      if (!bd_len) {
        // return failure
        return 0;
      }

      if (head.type > 0) {
        // save reference to section body
        stats.section_bodies[head.type - 1] = body;
      }
    }

    // increment section counters
    stats.num_section_types[type]++;
    stats.num_sections++;

    // advance buffer
    curr = pwasm_buf_step(curr, bd_len);
  }

  if (dst) {
    // copy results to destination
    *dst = stats;
  }

  // return result
  return src.len;
}
#endif /* 0 */

bool
pwasm_parse_module(
  const void * const src_ptr,
  const size_t src_len,
  const pwasm_parse_module_cbs_t * const cbs,
  void * const cb_data
) {
  const uint8_t * const src = src_ptr;

  // check length
  if (src_len < 8) {
    FAIL("module too small");
  }

  // fprintf(stderr," sizeof(WASM_HEADER) = %zu\n", sizeof(WASM_HEADER));
  // check magic and version
  if (memcmp(src, PWASM_HEADER, sizeof(PWASM_HEADER))) {
    FAIL("invalid module header");
  }

  // bitfield to catch duplicate sections
  uint64_t seen = 0;

  for (size_t ofs = 8; ofs < src_len;) {
    // parse section type, check for error
    const pwasm_section_type_t sec_type = src[ofs];
    if (sec_type >= PWASM_SECTION_TYPE_LAST) {
      // FIXME: show section type?
      FAIL("invalid section type");
    }

    if (sec_type != PWASM_SECTION_TYPE_CUSTOM) {
      // build section mask
      const uint64_t mask = (1 << (sec_type - 1));

      // check for duplicate sections
      if (seen & mask) {
        FAIL("duplicate section");
      }

      // set section mask
      seen |= mask;
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
    const size_t len_ofs = pwasm_u32_decode(&data_len, (pwasm_buf_t) { u32_ptr, u32_len });
    if (!len_ofs) {
      FAIL("invalid section length");
    }

    // check total length to make sure it doesn't exceed size
    if (ofs + 1 + len_ofs + data_len > src_len) {
      FAIL("truncated section");
    }

    // build section data pointer
    const uint8_t * const data_ptr = src + ofs + 1 + len_ofs;

    // build section buffer
    const pwasm_buf_t data = {
      .ptr = data_ptr,
      .len = data_len,
    };

    // parse section, check for error
    if (!pwasm_parse_section(cbs, sec_type, data, cb_data)) {
      // return failure
      return false;
    }

    // increment source offset
    ofs += 1 + len_ofs + data_len;
  }

  // return success
  return true;
}

/**
 * Parse expr in buffer +src+ into sequence of instructions.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
size_t
pwasm_old_parse_expr(
  const pwasm_buf_t src,
  const pwasm_old_parse_expr_cbs_t * const cbs,
  void * const cb_data
) {
  // check source length
  if (src.len < 1) {
    FAIL("invalid expr");
  }

  // build instruction parser context
  const pwasm_old_parse_inst_ctx_t in_ctx = {
    .on_error = cbs->on_error,
    .cb_data  = cb_data,
  };

  pwasm_inst_t ins[PWASM_BATCH_SIZE];

  size_t depth = 1;
  size_t ofs = 0;
  size_t num_ins = 0;
  while ((depth > 0) && (ofs < src.len)) {
    // build output offset
    const size_t ins_ofs = (num_ins & (PWASM_BATCH_SIZE - 1));

    const pwasm_buf_t in_src = {
      .ptr = src.ptr + ofs,
      .len = src.len - ofs,
    };

    // parse instruction, check for error
    pwasm_inst_t * const dst = ins + ins_ofs;
    const size_t len = pwasm_old_parse_inst(dst, in_ctx, in_src);
    if (!len) {
      return 0;
    }

    if ((ins_ofs == (LEN(ins) - 1)) && cbs && cbs->on_insts) {
      cbs->on_insts(ins, ins_ofs, cb_data);
    }

    // update depth
    depth += pwasm_op_is_control(dst->op) ? 1 : 0;
    depth -= (dst->op == PWASM_OP_END) ? 1 : 0;

    // increment offset
    ofs += len;

    // increment instruction count
    num_ins++;
  }

  // check for depth error
  if (depth > 0) {
    FAIL("unterminated expression");
  }

  // count remaining entries
  const size_t num_left = num_ins & (PWASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_insts) {
    // flush remaining entries
    cbs->on_insts(ins, num_left, cb_data);
  }

  // return number of bytes consumed
  return ofs;
}

typedef struct {
  bool success;
  size_t num;
} pwasm_get_expr_size_t;

static void
pwasm_get_expr_size_on_insts(
  const pwasm_inst_t * const insts,
  const size_t num,
  void *cb_data
) {
  pwasm_get_expr_size_t * const data = cb_data;
  (void) insts;
  data->num += num;
}

static void
pwasm_get_expr_size_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_get_expr_size_t * const data = cb_data;
  (void) text;
  data->success = false;
}

static const pwasm_old_parse_expr_cbs_t
PWASM_GET_EXPR_SIZE_CBS = {
  .on_insts = pwasm_get_expr_size_on_insts,
  .on_error = pwasm_get_expr_size_on_error,
};

bool
pwasm_get_expr_size(
  const pwasm_buf_t src,
  size_t * const ret_size
) {
  pwasm_get_expr_size_t data = {
    .success = true,
    .num = 0,
  };

  // count instructions, ignore result (number of bytes)
  (void) pwasm_old_parse_expr(src, &PWASM_GET_EXPR_SIZE_CBS, &data);

  if (data.success && ret_size) {
    // copy size to output
    *ret_size = data.num;
  }

  // return result
  return data.success;
}

static size_t
pwasm_parse_local(
  pwasm_local_t * const dst,
  const pwasm_buf_t src,
  const pwasm_parse_function_cbs_t * const cbs,
  void * const cb_data
) {
  if (src.len < 2) {
    FAIL("empty local");
  }

  // parse local count, check for error
  uint32_t num;
  const size_t n_len = pwasm_u32_decode(&num, src);
  if (!n_len) {
    FAIL("bad local count");
  }

  if (n_len >= src.len) {
    FAIL("missing local type");
  }

  // get type, check for error
  const pwasm_value_type_t type = src.ptr[n_len];
  if (!pwasm_is_valid_value_type(type)) {
    FAIL("bad local type");
  }

  // populate result
  const pwasm_local_t tmp = {
    .num  = num,
    .type = type,
  };

  if (dst) {
    // copy result to destination
    *dst = tmp;
  }

  // return number of bytes consumed
  return n_len + 1;
}

static inline size_t
pwasm_parse_function_locals(
  const pwasm_buf_t src,
  const pwasm_parse_function_cbs_t * const cbs,
  void * const cb_data
) {
  // get number of locals, check for error
  uint32_t num_locals = 0;
  const size_t num_len = pwasm_u32_decode(&num_locals, src);
  if (!num_len) {
    FAIL("bad locals count");
  }

  pwasm_local_t ls[PWASM_BATCH_SIZE];

  size_t ofs = num_len;
  for (size_t i = 0; i < num_locals; i++) {
    const size_t ls_ofs = (i & (PWASM_BATCH_SIZE - 1));

    // build temporary buffer
    const pwasm_buf_t tmp = {
      .ptr = src.ptr + ofs,
      .len = src.len - ofs,
    };

    // parse local, check for error
    const size_t len = pwasm_parse_local(ls + ls_ofs, tmp, cbs, cb_data);
    if (!len) {
      return 0;
    }

    // increment offset, check for error
    ofs += len;
    if (ofs > src.len) {
      FAIL("function section length overflow");
    }

    if ((ls_ofs == (LEN(ls) - 1)) && cbs && cbs->on_locals) {
      // flush batch
      cbs->on_locals(ls, ls_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_locals & (PWASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_locals) {
    // flush remaining locals
    cbs->on_locals(ls, num_left, cb_data);
  }

  // return success
  return true;
}

/**
 * Parse expr in buffer +src+ into sequence of instructions.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static inline size_t
pwasm_parse_function_expr(
  const pwasm_buf_t src,
  const pwasm_parse_function_cbs_t * const fn_cbs,
  void * const cb_data
) {
  const pwasm_old_parse_expr_cbs_t expr_cbs = {
    .on_insts = fn_cbs ? fn_cbs->on_insts : NULL,
    .on_error = fn_cbs ? fn_cbs->on_error : NULL,
  };

  return pwasm_old_parse_expr(src, &expr_cbs, cb_data);
}

bool
pwasm_parse_function(
  const pwasm_buf_t src,
  const pwasm_parse_function_cbs_t * const cbs,
  void * const cb_data
) {
  // parse locals, check for error
  const size_t ls_len = pwasm_parse_function_locals(src, cbs, cb_data);
  if (!ls_len) {
    return false;
  }

  // build temp expr source
  const pwasm_buf_t expr_src = {
    .ptr = src.ptr + ls_len,
    .len = src.len - ls_len,
  };

  // parse expression, check for error
  const size_t expr_len = pwasm_parse_function_expr(expr_src, cbs, cb_data);
  if (!expr_len) {
    return false;
  }

  // FIXME: warn about length here?

  // return success
  return true;
}

typedef struct {
  pwasm_function_sizes_t sizes;
  bool success;
} pwasm_get_function_sizes_t;

static void
pwasm_get_function_sizes_on_locals(
  const pwasm_local_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_get_function_sizes_t *data = cb_data;
  (void) rows;
  data->sizes.num_locals += num;
}

static void
pwasm_get_function_sizes_on_insts(
  const pwasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_get_function_sizes_t *data = cb_data;
  (void) rows;
  data->sizes.num_insts += num;

  for (size_t i = 0; i < num; i++) {
    if (rows[i].op == PWASM_OP_BR_TABLE) {
      const pwasm_buf_t buf = rows[i].v_br_table.labels.buf;
      data->sizes.num_labels += pwasm_count_labels(buf);
    }
  }
}

static void
pwasm_get_function_sizes_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_get_function_sizes_t *data = cb_data;
  (void) text;
  data->success = false;
}

static const pwasm_parse_function_cbs_t
PWASM_GET_FUNCTION_SIZES_CBS = {
  .on_locals = pwasm_get_function_sizes_on_locals,
  .on_insts  = pwasm_get_function_sizes_on_insts,
  .on_error  = pwasm_get_function_sizes_on_error,
};

bool
pwasm_get_function_sizes(
  const pwasm_buf_t src,
  pwasm_function_sizes_t * const ret_sizes
) {
  // build callback data
  pwasm_get_function_sizes_t data = {
    .sizes = { 0, 0 },
    .success = true,
  };

  // count sizes
  (void) pwasm_parse_function(src, &PWASM_GET_FUNCTION_SIZES_CBS, &data);

  if (data.success && ret_sizes) {
    // copy sizes to result
    *ret_sizes = data.sizes;
  }

  // return result
  return data.success;
}

typedef struct {
  pwasm_module_sizes_t sizes;
  const pwasm_get_module_sizes_cbs_t *cbs;
  void *cb_data;
  bool success;
} pwasm_get_module_sizes_t;

static void
pwasm_get_module_sizes_on_custom_section(
  const pwasm_custom_section_t * const ptr,
  void *cb_data
) {
  pwasm_get_module_sizes_t *data = cb_data;
  (void) ptr;
  data->sizes.num_custom_sections++;
}

static void
pwasm_get_module_sizes_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_get_module_sizes_t *data = cb_data;
  data->success = false;
  if (data->cbs && data->cbs->on_error) {
    data->cbs->on_error(text, data->cb_data);
  }
}

static void
pwasm_get_module_sizes_on_function_types(
  const pwasm_function_type_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_function_types += num;

  for (size_t i = 0; i < num; i++) {
    data->sizes.num_function_params += rows[i].params.len;
    data->sizes.num_function_results += rows[i].results.len;
  }
}

static void pwasm_get_module_sizes_on_imports(
  const pwasm_import_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_imports += num;

  // get number of imports by type
  for (size_t i = 0; i < num; i++) {
    data->sizes.num_import_types[rows[i].type]++;
  }
}

static void
pwasm_get_module_sizes_on_globals(
  const pwasm_global_t * const globals,
  const size_t num,
  void * const cb_data
) {
  pwasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_globals += num;

  for (size_t i = 0; i < num; i++) {
    size_t num_insts = 0;
    if (!pwasm_get_expr_size(globals[i].expr.buf, &num_insts)) {
      pwasm_get_module_sizes_on_error("get global expr size failed", data);
      return;
    }

    data->sizes.num_global_insts += num_insts;
    data->sizes.num_insts += num_insts;
  }
}

static void
pwasm_get_module_sizes_on_function_codes(
  const pwasm_buf_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_function_codes += num;
  data->sizes.num_functions += num;

  for (size_t i = 0; i < num; i++) {
    pwasm_function_sizes_t sizes;
    if (!pwasm_get_function_sizes(rows[i], &sizes)) {
      pwasm_get_module_sizes_on_error("get function size failed", data);
      return;
    }

    data->sizes.num_labels += sizes.num_labels;
    data->sizes.num_locals += sizes.num_locals;
    data->sizes.num_function_insts += sizes.num_insts;
    data->sizes.num_insts += sizes.num_insts;
  }
}

static void
pwasm_get_module_sizes_on_elements(
  const pwasm_element_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_elements += num;

  for (size_t i = 0; i < num; i++) {
    data->sizes.num_element_func_ids += rows[i].num_func_ids;

    // count number of instructions in expression
    size_t num_insts = 0;
    if (!pwasm_get_expr_size(rows[i].expr.buf, &num_insts)) {
      pwasm_get_module_sizes_on_error("get element expr size failed", data);
      return;
    }

    data->sizes.num_element_insts += num_insts;
    data->sizes.num_insts += num_insts;
  }
}

static void
pwasm_get_module_sizes_on_data_segments(
  const pwasm_data_segment_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_data_segments += num;

  for (size_t i = 0; i < num; i++) {
    size_t num_insts = 0;
    if (!pwasm_get_expr_size(rows[i].expr.buf, &num_insts)) {
      pwasm_get_module_sizes_on_error("get data segment expr size failed", data);
      return;
    }

    data->sizes.num_data_segment_insts += num_insts;
    data->sizes.num_insts += num_insts;
  }
}

#define GET_MOD_SIZES_FNS \
  CUSTOM_SIZE_FN(on_custom_section) \
  CUSTOM_SIZE_FN(on_function_types) \
  CUSTOM_SIZE_FN(on_imports) \
  SIZE_FN(on_functions, uint32_t, num_functions) \
  SIZE_FN(on_tables, pwasm_table_t, num_tables) \
  SIZE_FN(on_memories, pwasm_limits_t, num_memories) \
  CUSTOM_SIZE_FN(on_globals) \
  SIZE_FN(on_exports, pwasm_export_t, num_exports) \
  CUSTOM_SIZE_FN(on_elements) \
  CUSTOM_SIZE_FN(on_function_codes) \
  CUSTOM_SIZE_FN(on_data_segments)

#define SIZE_FN(fn_name, type, sum) \
  static void pwasm_get_module_sizes_ ## fn_name ( \
    const type * const ptr, \
    const size_t num, \
    void * const cb_data \
  ) { \
    pwasm_get_module_sizes_t * const data = cb_data; \
    (void) ptr; \
    data->sizes.sum += num; \
  }
#define CUSTOM_SIZE_FN(fn_name)
GET_MOD_SIZES_FNS
#undef SIZE_FN
#undef CUSTOM_SIZE_FN

static const pwasm_parse_module_cbs_t
PWASM_GET_MOD_SIZES_CBS = {
#define SIZE_FN(fn_name, type, sum) \
  .fn_name = pwasm_get_module_sizes_ ## fn_name,
#define CUSTOM_SIZE_FN(fn_name) \
  .fn_name = pwasm_get_module_sizes_ ## fn_name,
GET_MOD_SIZES_FNS
#undef CUSTOM_SIZE_FN
#undef SIZE_FN
  .on_error = pwasm_get_module_sizes_on_error,
};

_Bool pwasm_get_module_sizes(
  pwasm_module_sizes_t * const sizes,
  const void * const src,
  const size_t len,
  const pwasm_get_module_sizes_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_get_module_sizes_t data = {
    .sizes = {
      .src = {
        .ptr = src,
        .len = len,
      },
    },

    .success  = true,
    .cbs      = cbs,
    .cb_data  = cb_data,
  };

  // parse module, check for error
  if (!pwasm_parse_module(src, len, &PWASM_GET_MOD_SIZES_CBS, &data)) {
    // return failure
    return false;
  }

  // add import counts to result
  data.sizes.num_functions += data.sizes.num_import_types[PWASM_IMPORT_TYPE_FUNC];
  data.sizes.num_tables += data.sizes.num_import_types[PWASM_IMPORT_TYPE_TABLE];
  data.sizes.num_memories += data.sizes.num_import_types[PWASM_IMPORT_TYPE_MEM];
  data.sizes.num_globals += data.sizes.num_import_types[PWASM_IMPORT_TYPE_GLOBAL];

  if (!data.success) {
    // return failure
    return false;
  }

  if (sizes) {
    // copy to result
    *sizes = data.sizes;
  }

  // return success
  return true;
}

bool
pwasm_module_alloc(
  pwasm_module_t * const ret,
  const pwasm_module_sizes_t * const sizes,
  const pwasm_module_alloc_cbs_t * const cbs,
  void *cb_data
) {
  if (!cbs || !cbs->on_alloc) {
    // return failure
    return false;
  }

  // calculate total number of bytes needed
  const size_t num_bytes = (
    // custom section handles
    sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +

    // function types
    sizeof(pwasm_function_type_t) * sizes->num_function_types +

    // imports
    sizeof(pwasm_import_t) * sizes->num_imports +

    // function locals
    sizeof(pwasm_local_t) * sizes->num_locals +

    // instructions
    sizeof(pwasm_inst_t) * sizes->num_insts +

    // functions
    sizeof(pwasm_function_t) * sizes->num_functions +

    // tables
    sizeof(pwasm_table_t) * sizes->num_tables +

    // memories
    sizeof(pwasm_limits_t) * sizes->num_memories +

    // globals
    sizeof(pwasm_module_global_t) * sizes->num_globals +

    // exports
    sizeof(pwasm_export_t) * sizes->num_exports +

    // element function IDs
    sizeof(uint32_t) * sizes->num_element_func_ids +

    // elements
    sizeof(pwasm_module_element_t) * sizes->num_elements +

    // data segments
    sizeof(pwasm_module_data_segment_t) * sizes->num_data_segments +

    // labels in br_table instructions
    sizeof(uint32_t) * sizes->num_labels +

    // sentinel
    0
  );

  // allocate memory, check for error
  uint8_t * const mem = cbs->on_alloc(num_bytes, cb_data);
  if (!mem) {
    if (cbs->on_error) {
      cbs->on_error("alloc failed", cb_data);
    }

    // return failure
    return false;
  }

  // build result module
  pwasm_module_t mod = {
    // source module and sizes
    .src = sizes->src,
    .sizes = sizes,

    // memory buffer
    .mem = mem,

    // custom sections
    .custom_sections = (pwasm_custom_section_t*) mem,
    .num_custom_sections = sizes->num_custom_sections,

    // function types
    .function_types = (pwasm_function_type_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections
    )),
    .num_function_types = sizes->num_function_types,

    // imports
    .imports = (pwasm_import_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types
    )),
    .num_imports = sizes->num_imports,

    // locals
    .locals = (pwasm_local_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports
    )),
    .num_locals = sizes->num_locals,

    // instructions
    .insts = (pwasm_inst_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals
    )),
    .num_insts = sizes->num_insts,

    // functions
    .functions = (pwasm_function_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts
    )),
    .num_functions = sizes->num_functions,

    // tables
    .tables = (pwasm_table_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions
    )),
    .num_tables = sizes->num_tables,

    // memories
    .memories = (pwasm_limits_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions +
      sizeof(pwasm_table_t) * sizes->num_tables
    )),
    .num_memories = sizes->num_memories,

    // globals
    .globals = (pwasm_module_global_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions +
      sizeof(pwasm_table_t) * sizes->num_tables +
      sizeof(pwasm_limits_t) * sizes->num_memories
    )),
    .num_globals = sizes->num_globals,

    // exports
    .exports = (pwasm_export_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions +
      sizeof(pwasm_table_t) * sizes->num_tables +
      sizeof(pwasm_limits_t) * sizes->num_memories +
      sizeof(pwasm_module_global_t) * sizes->num_globals
    )),
    .num_exports = sizes->num_exports,

    // element function IDs
    .element_func_ids = (uint32_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions +
      sizeof(pwasm_table_t) * sizes->num_tables +
      sizeof(pwasm_limits_t) * sizes->num_memories +
      sizeof(pwasm_module_global_t) * sizes->num_globals +
      sizeof(pwasm_export_t) * sizes->num_exports
    )),
    .num_element_func_ids = sizes->num_element_func_ids,

    // elements
    .elements = (pwasm_module_element_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions +
      sizeof(pwasm_table_t) * sizes->num_tables +
      sizeof(pwasm_limits_t) * sizes->num_memories +
      sizeof(pwasm_module_global_t) * sizes->num_globals +
      sizeof(pwasm_export_t) * sizes->num_exports +
      sizeof(uint32_t) * sizes->num_element_func_ids
    )),
    .num_elements = sizes->num_elements,

    // data segments
    .data_segments = (pwasm_module_data_segment_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions +
      sizeof(pwasm_table_t) * sizes->num_tables +
      sizeof(pwasm_limits_t) * sizes->num_memories +
      sizeof(pwasm_module_global_t) * sizes->num_globals +
      sizeof(pwasm_export_t) * sizes->num_exports +
      sizeof(uint32_t) * sizes->num_element_func_ids +
      sizeof(pwasm_module_element_t) * sizes->num_elements
    )),
    .num_data_segments = sizes->num_data_segments,

    // br_table instruction labels
    .labels = (uint32_t*) (mem + (
      sizeof(pwasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pwasm_function_type_t) * sizes->num_function_types +
      sizeof(pwasm_import_t) * sizes->num_imports +
      sizeof(pwasm_local_t) * sizes->num_locals +
      sizeof(pwasm_inst_t) * sizes->num_insts +
      sizeof(pwasm_function_t) * sizes->num_functions +
      sizeof(pwasm_table_t) * sizes->num_tables +
      sizeof(pwasm_limits_t) * sizes->num_memories +
      sizeof(pwasm_module_global_t) * sizes->num_globals +
      sizeof(pwasm_export_t) * sizes->num_exports +
      sizeof(uint32_t) * sizes->num_element_func_ids +
      sizeof(pwasm_module_element_t) * sizes->num_elements +
      sizeof(pwasm_module_data_segment_t) * sizes->num_data_segments
    )),
    .num_labels = sizes->num_labels,
  };

  if (ret) {
    // copy module to output
    memcpy(ret, &mod, sizeof(pwasm_module_t));
  }

  // return success
  return true;
}

typedef struct {
  pwasm_module_t *mod;
  pwasm_module_sizes_t sizes;
  const pwasm_module_init_cbs_t *cbs;
  void *cb_data;
  bool success;
} pwasm_module_init_t;

static void
pwasm_module_init_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_module_init_t *data = cb_data;
  data->success = false;
  if (data->cbs && data->cbs->on_error) {
    data->cbs->on_error(text, data->cb_data);
  }
}

static void
pwasm_module_init_on_custom_section(
  const pwasm_custom_section_t * const ptr,
  void *cb_data
) {
  pwasm_module_init_t *data = cb_data;
  data->mod->custom_sections[data->sizes.num_custom_sections++] = *ptr;
}

static void
pwasm_module_init_on_function_types(
  const pwasm_function_type_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_function_type_t * const dst = data->mod->function_types + data->sizes.num_function_types;
  const size_t num_bytes = sizeof(pwasm_function_type_t) * num;

  memcpy(dst, rows, num_bytes);
  data->sizes.num_function_types += num;
}

static void
pwasm_module_init_on_imports(
  const pwasm_import_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pwasm_import_t * const dst = mod->imports + data->sizes.num_imports;
  const size_t num_bytes = sizeof(pwasm_import_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_imports += num;

  // populate imports
  for (size_t i = 0; i < num; i++) {
    switch (rows[i].type) {
    case PWASM_IMPORT_TYPE_FUNC:
      mod->functions[data->sizes.num_functions].source = PWASM_SOURCE_IMPORT;
      mod->functions[data->sizes.num_functions].type_id = rows[i].func.id;
      data->sizes.num_functions++;
      break;
    case PWASM_IMPORT_TYPE_TABLE:
      mod->tables[data->sizes.num_tables++] = rows[i].table;
      data->sizes.num_tables++;
      break;
    case PWASM_IMPORT_TYPE_MEM:
      mod->memories[data->sizes.num_memories++] = rows[i].mem.limits;
      data->sizes.num_memories++;
      break;
    case PWASM_IMPORT_TYPE_GLOBAL:
      mod->globals[data->sizes.num_globals].source = PWASM_SOURCE_IMPORT;
      mod->globals[data->sizes.num_globals].type = rows[i].global;
      data->sizes.num_globals++;
      break;
    default:
      // never reached
      pwasm_module_init_on_error("unknown import type", cb_data);
      return;
    }
  }
}

static void
pwasm_module_init_on_functions(
  const uint32_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;

  // populate internal function types
  for (size_t i = 0; i < num; i++) {
    const size_t ofs = data->mod->sizes->num_functions + i;
    data->mod->functions[ofs].source = PWASM_SOURCE_MODULE;
    data->mod->functions[ofs].type_id = rows[i];
  }

  // increment functions size
  data->sizes.num_functions += num;
}

static void
pwasm_module_init_on_tables(
  const pwasm_table_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pwasm_table_t * const dst = mod->tables + data->sizes.num_tables;
  const size_t num_bytes = sizeof(pwasm_table_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_tables += num;
}

static void
pwasm_module_init_on_memories(
  const pwasm_limits_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pwasm_limits_t * const dst = mod->memories + data->sizes.num_memories;
  const size_t num_bytes = sizeof(pwasm_limits_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_memories += num;
}

typedef struct {
  pwasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pwasm_module_init_on_globals_t;

static void
pwasm_module_init_on_globals_on_insts(
  const pwasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_module_init_on_globals_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  if (!data->success) {
    return;
  }

  // calculate destination and number of bytes
  pwasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pwasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  // update expr slice length
  init_data->mod->globals[data->ofs].expr.len += num;
}

static void
pwasm_module_init_on_globals_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_module_init_on_globals_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pwasm_module_init_on_error(text, init_data);
}

static const pwasm_old_parse_expr_cbs_t
PWASM_MODULE_INIT_ON_GLOBALS_CBS = {
  .on_insts = pwasm_module_init_on_globals_on_insts,
  .on_error = pwasm_module_init_on_globals_on_error,
};

static void
pwasm_module_init_on_globals(
  const pwasm_global_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_module_t * const mod = data->mod;

  for (size_t i = 0; i < num; i++) {
    const size_t ofs = data->mod->sizes->num_globals + i;
    mod->globals[ofs].source = PWASM_SOURCE_MODULE;
    mod->globals[ofs].type = rows[i].type;
    mod->globals[ofs].expr.ofs = data->sizes.num_insts;
    mod->globals[ofs].expr.len = 0;

    // build expr parsing callback data
    pwasm_module_init_on_globals_t expr_data = {
      .init_data  = data,
      .ofs        = ofs,
      .success    = true,
    };

    // parse expression
    // FIXME: should limit to constant expr here
    (void) pwasm_old_parse_expr(
      rows[i].expr.buf,
      &PWASM_MODULE_INIT_ON_GLOBALS_CBS,
      &expr_data
    );

    if (!expr_data.success) {
      // expression parsing failed, return
      return;
    }
  }

  // increment globals
  data->sizes.num_globals += num;
}

static void
pwasm_module_init_on_exports(
  const pwasm_export_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pwasm_export_t * const dst = mod->exports + data->sizes.num_exports;
  const size_t num_bytes = sizeof(pwasm_export_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_exports += num;
}

static void
pwasm_module_init_on_start(
  const uint32_t start,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  data->mod->has_start = true;
  data->mod->start = start;
}

typedef struct {
  pwasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pwasm_module_add_element_expr_t;

static void
pwasm_module_add_element_expr_on_insts(
  const pwasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_module_add_element_expr_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  if (!data->success) {
    return;
  }

  // calculate destination and number of bytes
  pwasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pwasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  // update expr slice length
  init_data->mod->elements[data->ofs].expr.len += num;
}

static void
pwasm_module_add_element_expr_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_module_add_element_expr_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pwasm_module_init_on_error(text, init_data);
}

static const pwasm_old_parse_expr_cbs_t
PWASM_MODULE_ADD_ELEMENT_EXPR_CBS = {
  .on_insts = pwasm_module_add_element_expr_on_insts,
  .on_error = pwasm_module_add_element_expr_on_error,
};

static inline bool
pwasm_module_add_element_expr(
  pwasm_module_t * const mod,
  pwasm_module_init_t * const init_data,
  const size_t ofs,
  const pwasm_buf_t src
) {
  // populate expression slice offset and length
  mod->elements[ofs].expr.ofs = init_data->sizes.num_insts;
  mod->elements[ofs].expr.len = 0;

  // build expr parsing callback data
  pwasm_module_add_element_expr_t expr_data = {
    .init_data  = init_data,
    .ofs        = ofs,
    .success    = true,
  };

  // parse expression
  // FIXME: should limit to constant expr here
  (void) pwasm_old_parse_expr(
    src,
    &PWASM_MODULE_ADD_ELEMENT_EXPR_CBS,
    &expr_data
  );

  return expr_data.success;
}

typedef struct {
  pwasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pwasm_module_add_element_fns_t;

static void
pwasm_module_add_element_fns_on_items(
  const uint32_t * const ids,
  const size_t num,
  void *cb_data
) {
  pwasm_module_add_element_fns_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  if (!data->success) {
    return;
  }

  // calculate destination and number of bytes
  uint32_t * const dst = init_data->mod->element_func_ids + init_data->sizes.num_element_func_ids;
  const size_t num_bytes = sizeof(uint32_t) * num;

  // copy data, increment offset
  memcpy(dst, ids, num_bytes);
  init_data->sizes.num_element_func_ids += num;

  // update function ids slice length
  init_data->mod->elements[data->ofs].func_ids.len += num;
}

static void
pwasm_module_add_element_fns_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_module_add_element_fns_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pwasm_module_init_on_error(text, init_data);
}

static const pwasm_parse_u32s_cbs_t
PWASM_MODULE_ADD_ELEMENT_FNS_CBS = {
  .on_items = pwasm_module_add_element_fns_on_items,
  .on_error = pwasm_module_add_element_fns_on_error,
};

static inline bool
pwasm_module_add_element_fns(
  pwasm_module_t * const mod,
  pwasm_module_init_t * const init_data,
  const size_t ofs,
  const pwasm_buf_t src
) {
  // populate function id slice offset and length
  mod->elements[ofs].func_ids.ofs = init_data->sizes.num_element_func_ids;
  mod->elements[ofs].func_ids.len = 0;

  // build function id parsing callback data
  pwasm_module_add_element_fns_t fns_data = {
    .init_data  = init_data,
    .ofs        = ofs,
    .success    = true,
  };

  // parse function IDs
  pwasm_parse_u32s(src, &PWASM_MODULE_ADD_ELEMENT_FNS_CBS, &fns_data);

  // return result
  return fns_data.success;
}

static void
pwasm_module_init_on_elements(
  const pwasm_element_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_module_t * const mod = data->mod;

  for (size_t i = 0; i < num; i++) {
    // get offset
    const size_t ofs = data->sizes.num_elements + i;

    // set table ID
    mod->elements[ofs].table_id = rows[i].table_id;

    // add element expression, check for error
    if (!pwasm_module_add_element_expr(mod, data, ofs, rows[i].expr.buf)) {
      // add failed, exit
      return;
    }

    // add element function IDs, check for error
    if (!pwasm_module_add_element_fns(mod, data, ofs, rows[i].func_ids)) {
      // add failed, exit
      return;
    }
  }

  // increment number of elements
  data->sizes.num_elements += num;
}

typedef struct {
  pwasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pwasm_module_add_segment_expr_t;

static void
pwasm_module_add_segment_expr_on_insts(
  const pwasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_module_add_segment_expr_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  if (!data->success) {
    return;
  }

  // calculate destination and number of bytes
  pwasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pwasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  // update expr slice length
  init_data->mod->data_segments[data->ofs].expr.len += num;
}

static void
pwasm_module_add_segment_expr_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_module_add_segment_expr_t * const data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pwasm_module_init_on_error(text, init_data);
}

static const pwasm_old_parse_expr_cbs_t
PWASM_MODULE_ADD_SEGMENT_EXPR_CBS = {
  .on_insts = pwasm_module_add_segment_expr_on_insts,
  .on_error = pwasm_module_add_segment_expr_on_error,
};

static inline bool
pwasm_module_add_segment_expr(
  pwasm_module_t * const mod,
  pwasm_module_init_t * const init_data,
  const size_t ofs,
  const pwasm_buf_t src
) {
  // populate expression slice offset and length
  mod->data_segments[ofs].expr.ofs = init_data->sizes.num_insts;
  mod->data_segments[ofs].expr.len = 0;

  // build expr parsing callback data
  pwasm_module_add_segment_expr_t expr_data = {
    .init_data  = init_data,
    .ofs        = ofs,
    .success    = true,
  };

  // parse expression
  // FIXME: should limit to constant expr here
  (void) pwasm_old_parse_expr(
    src,
    &PWASM_MODULE_ADD_SEGMENT_EXPR_CBS,
    &expr_data
  );

  return expr_data.success;
}


static void
pwasm_module_init_on_data_segments(
  const pwasm_data_segment_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  pwasm_module_t * const mod = data->mod;

  for (size_t i = 0; i < num; i++) {
    // get offset
    const size_t ofs = data->sizes.num_data_segments + i;

    // set memory ID
    mod->data_segments[ofs].mem_id = rows[i].mem_id;
    mod->data_segments[ofs].data = rows[i].data;

    // add expression, check for error
    if (!pwasm_module_add_segment_expr(mod, data, ofs, rows[i].expr.buf)) {
      // add failed, exit
      return;
    }
  }

  // increment count
  data->sizes.num_data_segments += num;
}

typedef struct {
  pwasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pwasm_module_add_code_t;

static void
pwasm_module_add_code_on_locals(
  const pwasm_local_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_module_add_code_t *data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  // calculate destination and number of bytes
  pwasm_local_t * const dst = init_data->mod->locals + init_data->sizes.num_locals;
  const size_t num_bytes = sizeof(pwasm_local_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_locals += num;

  // update function locals slice length
  init_data->mod->functions[data->ofs].locals.len += num;
}

typedef struct {
  pwasm_module_t * const mod;
  pwasm_inst_t * const inst;
} pwasm_module_add_br_table_t;

static void
pwasm_module_add_br_table_on_count(
  const uint32_t count,
  void *cb_data
) {
  pwasm_module_add_br_table_t * const data = cb_data;
  data->inst->v_br_table.labels.slice.ofs = data->mod->num_labels;
  data->inst->v_br_table.labels.slice.len = count;
}

static void
pwasm_module_add_br_table_on_items(
  const uint32_t * const labels,
  const size_t num,
  void *cb_data
) {
  pwasm_module_add_br_table_t * const data = cb_data;
  uint32_t * const dst = data->mod->labels + data->mod->num_labels;
  const size_t num_bytes = sizeof(uint32_t) * num;

  memcpy(dst, labels, num_bytes);
  data->mod->num_labels += num;
}

static const pwasm_parse_u32s_cbs_t
PWASM_MODULE_ADD_BR_TABLE_CBS = {
  .on_count = pwasm_module_add_br_table_on_count,
  .on_items = pwasm_module_add_br_table_on_items,
  // FIXME: need to handle errors
  // .on_error   = pwasm_module_br_table_on_error,
};

static bool
pwasm_module_add_br_table(
  pwasm_module_t * const mod,
  pwasm_inst_t * const inst
) {
  const pwasm_buf_t buf = inst->v_br_table.labels.buf;
  pwasm_module_add_br_table_t data = {
    .mod  = mod,
    .inst = inst,
  };

  // add labels, check for error
  const size_t num_bytes = pwasm_old_parse_labels(buf, &PWASM_MODULE_ADD_BR_TABLE_CBS, &data);

  // return result
  return num_bytes > 0;
}

static void
pwasm_module_add_code_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_module_add_code_t *data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;
  data->success = false;
  pwasm_module_init_on_error(text, init_data);
}

static void
pwasm_module_add_code_on_insts(
  const pwasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_module_add_code_t *data = cb_data;
  pwasm_module_init_t * const init_data = data->init_data;

  // calculate destination and number of bytes
  pwasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pwasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  for (size_t i = 0; i < num; i++) {
    if (dst[i].op == PWASM_OP_BR_TABLE) {
      if (!pwasm_module_add_br_table(init_data->mod, dst + i)) {
        pwasm_module_add_code_on_error("invalid br_table labels", cb_data);
        return;
      }
    }
  }

  // update function insts slice length
  init_data->mod->functions[data->ofs].insts.len += num;
}

static const pwasm_parse_function_cbs_t
PWASM_MODULE_ADD_CODE_CBS = {
  .on_error  = pwasm_module_add_code_on_error,
  .on_locals = pwasm_module_add_code_on_locals,
  .on_insts  = pwasm_module_add_code_on_insts,
};

bool
pwasm_module_add_code(
  pwasm_module_init_t * const init_data,
  const size_t ofs,
  const pwasm_buf_t src
) {
  // build callback data
  pwasm_module_add_code_t data = {
    .init_data = init_data,
    .ofs = ofs,
    .success = true,
  };

  // add code
  pwasm_parse_function(src, &PWASM_MODULE_ADD_CODE_CBS, &data);

  // return result
  return data.success;
}

static void
pwasm_module_init_on_function_codes(
  const pwasm_buf_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pwasm_module_init_t * const data = cb_data;
  const size_t num_imports = data->sizes.num_import_types[PWASM_IMPORT_TYPE_FUNC];

  for (size_t i = 0; i < num; i++) {
    // get offset
    const size_t ofs = num_imports + data->sizes.num_function_codes + i;

    // add function code, check for error
    if (!pwasm_module_add_code(data, ofs, rows[i])) {
      return;
    }
  }

  // increment function codes
  data->sizes.num_function_codes += num;
}

static const pwasm_parse_module_cbs_t
PWASM_MOD_INIT_CBS = {
  .on_error           = pwasm_module_init_on_error,
  .on_custom_section  = pwasm_module_init_on_custom_section,
  .on_function_types  = pwasm_module_init_on_function_types,
  .on_imports         = pwasm_module_init_on_imports,
  .on_functions       = pwasm_module_init_on_functions,
  .on_tables          = pwasm_module_init_on_tables,
  .on_memories        = pwasm_module_init_on_memories,
  .on_globals         = pwasm_module_init_on_globals,
  .on_exports         = pwasm_module_init_on_exports,
  .on_start           = pwasm_module_init_on_start,
  .on_elements        = pwasm_module_init_on_elements,
  .on_data_segments   = pwasm_module_init_on_data_segments,
  .on_function_codes  = pwasm_module_init_on_function_codes,
};

bool
pwasm_module_init(
  pwasm_module_t * const mod,
  const pwasm_module_init_cbs_t * const cbs,
  void *cb_data
) {
  mod->has_start = false;
  pwasm_module_init_t data = {
    .mod = mod,
    .sizes = {},
    .success = true,
    .cbs = cbs,
    .cb_data = cb_data,
  };

  // parse module, check for error
  if (!pwasm_parse_module(mod->src.ptr, mod->src.len, &PWASM_MOD_INIT_CBS, &data)) {
    // return failure
    return false;
  }

  // return success
  return true;
}

static const char *
PWASM_CHECK_TYPE_NAMES[] = {
#define PWASM_CHECK_TYPE(type, name, c) name,
PWASM_CHECK_TYPES
#undef PWASM_CHECK_TYPE
};

const char *
pwasm_check_type_get_name(
  const pwasm_check_type_t type
) {
  const size_t ofs = MIN(PWASM_CHECK_TYPE_LAST, type);
  return PWASM_CHECK_TYPE_NAMES[ofs];
}

typedef struct {
  const pwasm_module_t * const mod;
  size_t num_errors;
  const pwasm_check_cbs_t * const cbs;
  void *cb_data;
} pwasm_check_t;

static void
pwasm_check_fail(
  pwasm_check_t * const check,
  const pwasm_check_type_t type,
  const size_t id,
  const char * const text
) {
  if (check->cbs && check->cbs->on_error) {
    check->cbs->on_error(type, id, text, check->cb_data);
  }
  check->num_errors++;
}

#define FAIL_CHECK(check, type, id, text) \
  pwasm_check_fail((check), PWASM_CHECK_TYPE_ ## type, (id), (text));

static void
pwasm_check_function_types(
  pwasm_check_t * const check
) {
  const pwasm_function_type_t * const rows = check->mod->function_types;
  const size_t num = check->mod->num_function_types;

  for (size_t i = 0; i < num; i++) {
    if (rows[i].results.len > 1) {
      FAIL_CHECK(check, FUNCTION_TYPE, i, "too many results");
    }
  }
}

static void
pwasm_check_import_function(
  pwasm_check_t * const check,
  const size_t id,
  const pwasm_import_t row
) {
  if (row.func.id >= check->mod->num_functions) {
    FAIL_CHECK(check, IMPORT, id, "invalid import function id");
  }
}

static void
pwasm_check_import_table(
  pwasm_check_t * const check,
  const size_t id,
  const pwasm_import_t row
) {
  const pwasm_limits_t limits = row.mem.limits;
  if (row.table.elem_type != 0x70) {
    FAIL_CHECK(check, IMPORT, id, "invalid element type");
  }

  if (limits.has_max) {
    if (limits.max < limits.min) {
      FAIL_CHECK(check, IMPORT, id, "maximum is less than minimum");
    }
  }
}

static void
pwasm_check_import_memory(
  pwasm_check_t * const check,
  const size_t id,
  const pwasm_import_t row
) {
  const uint32_t MAX_SIZE = (1 << 16);
  const pwasm_limits_t limits = row.mem.limits;

  if (limits.min > MAX_SIZE) {
    FAIL_CHECK(check, IMPORT, id, "minimum is greater than 65536");
  }

  if (limits.has_max) {
    if (limits.max < limits.min) {
      FAIL_CHECK(check, IMPORT, id, "maximum is less than minimum");
    }

    if (limits.max > MAX_SIZE) {
      FAIL_CHECK(check, IMPORT, id, "maximum is greater than 65536");
    }
  }
}

static void
pwasm_check_import_global(
  pwasm_check_t * const check,
  const size_t id,
  const pwasm_import_t row
) {
  (void) check;
  (void) id;
  (void) row;
  // TODO: do we need any extra validation here?
}

static void
pwasm_check_import_invalid(
  pwasm_check_t * const check,
  const size_t id,
  const pwasm_import_t row
) {
  (void) row;
  FAIL_CHECK(check, IMPORT, id, "invalid import type");
}

#define PWASM_IMPORT_TYPE(type, name, suffix) \
  case PWASM_IMPORT_TYPE_ ## type: \
    pwasm_check_import_ ## suffix(check, i, row); \
    break;

static void
pwasm_check_imports(
  pwasm_check_t * const check
) {
  const pwasm_import_t * const rows = check->mod->imports;
  const size_t num = check->mod->num_imports;

  for (size_t i = 0; i < num; i++) {
    const pwasm_import_t row = rows[i];

    // check import module name
    if (!pwasm_utf8_is_valid(row.module)) {
      FAIL_CHECK(check, IMPORT, i, "invalid import module name");
    }

    // check import function name
    if (!pwasm_utf8_is_valid(row.name)) {
      FAIL_CHECK(check, IMPORT, i, "invalid import function name");
    }

    switch (row.type) {
    PWASM_IMPORT_TYPES
    default:
      pwasm_check_import_invalid(check, i, row);
    }
  }
}

#undef PWASM_IMPORT_TYPE

static void
pwasm_check_start(
  pwasm_check_t * const check
) {
  const pwasm_module_t * const mod = check->mod;

  if (!mod->has_start) {
    // no start function, so no check needed
    return;
  }

  // check start function index
  if (mod->start >= mod->num_functions) {
    FAIL_CHECK(check, START, 0, "invalid start function index");
    return;
  }

  // get/check function type offset
  const size_t type_id = mod->functions[mod->start].type_id;
  if (type_id >= mod->num_function_types) {
    FAIL_CHECK(check, START, 0, "invalid start function type index");
    return;
  }

  // get function type
  const pwasm_function_type_t type = mod->function_types[type_id];

  // check length of parameters vector
  if (type.params.len > 0) {
    FAIL_CHECK(check, START, 0, "start function must take no parameters");
  }

  // check length of results vector
  if (type.results.len > 0) {
    FAIL_CHECK(check, START, 0, "start function must not return results");
  }
}
/**
 * Get the type of the Nth local.
 *
 * Note: Must be called with a valid function ID, and the function ID
 * must have a valid type.
 */
static inline bool
pwasm_function_get_nth_local(
  const pwasm_module_t * const mod,
  const size_t fn_id,
  const size_t local_id,
  pwasm_value_type_t * const ret_type
) {
  // NOTE: assuming valid function ID and function type
  const pwasm_function_t fn = mod->functions[fn_id];

  // start with parameter count
  const pwasm_buf_t params = mod->function_types[fn.type_id].params;
  if (local_id < params.len) {
    if (ret_type) {
      // copy to result
      *ret_type = params.ptr[local_id];
    }

    // return success
    return true;
  }

  // append number of locals
  size_t sum = params.len;
  for (size_t i = 0; i < fn.locals.len; i++) {
    const pwasm_local_t locals = mod->locals[fn.locals.ofs + i];
    if ((local_id >= sum) && (local_id < sum + locals.num)) {
      if (ret_type) {
        // copy to result
        *ret_type = params.ptr[local_id];
      }

      // return success
      return true;
    }

    // increment sum
    sum += locals.num;
  }

  // no matching local found, return failure
  return false;
}


/**
 * Count the total number of local entries (parameters and locals)
 * available to this function.
 *
 * Note: Performs no error checking; must be called with a valid
 * function ID, and the function ID must have a valid type.
 */
static inline size_t
pwasm_function_get_max_local(
  const pwasm_module_t * const mod,
  const size_t fn_id
) {
  const pwasm_function_t fn = mod->functions[fn_id];

  // start with parameter count
  size_t sum = mod->function_types[fn.type_id].params.len;

  // append number of locals
  for (size_t i = 0; i < fn.locals.len; i++) {
    sum += mod->locals[fn.locals.ofs + i].num;
  }

  // return result
  return sum;
}

static void
pwasm_check_function_local_insts(
  pwasm_check_t * const check,
  const size_t fn_id
) {
  const pwasm_module_t * const mod = check->mod;
  const pwasm_function_t fn = mod->functions[fn_id];

  // get maximum local ID
  const size_t max_local = pwasm_function_get_max_local(check->mod, fn_id);

  // check local get/set/tee instructions
  for (size_t i = 0; i < fn.insts.len; i++) {
    const pwasm_inst_t in = mod->insts[i];
    if (pwasm_op_is_local(in.op) && (in.v_index.id >= max_local)) {
      FAIL_CHECK(check, FUNCTION, fn_id, "invalid local index");
    }
  }
}

static void
pwasm_check_function_call_insts(
  pwasm_check_t * const check,
  const size_t fn_id
) {
  const pwasm_module_t * const mod = check->mod;
  const pwasm_function_t fn = mod->functions[fn_id];
  const size_t num_functions = mod->num_functions;

  // check call instructions
  for (size_t i = 0; i < fn.insts.len; i++) {
    const pwasm_inst_t in = mod->insts[i];
    if ((in.op == PWASM_OP_CALL) && (in.v_index.id >= num_functions)) {
      FAIL_CHECK(check, FUNCTION, fn_id, "invalid function call");
    }
  }
}

static void
pwasm_check_function_global_insts(
  pwasm_check_t * const check,
  const size_t fn_id
) {
  const pwasm_module_t * const mod = check->mod;
  const pwasm_function_t fn = mod->functions[fn_id];
  const size_t num_globals = mod->num_globals;

  // check global get/set instructions
  for (size_t i = 0; i < fn.insts.len; i++) {
    const pwasm_inst_t in = mod->insts[i];
    if (pwasm_op_is_global(in.op) && (in.v_index.id >= num_globals)) {
      FAIL_CHECK(check, FUNCTION, fn_id, "invalid global index");
    }
  }
}

typedef enum {
  PWASM_STACK_CHECK_ENTRY_INIT,
  PWASM_STACK_CHECK_ENTRY_FRAME,
  PWASM_STACK_CHECK_ENTRY_BLOCK,
  PWASM_STACK_CHECK_ENTRY_LOOP,
  PWASM_STACK_CHECK_ENTRY_IF,
  PWASM_STACK_CHECK_ENTRY_VALUE,
  PWASM_STACK_CHECK_ENTRY_TRAP,
  PWASM_STACK_CHECK_ENTRY_LAST,
} pwasm_stack_check_entry_type_t;

static inline bool
pwasm_check_mem_inst(
  const size_t num_memories,
  const pwasm_inst_t in,
  const uint8_t head_entry_type,
  const uint8_t head_value_type
) {
  // get alignment immediate and opcode number of bits
  const size_t align = in.v_mem.align;
  const size_t num_bits = pwasm_op_get_num_bits(in.op);

  return (
    // check if module has memory
    (num_memories > 0) &&

    // check for sane alignment
    (align <= 3) &&

    // check alignment immediate for source (need: 2^align <= bits/8)
    ((1UL << align) <= (num_bits / 8)) &&

    // is top stack entry a value
    (head_entry_type == PWASM_STACK_CHECK_ENTRY_VALUE) &&

    // is top stack entry value an i32
    (head_value_type == PWASM_VALUE_TYPE_I32)
  );
}

static size_t
pwasm_find_block_end(
  const pwasm_module_t * const mod,
  const size_t fn_id,
  const size_t in_ofs
) {
  const pwasm_function_t * const fn = mod->functions + fn_id;
  const pwasm_inst_t * const insts = mod->insts + fn->insts.ofs;
  const size_t num_insts = fn->insts.len;

  size_t depth = 1;
  for (size_t i = in_ofs + 1; i < num_insts; i++) {
    switch (insts[i].op) {
    case PWASM_OP_IF:
    case PWASM_OP_BLOCK:
    case PWASM_OP_LOOP:
      depth++;
      break;
    case PWASM_OP_END:
      depth--;

      if (!depth) {
        // return result
        return i;
      }

      break;
    default:
      // do nothing
      break;
    }
  }

  // return failure
  return in_ofs;
}

#define E(en) PWASM_STACK_CHECK_ENTRY_ ## en
#define OP(op) PWASM_OP_ ## op
#define VT(vt) PWASM_VALUE_TYPE_ ## vt

#define TRAP(msg) do { \
  FAIL_CHECK(check, FUNCTION, fn_id, msg); \
  stack[0].entry = E(TRAP); \
  depth = 1; \
  goto retry; \
} while (0)

#define PUSH(e_type, v_type) do { \
  stack[depth].entry = E(e_type); \
  stack[depth].value = (v_type); \
  depth++; \
  if (depth == PWASM_STACK_CHECK_MAX_DEPTH) { \
    TRAP("stack underflow"); \
  } \
} while (0)

#define PEEK(n) (stack[depth - 1 - (n)])

#define POP() do { \
  if (depth) { \
    depth--; \
  } else { \
    TRAP("stack underflow"); \
  } \
} while (0)

#define CHECK_LOAD(in_name, val_type) do { \
  if (!pwasm_check_mem_inst(mod->num_memories, in, PEEK(0).entry, PEEK(0).value)) { \
    TRAP(in_name ": invalid memory access"); \
  } \
  \
  POP(); \
  PUSH(VALUE, VT(val_type)); \
} while (0)

#define CHECK_STORE(in_name, val_type) do { \
  if (depth < 2) { \
    TRAP(in_name ": stack underflow"); \
  } \
  \
  if (!pwasm_check_mem_inst(mod->num_memories, in, PEEK(1).entry, PEEK(1).value)) { \
    TRAP(in_name ": invalid memory access"); \
  } \
  \
  if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(val_type))) { \
    TRAP(in_name ": invalid value operand"); \
  } \
  \
  POP(); /* pop value */ \
  POP(); /* pop offset */ \
} while (0)

static void
pwasm_check_function_stack(
  pwasm_check_t * const check,
  const size_t fn_id
) {
  const pwasm_module_t * const mod = check->mod;
  const pwasm_function_t fn = mod->functions[fn_id];
  const pwasm_buf_t fn_results = mod->function_types[fn.type_id].results;

  size_t depth = 2;
  struct {
    // type of entry (init, frame, value, or trap)
    pwasm_stack_check_entry_type_t entry;

    // value (varies by entry type)
    // * VALUE: the value type
    // * IF/BLOCK/LOOP: return value type
    // * FRAME: return value type
    uint32_t value; // type of value (e.g. i32, i64, etc, or 0x40 for none)
  } stack[PWASM_STACK_CHECK_MAX_DEPTH] = {{
    .entry = E(INIT),
  }, {
    .entry = E(FRAME),
    .value = (fn_results.len > 0) ? fn_results.ptr[0] : 0x40,
  }};

  // FIXME: should this be one for an implicit label at the end of the
  // function?
  size_t num_labels = 0;
  size_t labels[PWASM_STACK_CHECK_MAX_DEPTH];

  for (size_t i = 0; i < fn.insts.len; i++) {
    const pwasm_inst_t in = mod->insts[fn.insts.ofs + i];

    retry:
    switch (stack[depth - 1].entry) {
    case E(TRAP):
      // do nothing
      break;
    case E(FRAME):
    case E(BLOCK):
    case E(LOOP):
    case E(IF):
    case E(VALUE):
      switch (in.op) {
      case OP(UNREACHABLE):
      case OP(NOP):
        break;
      case OP(BLOCK):
        PUSH(BLOCK, in.v_block.type);

        {
          // get end, check for error
          const size_t end = pwasm_find_block_end(mod, fn_id, i);
          if (end == i) {
            TRAP("block: missing end");
          }

          // push label
          labels[num_labels++] = i;
        }

        break;
      case OP(LOOP):
        PUSH(LOOP, in.v_block.type);
        labels[num_labels++] = i;

        break;
      case OP(IF):
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("if: invalid operand");
        }

        POP();
        PUSH(IF, in.v_block.type);

        {
          // get end, check for error
          const size_t end = pwasm_find_block_end(mod, fn_id, i);
          if (end == i) {
            TRAP("if: missing end");
          }

          // push label
          labels[num_labels++] = i;
        }

        break;
      case OP(ELSE):
        {
          // cache value type
          const uint32_t type = (PEEK(0).entry == E(VALUE)) ? PEEK(0).value : 0;

          if (!num_labels) {
            TRAP("invalid else");
          }

          const size_t if_ofs = labels[num_labels - 1];
          if (stack[if_ofs].entry != E(IF)) {
            TRAP("else outside of if");
          }

          const uint32_t result_type = stack[if_ofs].value;

          // check value type
          if (result_type != 0x40 && result_type != type) {
            TRAP("else: invalid return operand");
          }

          depth = if_ofs + 1;
        }

        break;
      case OP(END):
        {
          // cache value type
          const uint32_t type = (PEEK(0).entry == E(VALUE)) ? PEEK(0).value : 0;

          if (!num_labels) {
            // FIXME: add frame support
            TRAP("invalid end");
          }

          const size_t block_ofs = labels[num_labels - 1];
          const uint32_t result_type = stack[block_ofs].value;

          // check value type
          if (result_type != 0x40 && result_type != type) {
            TRAP("end: invalid return operand");
          }

          depth = block_ofs;
          num_labels--;

          if (result_type != 0x40) {
            // push result type
            PUSH(VALUE, result_type);
          }
        }

        break;
      case OP(BR):
        {
          if (in.v_index.id >= num_labels) {
            TRAP("br: invalid label index");
          }

          // cache value type
          const uint32_t type = (PEEK(0).entry == E(VALUE)) ? PEEK(0).value : 0;

          if (!num_labels) {
            TRAP("br: unnested");
          }

          const size_t block_ofs = labels[num_labels - 1 - in.v_index.id];

          if (stack[block_ofs].entry != E(LOOP)) {
            // check result type
            const uint32_t result_type = stack[block_ofs].value;
            if (result_type != 0x40 && result_type != type) {
              TRAP("br: invalid return operand");
            }
          }
        }

        break;
      case OP(BR_IF):
        {
          if (in.v_index.id >= num_labels) {
            TRAP("br_if: invalid label index");
          }

          if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
            TRAP("br_if: mission condition operand");
          }

          POP();

          // cache value type
          const uint32_t type = (PEEK(0).entry == E(VALUE)) ? PEEK(0).value : 0;

          if (!num_labels) {
            TRAP("unnested br_if");
          }

          const size_t block_ofs = labels[num_labels - 1 - in.v_index.id];

          if (stack[block_ofs].entry != E(LOOP)) {
            // check result type
            const uint32_t result_type = stack[block_ofs].value;
            if (result_type != 0x40 && result_type != type) {
              TRAP("br_if: invalid return operand");
            }
          }
        }

        break;
      case OP(BR_TABLE):
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("br_table: missing index operand");
        }

        POP();

        if (!num_labels) {
          TRAP("unnested br_table");
        }

        {
          // check return types
          const pwasm_slice_t br_labels = in.v_br_table.labels.slice;
          for (size_t j = 0; j < br_labels.len; j++) {
            const uint32_t label = mod->labels[br_labels.ofs + j];
            if (label >= num_labels) {
              TRAP("br_table: invalid label index");
            }

            // cache value type
            const uint32_t type = (PEEK(0).entry == E(VALUE)) ? PEEK(0).value : 0;
            const size_t block_ofs = labels[num_labels - 1 - label];

            if (stack[block_ofs].entry != E(LOOP)) {
              // check result type
              const uint32_t result_type = stack[block_ofs].value;
              if (result_type != 0x40 && result_type != type) {
                TRAP("br_table: invalid return operand");
              }
            }
          }
        }

        break;
      case OP(RETURN):
        // TODO
        break;
      case OP(CALL):
        {
          // get call function
          const size_t fn_id = in.v_index.id;
          if (fn_id >= mod->num_functions) {
            TRAP("call: invalid function index");
          }

          // get/check type id
          const size_t type_id = mod->functions[fn_id].type_id;
          if (type_id >= mod->num_function_types) {
            TRAP("call: invalid type index");
          }

          // get function type
          const pwasm_function_type_t type = mod->function_types[type_id];

          // check parameter length
          if (type.params.len > depth - 1) {
            TRAP("call: parameter length mismatch");
          }

          // count parameter type matches
          size_t num_matches = 0;
          for (size_t j = 0; j < type.params.len; j++) {
            const size_t ofs = depth - 1 - type.params.len + j;
            num_matches += (
              (stack[ofs].entry == E(VALUE)) &&
              (stack[ofs].value == type.params.ptr[j])
            );
          }

          // check parameter type matches
          if (num_matches != type.params.len) {
            TRAP("call: parameter type mismatch");
          }

          // pop parameters from stack
          depth -= type.params.len;

          // append results to stack
          for (size_t j = 0; j < type.results.len; j++) {
            PUSH(VALUE, type.results.ptr[j]);
          }
        }

        break;
      case OP(CALL_INDIRECT):
        // TODO

        break;
      case OP(DROP):
        if (PEEK(1).entry != E(VALUE)) {
          TRAP("drop: stack underflow");
        }

        POP();

        break;
      case OP(SELECT):
        // check stack size
        if (depth < 3) {
          TRAP("select: stack underflow");
        }

        // is 1st entry is an i32 value (conditional)
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("select: missing condition operand");
        }

        // are 2nd and 3rd stack entries values?
        if ((PEEK(1).entry != E(VALUE)) || (PEEK(2).entry != E(VALUE))) {
          TRAP("select: missing value operands");
        }
        // are 2nd and 3rd stack values the same type?
        if (PEEK(1).value != PEEK(2).value) {
          TRAP("select: value operand type mismatch");
        }

        POP();
        POP();

        break;
      case OP(LOCAL_GET):
        {
          const size_t id = in.v_index.id;

          pwasm_value_type_t type = 0;
          if (!pwasm_function_get_nth_local(mod, fn_id, id, &type)) {
            TRAP("local.get: invalid local index");
          }

          PUSH(VALUE, type);
        }

        break;
      case OP(LOCAL_SET):
        {
          const size_t id = in.v_index.id;

          // get local type
          pwasm_value_type_t type = 0;
          if (!pwasm_function_get_nth_local(mod, fn_id, id, &type)) {
            TRAP("local.set: invalid local index");
          }

          // check local type
          if (type != PEEK(0).value) {
            TRAP("local.set: type mismatch");
          }

          // pop value
          POP();
        }

        break;
      case OP(LOCAL_TEE):
        {
          const size_t id = in.v_index.id;

          // get local type
          pwasm_value_type_t type = 0;
          if (!pwasm_function_get_nth_local(mod, fn_id, id, &type)) {
            TRAP("local.tee: invalid local index");
          }

          // check local type
          if (type != PEEK(0).value) {
            TRAP("local.tee: type mismatch");
          }
        }

        break;
      case OP(GLOBAL_GET):
        {
          // get global type
          const size_t id = in.v_index.id;
          if (id >= mod->num_globals) {
            TRAP("global.get: invalid global index");
          }

          PUSH(VALUE, mod->globals[id].type.type);
        }

        break;
      case OP(GLOBAL_SET):
        {
          // get global index
          const size_t id = in.v_index.id;

          // check index
          if (id >= mod->num_globals) {
            TRAP("global.set: invalid global index");
          }

          // get type
          const pwasm_global_type_t type = mod->globals[id].type;

          // check immutability
          if (!type.mutable) {
            TRAP("global.set: cannot set immutable global");
          }

          // check value type
          if (type.type != PEEK(0).value) {
            TRAP("global.set: type mismatch");
          }

          // pop value
          POP();
        }

        break;
      case OP(I32_LOAD):
        CHECK_LOAD("i32.load", I32);
        break;
      case OP(I64_LOAD):
        CHECK_LOAD("i64.load", I64);
        break;
      case OP(F32_LOAD):
        CHECK_LOAD("f32.load", F32);
        break;
      case OP(F64_LOAD):
        CHECK_LOAD("f64.load", F64);
        break;
      case OP(I32_LOAD8_S):
        CHECK_LOAD("i32.load8_s", I32);
        break;
      case OP(I32_LOAD8_U):
        CHECK_LOAD("i32.load8_u", I32);
        break;
      case OP(I32_LOAD16_S):
        CHECK_LOAD("i32.load16_s", I32);
        break;
      case OP(I32_LOAD16_U):
        CHECK_LOAD("i32.load16_u", I32);
        break;
      case OP(I64_LOAD8_S):
        CHECK_LOAD("i64.load8_s", I64);
        break;
      case OP(I64_LOAD8_U):
        CHECK_LOAD("i64.load8_u", I64);
        break;
      case OP(I64_LOAD16_S):
        CHECK_LOAD("i64.load16_s", I64);
        break;
      case OP(I64_LOAD16_U):
        CHECK_LOAD("i64.load16_u", I64);
        break;
      case OP(I64_LOAD32_S):
        CHECK_LOAD("i64.load32_s", I64);
        break;
      case OP(I64_LOAD32_U):
        CHECK_LOAD("i64.load32_u", I64);
        break;
      case OP(I32_STORE):
        CHECK_STORE("i32.store", I32);
        break;
      case OP(I64_STORE):
        CHECK_STORE("i64.store", I64);
        break;
      case OP(F32_STORE):
        CHECK_STORE("f32.store", F32);
        break;
      case OP(F64_STORE):
        CHECK_STORE("f64.store", F64);
        break;
      case OP(I32_STORE8):
        CHECK_STORE("i32.store8", I32);
        break;
      case OP(I32_STORE16):
        CHECK_STORE("i32.store16", I32);
        break;
      case OP(I64_STORE8):
        CHECK_STORE("i64.store8", I64);
        break;
      case OP(I64_STORE16):
        CHECK_STORE("i64.store16", I64);
        break;
      case OP(I64_STORE32):
        CHECK_STORE("i64.store32", I64);
        break;
      case OP(MEMORY_SIZE):
        PUSH(VALUE, VT(I32));
        break;
      case OP(MEMORY_GROW):
        // is the top stack entry an i32 value?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("memory.grow: missing size operand");
        }

        // redundant, but whatever
        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I32_CONST):
        PUSH(VALUE, VT(I32));
        break;
      case OP(I64_CONST):
        PUSH(VALUE, VT(I64));
        break;
      case OP(F32_CONST):
        PUSH(VALUE, VT(F32));
        break;
      case OP(F64_CONST):
        PUSH(VALUE, VT(F64));
        break;
      case OP(I32_EQZ): // i32 tests
        // is the top stack entry an i32 value?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("missing test operand");
        }

        // redundant
        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I32_EQ): // i32 relops
      case OP(I32_NE):
      case OP(I32_LT_S):
      case OP(I32_LT_U):
      case OP(I32_GT_S):
      case OP(I32_GT_U):
      case OP(I32_LE_S):
      case OP(I32_LE_U):
      case OP(I32_GE_S):
      case OP(I32_GE_U):
        // are the top stack entries i32 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(I32))
        ) {
          TRAP("missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I64_EQZ):
        // is the top stack entry an i64 value?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64))) {
          TRAP("missing test operand");
        }

        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I64_EQ): // i64 tests
      case OP(I64_NE):
      case OP(I64_LT_S):
      case OP(I64_LT_U):
      case OP(I64_GT_S):
      case OP(I64_GT_U):
      case OP(I64_LE_S):
      case OP(I64_LE_U):
      case OP(I64_GE_S):
      case OP(I64_GE_U):
        // are the top stack entries i64 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(I64))
        ) {
          TRAP("missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(F32_EQ): // f32 relops
      case OP(F32_NE):
      case OP(F32_LT):
      case OP(F32_GT):
      case OP(F32_LE):
      case OP(F32_GE):
        // are the top stack entries f32 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F32)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(F32))
        ) {
          TRAP("missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(F64_EQ): // f64 relops
      case OP(F64_NE):
      case OP(F64_LT):
      case OP(F64_GT):
      case OP(F64_LE):
      case OP(F64_GE):
        // are the top stack entries f64 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F64)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(F64))
        ) {
          TRAP("missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I32_CLZ): // i32 unops
      case OP(I32_CTZ):
      case OP(I32_POPCNT):
        // is the top stack entry an i32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("i32 unop: missing operand");
        }

        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I32_ADD): // i32 binops
      case OP(I32_SUB):
      case OP(I32_MUL):
      case OP(I32_DIV_S):
      case OP(I32_DIV_U):
      case OP(I32_REM_S):
      case OP(I32_REM_U):
      case OP(I32_AND):
      case OP(I32_OR):
      case OP(I32_XOR):
      case OP(I32_SHL):
      case OP(I32_SHR_S):
      case OP(I32_SHR_U):
      case OP(I32_ROTL):
      case OP(I32_ROTR):
        // are the top stack entries i32 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(I32))
        ) {
          TRAP("i32 binop: missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I64_CLZ): // i64 unops
      case OP(I64_CTZ):
      case OP(I64_POPCNT):
        // is the top stack entry an i64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64))) {
          TRAP("i64 unop: missing operand");
        }

        POP();
        PUSH(VALUE, VT(I64));

        break;
      case OP(I64_ADD): // i64 binops
      case OP(I64_SUB):
      case OP(I64_MUL):
      case OP(I64_DIV_S):
      case OP(I64_DIV_U):
      case OP(I64_REM_S):
      case OP(I64_REM_U):
      case OP(I64_AND):
      case OP(I64_OR):
      case OP(I64_XOR):
      case OP(I64_SHL):
      case OP(I64_SHR_S):
      case OP(I64_SHR_U):
      case OP(I64_ROTL):
      case OP(I64_ROTR):
        // are the top stack entries i64 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(I64))
        ) {
          TRAP("i64 binop: missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(I64));

        break;
      case OP(F32_ABS): // f32 unops
      case OP(F32_NEG):
      case OP(F32_CEIL):
      case OP(F32_FLOOR):
      case OP(F32_TRUNC):
      case OP(F32_NEAREST):
      case OP(F32_SQRT):
        // is the top stack entry an f32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F32))) {
          TRAP("f32 unop: missing operand");
        }

        POP();
        PUSH(VALUE, VT(F32));

        break;
      case OP(F32_ADD): // f32 binops
      case OP(F32_SUB):
      case OP(F32_MUL):
      case OP(F32_DIV):
      case OP(F32_MIN):
      case OP(F32_MAX):
      case OP(F32_COPYSIGN):
        // are the top stack entries f32 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F32)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(F32))
        ) {
          TRAP("f32 binop: missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(F32));

        break;
      case OP(F64_ABS): // f64 unops
      case OP(F64_NEG):
      case OP(F64_CEIL):
      case OP(F64_FLOOR):
      case OP(F64_TRUNC):
      case OP(F64_NEAREST):
      case OP(F64_SQRT):
        // is the top stack entry an f64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F64))) {
          TRAP("f64 unop: missing operand");
        }

        POP();
        PUSH(VALUE, VT(F64));

        break;
      case OP(F64_ADD): // f64 binops
      case OP(F64_SUB):
      case OP(F64_MUL):
      case OP(F64_DIV):
      case OP(F64_MIN):
      case OP(F64_MAX):
      case OP(F64_COPYSIGN):
        // are the top stack entries f64 values?
        if (
          (depth < 2) ||
          (PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F64)) ||
          (PEEK(1).entry != E(VALUE)) || (PEEK(1).value != VT(F64))
        ) {
          TRAP("f64 binop: missing operand(s)");
        }

        POP();
        POP();
        PUSH(VALUE, VT(F64));

        break;
      case OP(I32_WRAP_I64):
        // is the top stack entry an i64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64))) {
          TRAP("i32.wrap_i64: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I32_TRUNC_F32_S):
      case OP(I32_TRUNC_F32_U):
        // is the top stack entry an f32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F32))) {
          TRAP("i32.trunc_f32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I32_TRUNC_F64_S):
      case OP(I32_TRUNC_F64_U):
        // is the top stack entry an f64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F64))) {
          TRAP("i32.trunc_f64: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I64_EXTEND_I32_S):
      case OP(I64_EXTEND_I32_U):
        // is the top stack entry an i32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("i64.extend_i32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I64));

        break;
      case OP(I64_TRUNC_F32_S):
      case OP(I64_TRUNC_F32_U):
        // is the top stack entry an f32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F32))) {
          TRAP("i64.trunc_f32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I64));

        break;
      case OP(I64_TRUNC_F64_S):
      case OP(I64_TRUNC_F64_U):
        // is the top stack entry an f64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F64))) {
          TRAP("i64.trunc_f64: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I64));

        break;
      case OP(F32_CONVERT_I32_S):
      case OP(F32_CONVERT_I32_U):
        // is the top stack entry an i32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("f32.convert_i32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F32));

        break;
      case OP(F32_CONVERT_I64_S):
      case OP(F32_CONVERT_I64_U):
        // is the top stack entry an i64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64))) {
          TRAP("f32.convert_i32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F32));

        break;
      case OP(F32_DEMOTE_F64):
        // is the top stack entry an f64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F64))) {
          TRAP("f32.demote_f64: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F32));

        break;
      case OP(F64_CONVERT_I32_S):
      case OP(F64_CONVERT_I32_U):
        // is the top stack entry an i32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("f64.convert_i32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F64));

        break;
      case OP(F64_CONVERT_I64_S):
      case OP(F64_CONVERT_I64_U):
        // is the top stack entry an i64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64))) {
          TRAP("f64.convert_i32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F64));

        break;
      case OP(F64_PROMOTE_F32):
        // is the top stack entry an f32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F32))) {
          TRAP("f64.promote_f32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F64));

        break;
      case OP(I32_REINTERPRET_F32):
        // is the top stack entry an f32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F32))) {
          TRAP("i32.reinterpret_f32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I32));

        break;
      case OP(I64_REINTERPRET_F64):
        // is the top stack entry an f64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(F64))) {
          TRAP("i64.reinterpret_f64: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(I64));

        break;
      case OP(F32_REINTERPRET_I32):
        // is the top stack entry an i32?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I32))) {
          TRAP("f32.reinterpret_i32: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F32));

        break;
      case OP(F64_REINTERPRET_I64):
        // is the top stack entry an i64?
        if ((PEEK(0).entry != E(VALUE)) || (PEEK(0).value != VT(I64))) {
          TRAP("f64.reinterpret_i64: invalid operand");
        }

        POP();
        PUSH(VALUE, VT(F64));

        break;
      default:
        TRAP("invalid opcode");
      }

      break;
    default:
      TRAP("invalid stack entry type");
    }
  }
}

#undef E
#undef TRAP
#undef PUSH
#undef POP
#undef PEEK
#undef CHECK_LOAD
#undef CHECK_STORE

static void
pwasm_check_function(
  pwasm_check_t * const check,
  const size_t fn_id
) {
  const pwasm_module_t * const mod = check->mod;
  const pwasm_function_t fn = mod->functions[fn_id];

  // check function source
  if (fn.source >= PWASM_SOURCE_LAST) {
    FAIL_CHECK(check, FUNCTION, fn_id, "invalid function source");
  }

  // get/check function type offset
  if (fn.type_id >= mod->num_function_types) {
    FAIL_CHECK(check, FUNCTION, fn_id, "invalid function type index");
    return;
  }

  // check local instructions
  pwasm_check_function_local_insts(check, fn_id);

  // check call instructions
  pwasm_check_function_call_insts(check, fn_id);

  // check global instructions
  pwasm_check_function_global_insts(check, fn_id);

  // check stack
  pwasm_check_function_stack(check, fn_id);

  // get function type
  // TODO: check arity, check expr
  const pwasm_function_type_t type = mod->function_types[fn.type_id];
  (void) type;
}

static void
pwasm_check_functions(
  pwasm_check_t * const check
) {
  const size_t num = check->mod->num_functions;

  for (size_t i = 0; i < num; i++) {
    pwasm_check_function(check, i);
  }
}

size_t
pwasm_check(
  const pwasm_module_t * const mod,
  const pwasm_check_cbs_t * const cbs,
  void *cb_data
) {
  // build check data
  pwasm_check_t check = {
    .mod = mod,
    .num_errors = 0,
    .cbs = cbs,
    .cb_data = cb_data,
  };

  // TODO: macro this once everything is implemented
  pwasm_check_function_types(&check);
  pwasm_check_imports(&check);
  pwasm_check_functions(&check);
  pwasm_check_start(&check);

  // return total number of errors
  return check.num_errors;
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
  if (!ptr && num_bytes > 0) {
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
  pwasm_vec_t * ret,
  const size_t stride
) {
  pwasm_vec_t vec = {
    .mem_ctx = mem_ctx,
    .max_rows = 10,
    .stride = stride,
  };

  if (!pwasm_vec_resize(&vec, vec.max_rows)) {
    return false;
  }

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

#define BUILDER_VEC(name, type, prev) \
  if (!pwasm_vec_init(mem_ctx, &(b.name ## s), sizeof(type))) { \
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
    const size_t size = sizeof(type); \
    const size_t ofs = pwasm_vec_get_size(vec) / size; \
    const size_t len = src_len * size; \
    const bool ok = pwasm_vec_push(vec, len, src_ptr, NULL); \
    return (pwasm_slice_t) { ok ? ofs : 0, ok ? src_len : 0 }; \
  }
BUILDER_VECS
#undef BUILDER_VEC

size_t
pwasm_builder_get_size(
  const pwasm_builder_t * const builder
) {
#define BUILDER_VEC(name, type, prev) \
  pwasm_vec_get_size(&(builder->name ## s)) * sizeof(type) +
  return (
BUILDER_VECS
  0 /* sentinel */
  );
#undef BUILDER_VEC
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
    .mem = { ptr, total_num_bytes },

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

#if 0
#define DEF_VEC_SECTION(NAME, TYPE) \
  typedef struct { \
    const pwasm_mod_parse_cbs_t * const cbs; \
    void *cb_data; \
    bool success; \
  } pwasm_mod_parse_ ## NAME ## _section_items_t; \
   \
  static void pwasm_mod_parse_ ## NAME ## _on_error( \
    const char * const text, \
    void *cb_data \
  ) { \
    pwasm_mod_parse_ ## NAME ## _section_items_t * const data = cb_data; \
   \
    if (data->success) { \
      if (data->cbs && data-cbs->on_error) { \
        data->cbs->on_error(text, data->cb_data); \
      } \
   \
      data->success = false; \
    } \
  } \
   \
  static void pwasm_mod_parse_ ## NAME ## _section_on_items( \
    const TYPE * const rows, \
    const size_t num, \
    void *cb_data \
  ) { \
    pwasm_mod_parse_ ## NAME ## _section_items_t * const data = cb_data; \
    if (!pwasm_builder_push_ ## NAME ## s(data->builder, rows, num).len) { \
      const char * const text = "push " #NAME "s failed"; \
      pwasm_mod_parse_ ## NAME ## _section_items_on_error(text, data); \
    } \
  } \
   \
  static const pwasm_parse_ ## NAME ## _cbs_t  \
  PWASM_MOD_PARSE_ ## NAME ## _SECTION_ITEMS_CBS = { \
    .on_items = pwasm_mod_parse_ ## NAME ## _section_items_on_items, \
    .on_error = pwasm_mod_parse_ ## NAME ## _section_items_on_error, \
  }; \
   \
  static size_t pwasm_mod_parse_ ## NAME ## _section( \
    const pwasm_buf_t src \
    const pwasm_mod_parse_cbs_t * const cbs,  \
    void *cb_data  \
  ) { \
    pwasm_mod_parse_ ## NAME ## _section_items_t data = { \
      .cbs = cbs, \
      .cb_data = cb_data, \
      .success = true; \
    }; \
   \
    const size_t len = pwasm_parse_ ## NAME ## s(src, &PWASM_MOD_PARSE_ ## NAME ## _SECTION_ITEMS_CBS, &data); \
    return data.success ? len : 0; \
  }

DEF_VEC_SECTION(type, pwasm_type_t)
#endif /* 0 */

typedef struct {
  const pwasm_mod_parse_cbs_t * const cbs;
  void *cb_data;
  bool success;
} pwasm_mod_parse_custom_section_t;

static void
pwasm_mod_parse_custom_section_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_mod_parse_custom_section_t * const data = cb_data;

  if (data->success) {
    data->success = false;
    data->cbs->on_error(text, data->cb_data);
  }
}

static const pwasm_parse_buf_cbs_t
PWASM_MOD_PARSE_CUSTOM_SECTION_CBS = {
  .on_error = pwasm_mod_parse_custom_section_on_error,
};

static size_t
pwasm_mod_parse_custom_section(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_slice_t slices[2];
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  pwasm_mod_parse_custom_section_t data = {
    .cbs = cbs,
    .cb_data = cb_data,
    .success = true,
  };

  for (size_t i = 0; i < 2; i++) {
    // parse to buffer, check for error
    pwasm_buf_t buf;
    const size_t len = pwasm_parse_buf(&buf, src, &PWASM_MOD_PARSE_CUSTOM_SECTION_CBS, &data);
    if (!len) {
      return 0;
    }

    // pass bytes to callback
    slices[i] = cbs->on_bytes(buf.ptr, buf.len, cb_data);
    if (!slices[i].len) {
      return 0;
    }

    // advance curr, increment number of bytes
    curr = pwasm_buf_step(curr, len + buf.len);
    num_bytes += len + buf.len;
  }

  // build section
  const pwasm_new_custom_section_t section = {
    .name = slices[0],
    .data = slices[1],
  };

  // pass section to callback
  cbs->on_custom_section(&section, cb_data);

  // return result
  return data.success ? num_bytes : 0;
}

DEF_VEC_PARSER(type, pwasm_type_t);
DEF_VEC_PARSER(import, pwasm_new_import_t);
DEF_VEC_PARSER(func, uint32_t);
DEF_VEC_PARSER(table, pwasm_table_t);
DEF_VEC_PARSER(mem, pwasm_limits_t);
DEF_VEC_PARSER(global, pwasm_new_global_t);
DEF_VEC_PARSER(export, pwasm_new_export_t);
DEF_VEC_PARSER(elem, pwasm_elem_t);
DEF_VEC_PARSER(code, pwasm_func_t); // FIXME
DEF_VEC_PARSER(segment, pwasm_segment_t);

typedef struct {
  const pwasm_mod_parse_cbs_t * const cbs;
  void *cb_data;
  bool success;
  pwasm_slice_t * const slice;
} pwasm_mod_parse_type_t;

static void
pwasm_mod_parse_type_on_items(
  const uint32_t * const rows,
  const size_t num,
  void *cb_data
) {
  pwasm_mod_parse_type_t * const data = cb_data;
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
pwasm_mod_parse_type_on_error(
  const char * const text,
  void *cb_data
) {
  pwasm_mod_parse_type_t * const data = cb_data;

  if (data->success) {
    data->success = false;
    data->cbs->on_error(text, data->cb_data);
  }
}

static const pwasm_parse_u32s_cbs_t
PWASM_MOD_PARSE_TYPE_CBS = {
  .on_items = pwasm_mod_parse_type_on_items,
  .on_error = pwasm_mod_parse_type_on_error,
};

static size_t
pwasm_mod_parse_type(
  pwasm_type_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  pwasm_slice_t slices[2];
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  for (size_t i = 0; i < 2; i++) {
    // build context
    pwasm_mod_parse_type_t data = {
      .cbs = cbs,
      .cb_data = cb_data,
      .success = true,
      .slice = slices + i,
    };

    // parse params, check for error
    const size_t len = pwasm_parse_u32s(src, &PWASM_MOD_PARSE_TYPE_CBS, &data);
    if (!len) {
      return 0;
    }

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

static inline size_t
pwasm_mod_parse_import_function( // FIXME: rename to "func"
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  (void) cbs;
  (void) cb_data;
  return pwasm_u32_decode(&(dst->func), src);
}

static inline size_t
pwasm_mod_parse_import_table(
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_mod_parse_table(&(dst->table), src, cbs, cb_data);
}

static inline size_t
pwasm_mod_parse_import_memory( // FIXME: rename to "mem"
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_parse_limits(&(dst->mem), src, cbs->on_error, cb_data);
}

static inline size_t
pwasm_mod_parse_import_global(
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  return pwasm_parse_global_type(&(dst->global), src, cbs->on_error, cb_data);
}

static inline size_t
pwasm_mod_parse_import_invalid(
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
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
pwasm_mod_parse_import_data(
  const pwasm_import_type_t type,
  pwasm_new_import_t * const dst,
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const cbs,
  void *cb_data
) {
  switch (type) {
#define PWASM_IMPORT_TYPE(ID, TEXT, NAME) \
  case PWASM_IMPORT_TYPE_ ## ID: \
    return pwasm_mod_parse_import_ ## NAME (dst, src, cbs, cb_data);
PWASM_IMPORT_TYPES
#undef PWASM_IMPORT_TYPE
  default:
    return pwasm_mod_parse_import_invalid(dst, src, cbs, cb_data);
  }
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
  const pwasm_mod_parse_cbs_t * const cbs,
  void * const cb_data
) {
  pwasm_buf_t curr = src;
  size_t num_bytes = 0;

  // parse module name, check for error
  // FIXME: handle on_error here
  pwasm_slice_t names[2];
  for (size_t i = 0; i < 2; i++) {
    pwasm_buf_t buf;
    const size_t len = pwasm_parse_buf(&buf, curr, NULL, cb_data);
    if (!len) {
      return 0;
    }

    // add bytes
    names[i] = cbs->on_bytes(buf.ptr, buf.len, cb_data);
    if (names[i].len) {
      return 0;
    }

    // advance buffer, increment byte count
    curr = pwasm_buf_step(curr, len);
    num_bytes += len;
  }

  if (curr.len < 2) {
    cbs->on_error("missing import type", cb_data);
    return 0;
  }

  // get import type
  const pwasm_import_type_t type = curr.ptr[0];

  // build result
  pwasm_new_import_t tmp = {
    .module = names[0],
    .name = names[1],
    .type = type,
  };

  curr = pwasm_buf_step(curr, 1);
  num_bytes += 1;

  const size_t len = pwasm_mod_parse_import_data(type, &(tmp), curr, cbs, cb_data);
  if (!len) {
    cbs->on_error("invalid import data", cb_data);
    return 0;
  }

  // add length to result
  curr = pwasm_buf_step(curr, len);
  num_bytes += len;

  // save result to destination
  *dst = tmp;

  // return number of bytes consumed
  return num_bytes;
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
  const pwasm_parse_expr_cbs_t cbs = {
    .on_insts = src_cbs->on_insts,
    .on_error = src_cbs->on_error,
  };

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

static pwasm_slice_t
pwasm_mod_parse_null_on_u32s(
  const uint32_t * const ptr,
  const size_t len,
  void *cb_data
) {
  (void) ptr;
  (void) len;
  (void) cb_data;
  return (pwasm_slice_t) { 0, 0 };
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
  return (pwasm_slice_t) { 0, 0 };
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
  // return callbacks
  return (pwasm_mod_parse_cbs_t) {
    .on_error = (cbs && cbs->on_error) ? cbs->on_error : pwasm_null_on_error,
    .on_bytes = (cbs && cbs->on_bytes) ? cbs->on_bytes : pwasm_mod_parse_null_on_bytes,
    .on_u32s = (cbs && cbs->on_u32s) ? cbs->on_u32s : pwasm_mod_parse_null_on_u32s,
    .on_section = (cbs && cbs->on_section) ? cbs->on_section : pwasm_mod_parse_null_on_section,
    .on_custom_section = (cbs && cbs->on_custom_section) ? cbs->on_custom_section : pwasm_mod_parse_null_on_custom_section,
  };
}

size_t
pwasm_mod_parse(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t * const src_cbs,
  void *cb_data
) {
  // cache callbacks locally
  const pwasm_mod_parse_cbs_t cbs = pwasm_mod_parse_get_cbs(src_cbs);

  // check source length
  if (src.len < 8) {
    cbs.on_error("source too small", cb_data);
    return 0;
  }

  // check magic and version
  if (memcmp(src.ptr, PWASM_HEADER, sizeof(PWASM_HEADER))) {
    cbs.on_error("invalid module header", cb_data);
    return 0;
  }

  pwasm_section_type_t max_type = 0;

  pwasm_buf_t curr = pwasm_buf_step(src, 8);
  while (curr.len > 0) {
    // get section header, check for error
    pwasm_header_t head;
    const size_t head_len = pwasm_header_parse(&head, curr);
    if (!head_len) {
      cbs.on_error("invalid section header", cb_data);
      return 0;
    }

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
      const pwasm_buf_t body = { curr.ptr + head_len, head.len };

      // parse section, check for error
      const size_t body_len = pwasm_mod_parse_section(head.type, body, &cbs, cb_data);
      if (!body_len) {
        // return failure
        return 0;
      }
    }

    // advance buffer
    curr = pwasm_buf_step(curr, head_len + head.len);
  }

  // return number of bytes consumed
  return src.len;
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

  const pwasm_slice_t ret = pwasm_builder_push_bytes(data->builder, bytes, num);
  if (!ret.len) {
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
    const size_t len = MIN((num - i), PWASM_BATCH_SIZE);

    uint32_t funcs[PWASM_BATCH_SIZE];
    for (size_t j = 0; j < len; j++) {
      funcs[j] = rows[i + j];
    }

    if (!pwasm_builder_push_funcs(data->builder, funcs, len).len) {
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
