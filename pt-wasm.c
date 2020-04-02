#include <stdbool.h> // bool
#include <string.h> // memcmp()
#include "pt-wasm.h"

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
} pt_wasm_parse_inst_ctx_t;

/**
 * Batch size.
 *
 * Used to batch up function types, imports, functions,
 * etc, when dispatching to parsing callbacks.
 *
 * Note: must be a power of two.
 */
#define PT_WASM_BATCH_SIZE 128

#ifdef PT_WASM_DEBUG
// FIXME: limit to DEBUG
#include <stdio.h>
#define D(fmt, ...) fprintf( \
  stderr, \
  "D %s:%d %s(): " fmt "\n", \
  __FILE__, __LINE__, __func__, __VA_ARGS__ \
)
#else
#define D(fmt, ...)
#endif /* PT_WASM_DEBUG */

#define DEF_VEC_PARSE_FN(FN_NAME, TEXT, EL_TYPE, CBS_TYPE, PARSE_FN, FLUSH_CB) \
static size_t FN_NAME ( \
  const pt_wasm_buf_t src, \
  const CBS_TYPE * const cbs, \
  void * const cb_data \
) { \
  /* get count, check for error */ \
  uint32_t num_els = 0; \
  size_t src_ofs = pt_wasm_decode_u32(&num_els, src.ptr, src.len); \
  if (!src_ofs) { \
    FAIL(TEXT ": invalid vector length"); \
  } \
  \
  /* element buffer */ \
  EL_TYPE dst[PT_WASM_BATCH_SIZE]; \
  \
  for (size_t i = 0; i < num_els; i++) { \
    const size_t dst_ofs = (i & (PT_WASM_BATCH_SIZE - 1)); \
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
    if ((dst_ofs == (PT_WASM_BATCH_SIZE - 1)) && cbs && cbs->FLUSH_CB) { \
      /* flush batch */ \
      cbs->FLUSH_CB(dst, PT_WASM_BATCH_SIZE, cb_data); \
    } \
  } \
  \
  /* count remaining entries */ \
  const size_t num_left = num_els & (PT_WASM_BATCH_SIZE - 1); \
  if (num_left && cbs && cbs->FLUSH_CB) { \
    /* flush remaining entries */ \
    cbs->FLUSH_CB(dst, num_left, cb_data); \
  } \
  \
  /* return number of bytes consumed */ \
  return src_ofs; \
}

#define PT_WASM_SECTION_TYPE(a, b) #b,
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

#define PT_WASM_IMPORT_TYPE(a, b) b,
static const char *PT_WASM_IMPORT_TYPE_NAMES[] = {
PT_WASM_IMPORT_TYPES
};
#undef PT_WASM_IMPORT_TYPE

const char *
pt_wasm_import_type_get_name(
  const pt_wasm_import_type_t v
) {
  const size_t ofs = MIN(PT_WASM_IMPORT_TYPE_LAST, v);
  return PT_WASM_IMPORT_TYPE_NAMES[ofs];
}

#define PT_WASM_EXPORT_TYPE(a, b) b,
static const char *PT_WASM_EXPORT_TYPE_NAMES[] = {
PT_WASM_EXPORT_TYPES
};
#undef PT_WASM_EXPORT_TYPE

const char *
pt_wasm_export_type_get_name(
  const pt_wasm_export_type_t v
) {
  const size_t ofs = MIN(PT_WASM_EXPORT_TYPE_LAST, v);
  return PT_WASM_EXPORT_TYPE_NAMES[ofs];
}

static inline bool
pt_wasm_is_valid_export_type(
  const uint8_t v
) {
  return v < PT_WASM_EXPORT_TYPE_LAST;
}

static const char *PT_WASM_VALUE_TYPE_NAMES[] = {
  "i32",
  "i64",
  "f32",
  "f64",
  "unknown type",
};

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

const char *
pt_wasm_value_type_get_name(
  const pt_wasm_value_type_t v
) {
  const size_t ofs = MIN(0x7F - (v & 0x7F), LEN(PT_WASM_VALUE_TYPE_NAMES));
  return PT_WASM_VALUE_TYPE_NAMES[ofs];
}

const char *
pt_wasm_result_type_get_name(
  const pt_wasm_result_type_t v
) {
  return (v == 0x40) ? "void" : pt_wasm_value_type_get_name(v);
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
  const void * const src_ptr,
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

static size_t
pt_wasm_parse_name(
  pt_wasm_buf_t * const ret_buf,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // check source length
  if (!src.len) {
    FAIL("empty name");
  }

  // decode name length, check for error
  uint32_t len = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&len, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("bad name length");
  }

  // D("src: %p, src_len = %zu, len = %u, len_ofs = %zu", src, src_len, len, len_ofs);

  // calculate total length, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src.len) {
    FAIL("truncated name");
  }

  // build result
  const pt_wasm_buf_t buf = {
    .ptr = src.ptr + len_ofs,
    .len = len,
  };

  if (ret_buf) {
    // save to result
    *ret_buf = buf;
  }

  // return section length, in bytes
  return num_bytes;
}

static size_t
pt_wasm_parse_value_type_list(
  pt_wasm_buf_t * const ret_buf,
  const pt_wasm_parse_module_cbs_t * const cbs,
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
  const size_t len_ofs = pt_wasm_decode_u32(&len, src, src_len);
  if (!len_ofs) {
    FAIL("bad value type list length");
  }

  // calculate total number of bytes, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src_len) {
    FAIL("value type list length too long");
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

typedef struct {
  void (*on_count)(const uint32_t, void *);
  void (*on_vals)(const uint32_t *, const size_t, void *);
  void (*on_error)(const char *, void *);
} pt_wasm_parse_u32s_cbs_t;

static bool
pt_wasm_parse_u32s(
  const pt_wasm_buf_t src,
  const pt_wasm_parse_u32s_cbs_t * const cbs,
  void *cb_data
) {
  size_t ofs = 0;

  // get count, check for error
  uint32_t num;
  const size_t n_len = pt_wasm_decode_u32(&num, src.ptr, src.len);
  if (!n_len) {
    FAIL("bad u32 vector count");
  }

  if (cbs && cbs->on_count) {
    cbs->on_count(num, cb_data);
  }

  // increment offset
  ofs += n_len;

  uint32_t vals[PT_WASM_BATCH_SIZE];

  size_t vals_ofs = 0;
  for (size_t i = 0; i < num; i++) {
    if (ofs > src.len) {
      FAIL("u32 vector buffer overflow");
    }

    // decode id, check for error
    const size_t len = pt_wasm_decode_u32(vals + vals_ofs, src.ptr + ofs, src.len - ofs);
    if (!len) {
      FAIL("bad u32 in u32 vector");
    }

    // increment vals offset
    vals_ofs++;
    if (vals_ofs == (LEN(vals) - 1)) {
      if (cbs && cbs->on_vals) {
        // flush batch
        cbs->on_vals(vals, PT_WASM_BATCH_SIZE, cb_data);
      }

      // reset offset
      vals_ofs = 0;
    }

    // increment offset
    ofs += len;
  }

  if ((vals_ofs > 0) && cbs && cbs->on_vals) {
    // flush remaining values
    cbs->on_vals(vals, vals_ofs, cb_data);
  }

  // return success
  return true;
}

static bool
pt_wasm_parse_custom_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // parse name, check for error
  pt_wasm_buf_t name;
  const size_t ofs = pt_wasm_parse_name(&name, cbs, src, cb_data);
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
pt_wasm_parse_function_type(
  pt_wasm_function_type_t * const dst_func_type,
  const pt_wasm_parse_module_cbs_t * const cbs,
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

  // parse results, check for error
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

DEF_VEC_PARSE_FN(
  pt_wasm_parse_types,
  "parse types",
  pt_wasm_function_type_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_function_type,
  on_function_types
);

static bool
pt_wasm_parse_type_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_types(src, cbs, cb_data);
}

/**
 * Parse limits into +dst+ from buffer +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_limits(
  pt_wasm_limits_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  if (src_len < 2) {
    FAIL("truncated limits");
  }

  // check limits flag
  if ((src[0] != 0) && (src[0] != 1)) {
    FAIL("bad limits flag");
  }

  // build result
  pt_wasm_limits_t tmp = {
    .has_max = (src[0] == 1),
    .min = 0,
    .max = 0,
  };

  // parse min, check for error
  const size_t min_len = pt_wasm_decode_u32(&(tmp.min), src + 1, src_len - 1);
  if (!min_len) {
    FAIL("bad limits minimum");
  }

  // build return value
  size_t num_bytes = 1 + min_len;

  if (src[0] == 1) {
    // parse max, check for error
    const size_t max_len = pt_wasm_decode_u32(&(tmp.max), src + num_bytes, src_len - num_bytes);
    if (!max_len) {
      FAIL("bad limits maximum");
    }

    // increment number of bytes
    num_bytes += max_len;
  }

  if (dst) {
    // save to result
    *dst = tmp;
  }

  // return number of bytes consumed
  return num_bytes;
}

/**
 * Parse table into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_table(
  pt_wasm_table_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  if (src_len < 3) {
    FAIL("incomplete table type");
  }

  // get element type, check for error
  // NOTE: at the moment only one element type is supported
  const pt_wasm_table_elem_type_t elem_type = src[0];
  if (elem_type != 0x70) {
    FAIL("invalid table element type");
  }

  // parse limits, check for error
  pt_wasm_limits_t limits;
  const size_t len = pt_wasm_parse_limits(&limits, cbs, src + 1, src_len - 1, cb_data);
  if (!len) {
    return 0;
  }

  // build temp table
  const pt_wasm_table_t tmp = {
    .elem_type  = elem_type,
    .limits     = limits,
  };

  if (dst) {
    // copy result
    *dst = tmp;
  }

  // return number of bytes consumed
  return 1 + len;
}

static const char *PT_WASM_IMM_NAMES[] = {
#define PT_WASM_IMM(a, b) b,
PT_WASM_IMM_DEFS
#undef PT_WASM_IMM
};

const char *
pt_wasm_imm_get_name(
  const pt_wasm_imm_t v
) {
  return PT_WASM_IMM_NAMES[MIN(v, PT_WASM_IMM_LAST)];
}

#define PT_WASM_OP(a, b, c) { \
  .name = (b), \
  .is_valid = true, \
  .imm = PT_WASM_IMM_##c, \
},

#define PT_WASM_OP_CONST(a, b, c) { \
  .name = (b), \
  .is_valid = true, \
  .is_const = true, \
  .imm = PT_WASM_IMM_##c, \
},

#define PT_WASM_OP_CONTROL(a, b, c) { \
  .name = (b), \
  .is_valid = true, \
  .is_control = true, \
  .imm = PT_WASM_IMM_##c, \
},

#define PT_WASM_OP_RESERVED(a, b) { \
  .name = ("reserved." b), \
  .imm = PT_WASM_IMM_LAST, \
},

static const struct {
  const char * name;
  bool is_control;
  bool is_valid;
  bool is_const;
  pt_wasm_imm_t imm;
} PT_WASM_OPS[] = {
PT_WASM_OP_DEFS
};
#undef PT_WASM_OP
#undef PT_WASM_OP_CONTROL
#undef PT_WASM_OP_RESERVED

const char *
pt_wasm_op_get_name(
  const pt_wasm_op_t op
) {
  return PT_WASM_OPS[op].name;
}

static inline bool
pt_wasm_op_is_valid(
  const uint8_t byte
) {
  return PT_WASM_OPS[byte].is_valid;
}

static inline pt_wasm_imm_t
pt_wasm_op_get_imm(
  const pt_wasm_op_t op
) {
  return PT_WASM_OPS[op].imm;
}

static inline bool
pt_wasm_op_is_control(
  const pt_wasm_op_t op
) {
  return PT_WASM_OPS[op].is_control;
}

static inline bool
pt_wasm_op_is_const(
  const pt_wasm_op_t op
) {
  return PT_WASM_OPS[op].is_const;
}

/**
 * Parse inst into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_inst(
  pt_wasm_inst_t * const dst,
  const pt_wasm_parse_inst_ctx_t ctx,
  const pt_wasm_buf_t src
) {
  // check source length
  if (src.len < 1) {
    INST_FAIL("short instruction");
  }

  // get op, check for error
  const pt_wasm_op_t op = src.ptr[0];
  if (!pt_wasm_op_is_valid(op)) {
    INST_FAIL("invalid op");
  }

  // build length and instruction
  size_t len = 1;
  pt_wasm_inst_t in = {
    .op = op,
  };

  // get op immediate
  switch (pt_wasm_op_get_imm(in.op)) {
  case PT_WASM_IMM_NONE:
    // do nothing
    break;
  case PT_WASM_IMM_BLOCK:
    {
      // check length
      if (src.len < 2) {
        INST_FAIL("missing result type immediate");
      }

      // get block result type, check for error
      const uint8_t type = src.ptr[1];
      if (!pt_wasm_is_valid_result_type(type)) {
        INST_FAIL("invalid result type");
      }

      // save result type, increment length
      in.v_block.type = type;
      len += 1;
    }

    break;
  case PT_WASM_IMM_INDEX:
    {
      // get index, check for error
      uint32_t id = 0;
      const size_t id_len = pt_wasm_decode_u32(&id, src.ptr + 1, src.len - 1);
      if (!id_len) {
        INST_FAIL("bad immediate index value");
      }

      // save index, increment length
      in.v_index.id = id;
      len += id_len;
    }

    break;
  case PT_WASM_IMM_CALL_INDIRECT:
    {
      // get index, check for error
      uint32_t id = 0;
      const size_t id_len = pt_wasm_decode_u32(&id, src.ptr + 1, src.len - 1);
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
  case PT_WASM_IMM_MEM:
    {
      // get align value, check for error
      uint32_t align = 0;
      const size_t a_len = pt_wasm_decode_u32(&align, src.ptr + 1, src.len - 1);
      if (!a_len) {
        INST_FAIL("bad align value");
      }

      // get offset value, check for error
      uint32_t offset = 0;
      const size_t o_len = pt_wasm_decode_u32(&offset, src.ptr + 1 + a_len, src.len - 1 - a_len);
      if (!o_len) {
        INST_FAIL("bad offset value");
      }

      // save alignment and offset, increment length
      in.v_mem.align = align;
      in.v_mem.offset = offset;
      len += a_len + o_len;
    }

    break;
  case PT_WASM_IMM_I32_CONST:
    {
      // get value, check for error
      uint32_t val = 0;
      const size_t v_len = pt_wasm_decode_u32(&val, src.ptr + 1, src.len - 1);
      if (!v_len) {
        INST_FAIL("bad align value");
      }

      // save value, increment length
      in.v_i32.val = val;
      len += v_len;
    }

    break;
  case PT_WASM_IMM_I64_CONST:
    {
      // get value, check for error
      uint64_t val = 0;
      const size_t v_len = pt_wasm_decode_u64(&val, src.ptr + 1, src.len - 1);
      if (!v_len) {
        INST_FAIL("bad align value");
      }

      // save value, increment length
      in.v_i64.val = val;
      len += v_len;
    }

    break;
  case PT_WASM_IMM_F32_CONST:
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
  case PT_WASM_IMM_F64_CONST:
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

/**
 * Parse constant expr into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_const_expr(
  pt_wasm_expr_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (src_len < 1) {
    FAIL("invalid const expr");
  }

  // build instruction parser context
  const pt_wasm_parse_inst_ctx_t in_ctx = {
    .on_error = cbs->on_error,
    .cb_data  = cb_data,
  };

  size_t depth = 1;
  size_t ofs = 0;
  while ((depth > 0) && (ofs < src_len)) {
    // pack instruction source buffer
    const pt_wasm_buf_t in_src = {
      .ptr = src + ofs,
      .len = src_len - ofs,
    };

    // parse instruction, check for error
    pt_wasm_inst_t in;
    const size_t len = pt_wasm_parse_inst(&in, in_ctx, in_src);
    if (!len) {
      return 0;
    }

    // check op
    if (!pt_wasm_op_is_const(in.op)) {
      D("in.op = %u", in.op);
      FAIL("non-constant instruction in expr");
    }

    // update depth
    depth += pt_wasm_op_is_control(in.op) ? 1 : 0;
    depth -= (in.op == PT_WASM_OP_END) ? 1 : 0;

    // increment offset
    ofs += len;
  }

  // check for error
  if (depth > 0) {
    FAIL("unterminated const expression");
  }

  const pt_wasm_expr_t tmp = {
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
pt_wasm_parse_global_type(
  pt_wasm_global_type_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (src_len < 2) {
    FAIL("incomplete global type");
  }

  // parse value type, check for error
  pt_wasm_value_type_t type;
  const size_t len = pt_wasm_decode_u32(&type, src, src_len);
  if (!len) {
    FAIL("bad global value type");
  }

  // check value type
  if (!pt_wasm_is_valid_value_type(type)) {
    FAIL("bad global value type");
  }

  // check for overflow
  if (len >= src_len) {
    FAIL("missing global mutable flag");
  }

  // get mutable flag, check for error
  const uint8_t mut = src[len];
  if ((mut != 0) && (mut != 1)) {
    FAIL("bad global mutable flag value");
  }

  const pt_wasm_global_type_t tmp = {
    .type = type,
    .mutable = (mut == 1),
  };

  if (dst) {
    // copy to result
    *dst = tmp;
  }

  // return number of bytes consumed
  return len + 1;
}

/**
 * Parse global into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_global(
  pt_wasm_global_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // check source length
  if (src_len < 3) {
    FAIL("incomplete global");
  }

  // parse type, check for error
  pt_wasm_global_type_t type;
  const size_t type_len = pt_wasm_parse_global_type(&type, cbs, src, src_len, cb_data);
  if (!type_len) {
    return 0;
  }

  // parse expr, check for error
  pt_wasm_expr_t expr;
  const size_t expr_len = pt_wasm_parse_const_expr(&expr, cbs, src + type_len, src_len - type_len, cb_data);
  if (!expr_len) {
    return 0;
  }

  const pt_wasm_global_t tmp = {
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
pt_wasm_parse_import(
  pt_wasm_import_t * const dst_import,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  const pt_wasm_buf_t mod_src = {
    .ptr = src,
    .len = src_len,
  };

  // parse module name, check for error
  pt_wasm_buf_t mod;
  const size_t mod_len = pt_wasm_parse_name(&mod, cbs, mod_src, cb_data);
  if (!mod_len) {
    return false;
  }

  const pt_wasm_buf_t name_src = {
    .ptr = mod_src.ptr + mod_len,
    .len = mod_src.len - mod_len,
  };

  // parse name, check for error
  pt_wasm_buf_t name;
  const size_t name_len = pt_wasm_parse_name(&name, cbs, name_src, cb_data);
  if (!name_len) {
    return false;
  }

  // get import type
  const pt_wasm_import_type_t type = src[mod_len + name_len];

  pt_wasm_import_t tmp = {
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
  case PT_WASM_IMPORT_TYPE_FUNC:
    {
      const size_t len = pt_wasm_decode_u32(&(tmp.func.id), data_ptr, data_len);
      if (!len) {
        FAIL("invalid function import type");
      }

      // add length to result
      num_bytes += len;
    }

    break;
  case PT_WASM_IMPORT_TYPE_TABLE:
    {
      // parse table, check for error
      const size_t len = pt_wasm_parse_table(&(tmp.table), cbs, data_ptr, data_len, cb_data);
      if (!len) {
        return false;
      }

      // add length to result
      num_bytes += len;
    }

    break;
  case PT_WASM_IMPORT_TYPE_MEM:
    {
      // parse memory limits, check for error
      const size_t len = pt_wasm_parse_limits(&(tmp.mem.limits), cbs, data_ptr, data_len, cb_data);
      if (!len) {
        return false;
      }

      // add length to result
      num_bytes += len;
    }

    break;
  case PT_WASM_IMPORT_TYPE_GLOBAL:
    {
      // parse global, check for error
      const size_t len = pt_wasm_parse_global_type(&(tmp.global), cbs, data_ptr, data_len, cb_data);
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
  pt_wasm_parse_imports,
  "parse imports",
  pt_wasm_import_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_import,
  on_imports
);

static bool
pt_wasm_parse_import_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_imports(src, cbs, cb_data) > 0;
}

static inline size_t
pt_wasm_function_section_parse_fn(
  uint32_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // parse index, check for error
  const size_t len = pt_wasm_decode_u32(dst, src, src_len);
  if (!len) {
    FAIL("invalid function index");
  }

  // return number of bytes consumed
  return len;
}

DEF_VEC_PARSE_FN(
  pt_wasm_parse_functions,
  "parse tables",
  uint32_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_function_section_parse_fn,
  on_functions
);

static bool
pt_wasm_parse_function_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_functions(src, cbs, cb_data) > 0;
}

DEF_VEC_PARSE_FN(
  pt_wasm_parse_tables,
  "parse tables",
  pt_wasm_table_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_table,
  on_tables
);

static bool
pt_wasm_parse_table_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_tables(src, cbs, cb_data) > 0;
}

DEF_VEC_PARSE_FN(
  pt_wasm_parse_memories,
  "parse memories",
  pt_wasm_limits_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_limits,
  on_memories
);

static bool
pt_wasm_parse_memory_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_memories(src, cbs, cb_data) > 0;
}

DEF_VEC_PARSE_FN(
  pt_wasm_parse_globals,
  "parse globals",
  pt_wasm_global_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_global,
  on_globals
);

static bool
pt_wasm_parse_global_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_globals(src, cbs, cb_data) > 0;
}

/**
 * Parse export into +dst+ from source buffer +src+, consuming a
 * maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_export(
  pt_wasm_export_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  const pt_wasm_buf_t name_src = {
    .ptr = src,
    .len = src_len,
  };

  // parse export name, check for error
  pt_wasm_buf_t name;
  const size_t n_len = pt_wasm_parse_name(&name, cbs, name_src, cb_data);
  if (!n_len) {
    return 0;
  }

  // check src length
  if (n_len + 2 > src_len) {
    FAIL("truncated export");
  }

  // get export type, check for error
  const pt_wasm_export_type_t type = src[n_len];
  if (!pt_wasm_is_valid_export_type(type)) {
    FAIL("bad export type");
  }

  // parse id, check for error
  uint32_t id;
  const size_t id_len = pt_wasm_decode_u32(&id, src + 1 + n_len, src_len - 1 - n_len);
  if (!id_len) {
    FAIL("bad export index");
  }

  // build result
  const pt_wasm_export_t tmp = {
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
  pt_wasm_parse_exports,
  "parse exports",
  pt_wasm_export_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_export,
  on_exports
);

static bool
pt_wasm_parse_export_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_exports(src, cbs, cb_data) > 0;
}

static bool
pt_wasm_parse_start_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // check source length
  if (!src.len) {
    FAIL("empty start section");
  }

  // get id, check for error
  uint32_t id = 0;
  const size_t len = pt_wasm_decode_u32(&id, src.ptr, src.len);
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

/**
 * Parse element into +dst+ from source buffer +src+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_element(
  pt_wasm_element_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // get table_id, check for error
  uint32_t t_id = 0;
  const size_t t_len = pt_wasm_decode_u32(&t_id, src, src_len);
  if (!t_len) {
    FAIL("bad element table id");
  }

  // parse expr, check for error
  pt_wasm_expr_t expr;
  const size_t expr_len = pt_wasm_parse_const_expr(&expr, cbs, src + t_len, src_len - t_len, cb_data);
  if (!expr_len) {
    return 0;
  }

  // build offset
  size_t ofs = t_len + expr_len;

  // get function index count, check for error
  uint32_t num_fns;
  const size_t n_len = pt_wasm_decode_u32(&num_fns, src + ofs, src_len - ofs);
  if (!n_len) {
    FAIL("bad element function index count");
  }

  // increment offset
  ofs += n_len;

  size_t data_len = 0;
  for (size_t i = 0; i < num_fns; i++) {
    uint32_t id = 0;
    const size_t len = pt_wasm_decode_u32(&id, src + ofs, src_len - ofs);
    if (!len) {
      FAIL("bad element function index");
    }

    // increment offset
    data_len += len;
  }

  // build result
  const pt_wasm_element_t tmp = {
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
  pt_wasm_parse_elements,
  "parse elements",
  pt_wasm_element_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_element,
  on_elements
);

static bool
pt_wasm_parse_element_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_elements(src, cbs, cb_data) > 0;
}

/**
 * Parse function code into +dst+ from source buffer +src+, consuming a
 * maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_fn_code(
  pt_wasm_buf_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
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
  const size_t size_len = pt_wasm_decode_u32(&size, src, src_len);
  if (!size_len) {
    FAIL("bad code size");
  }

  // check size
  if (size > src_len - size_len) {
    FAIL("truncated code");
  }

  // populate result
  const pt_wasm_buf_t tmp = {
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
  pt_wasm_parse_codes,
  "parse function codes",
  pt_wasm_buf_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_fn_code,
  on_function_codes
);

static bool
pt_wasm_parse_code_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_codes(src, cbs, cb_data) > 0;
}

static size_t
pt_wasm_parse_data_segment(
  pt_wasm_data_segment_t * const dst,
  const pt_wasm_parse_module_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // get memory index, check for error
  uint32_t id = 0;
  const size_t id_len = pt_wasm_decode_u32(&id, src, src_len);
  if (!id_len) {
    FAIL("bad data section memory index");
  }

  // parse expr, check for error
  pt_wasm_expr_t expr;
  const size_t expr_len = pt_wasm_parse_const_expr(&expr, cbs, src + id_len, src_len - id_len, cb_data);
  if (!expr_len) {
    return 0;
  }

  const size_t data_ofs = id_len + expr_len;
  if (data_ofs >= src_len) {
    FAIL("missing data section data");
  }

  // get size, check for error
  uint32_t size = 0;
  const size_t size_len = pt_wasm_decode_u32(&size, src + data_ofs, src_len - data_ofs);
  if (!size_len) {
    FAIL("bad data section data size");
  }

  // build result
  pt_wasm_data_segment_t tmp = {
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
  pt_wasm_parse_data_segments,
  "parse data segments",
  pt_wasm_data_segment_t,
  pt_wasm_parse_module_cbs_t,
  pt_wasm_parse_data_segment,
  on_data_segments
);

static bool
pt_wasm_parse_data_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  return pt_wasm_parse_data_segments(src, cbs, cb_data) > 0;
}

static bool
pt_wasm_parse_invalid_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  (void) src;
  FAIL("unknown section type");
}

typedef bool (*pt_wasm_parse_section_fn_t)(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
);

#define PT_WASM_SECTION_TYPE(a, b) pt_wasm_parse_ ## b ## _section,
static const pt_wasm_parse_section_fn_t
PT_WASM_SECTION_PARSERS[] = {
PT_WASM_SECTION_TYPES
};
#undef PT_WASM_SECTION_TYPE

static bool
pt_wasm_parse_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_section_type_t sec_type,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  const size_t ofs = MIN(sec_type, PT_WASM_SECTION_TYPE_LAST);
  return PT_WASM_SECTION_PARSERS[ofs](cbs, src, cb_data);
}

static const uint8_t PT_WASM_HEADER[] = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };

bool
pt_wasm_parse_module(
  const void * const src_ptr,
  const size_t src_len,
  const pt_wasm_parse_module_cbs_t * const cbs,
  void * const cb_data
) {
  const uint8_t * const src = src_ptr;

  // check length
  if (src_len < 8) {
    FAIL("module too small");
  }

  // fprintf(stderr," sizeof(WASM_HEADER) = %zu\n", sizeof(WASM_HEADER));
  // check magic and version
  if (memcmp(src, PT_WASM_HEADER, sizeof(PT_WASM_HEADER))) {
    FAIL("invalid module header");
  }

  // bitfield to catch duplicate sections
  uint64_t seen = 0;

  for (size_t ofs = 8; ofs < src_len;) {
    // parse section type, check for error
    const pt_wasm_section_type_t sec_type = src[ofs];
    if (sec_type >= PT_WASM_SECTION_TYPE_LAST) {
      // FIXME: show section type?
      FAIL("invalid section type");
    }

    if (sec_type != PT_WASM_SECTION_TYPE_CUSTOM) {
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
    const size_t len_ofs = pt_wasm_decode_u32(&data_len, u32_ptr, u32_len);
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
    const pt_wasm_buf_t data = {
      .ptr = data_ptr,
      .len = data_len,
    };

    // parse section, check for error
    if (!pt_wasm_parse_section(cbs, sec_type, data, cb_data)) {
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
pt_wasm_parse_expr(
  const pt_wasm_buf_t src,
  const pt_wasm_parse_expr_cbs_t * const cbs,
  void * const cb_data
) {
  // check source length
  if (src.len < 1) {
    FAIL("invalid expr");
  }

  // build instruction parser context
  const pt_wasm_parse_inst_ctx_t in_ctx = {
    .on_error = cbs->on_error,
    .cb_data  = cb_data,
  };

  pt_wasm_inst_t ins[PT_WASM_BATCH_SIZE];

  size_t depth = 1;
  size_t ofs = 0;
  size_t num_ins = 0;
  while ((depth > 0) && (ofs < src.len)) {
    // build output offset
    const size_t ins_ofs = (num_ins & (PT_WASM_BATCH_SIZE - 1));

    const pt_wasm_buf_t in_src = {
      .ptr = src.ptr + ofs,
      .len = src.len - ofs,
    };

    // parse instruction, check for error
    pt_wasm_inst_t * const dst = ins + ins_ofs;
    const size_t len = pt_wasm_parse_inst(dst, in_ctx, in_src);
    if (!len) {
      return 0;
    }

    if ((ins_ofs == (LEN(ins) - 1)) && cbs && cbs->on_insts) {
      cbs->on_insts(ins, ins_ofs, cb_data);
    }

    // update depth
    depth += pt_wasm_op_is_control(dst->op) ? 1 : 0;
    depth -= (dst->op == PT_WASM_OP_END) ? 1 : 0;

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
  const size_t num_left = num_ins & (PT_WASM_BATCH_SIZE - 1);
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
} pt_wasm_get_expr_size_t;

static void
pt_wasm_get_expr_size_on_insts(
  const pt_wasm_inst_t * const insts,
  const size_t num,
  void *cb_data
) {
  pt_wasm_get_expr_size_t * const data = cb_data;
  (void) insts;
  data->num += num;
}

static void
pt_wasm_get_expr_size_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_get_expr_size_t * const data = cb_data;
  (void) text;
  data->success = false;
}

static const pt_wasm_parse_expr_cbs_t
PT_WASM_GET_EXPR_SIZE_CBS = {
  .on_insts = pt_wasm_get_expr_size_on_insts,
  .on_error = pt_wasm_get_expr_size_on_error,
};

bool
pt_wasm_get_expr_size(
  const pt_wasm_buf_t src,
  size_t * const ret_size
) {
  pt_wasm_get_expr_size_t data = {
    .success = true,
    .num = 0,
  };

  // count instructions, ignore result (number of bytes)
  (void) pt_wasm_parse_expr(src, &PT_WASM_GET_EXPR_SIZE_CBS, &data);

  if (data.success && ret_size) {
    // copy size to output
    *ret_size = data.num;
  }

  // return result
  return data.success;
}

static size_t
pt_wasm_parse_local(
  pt_wasm_local_t * const dst,
  const pt_wasm_buf_t src,
  const pt_wasm_parse_function_cbs_t * const cbs,
  void * const cb_data
) {
  if (src.len < 2) {
    FAIL("empty local");
  }

  // parse local count, check for error
  uint32_t num;
  const size_t n_len = pt_wasm_decode_u32(&num, src.ptr, src.len);
  if (!n_len) {
    FAIL("bad local count");
  }

  if (n_len >= src.len) {
    FAIL("missing local type");
  }

  // get type, check for error
  const pt_wasm_value_type_t type = src.ptr[n_len];
  if (!pt_wasm_is_valid_value_type(type)) {
    FAIL("bad local type");
  }

  // populate result
  const pt_wasm_local_t tmp = {
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
pt_wasm_parse_function_locals(
  const pt_wasm_buf_t src,
  const pt_wasm_parse_function_cbs_t * const cbs,
  void * const cb_data
) {
  // get number of locals, check for error
  uint32_t num_locals = 0;
  const size_t num_len = pt_wasm_decode_u32(&num_locals, src.ptr, src.len);
  if (!num_len) {
    FAIL("bad locals count");
  }

  pt_wasm_local_t ls[PT_WASM_BATCH_SIZE];

  size_t ofs = num_len;
  for (size_t i = 0; i < num_locals; i++) {
    const size_t ls_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // build temporary buffer
    const pt_wasm_buf_t tmp = {
      .ptr = src.ptr + ofs,
      .len = src.len - ofs,
    };

    // parse local, check for error
    const size_t len = pt_wasm_parse_local(ls + ls_ofs, tmp, cbs, cb_data);
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
  const size_t num_left = num_locals & (PT_WASM_BATCH_SIZE - 1);
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
pt_wasm_parse_function_expr(
  const pt_wasm_buf_t src,
  const pt_wasm_parse_function_cbs_t * const fn_cbs,
  void * const cb_data
) {
  const pt_wasm_parse_expr_cbs_t expr_cbs = {
    .on_insts = fn_cbs ? fn_cbs->on_insts : NULL,
    .on_error = fn_cbs ? fn_cbs->on_error : NULL,
  };

  return pt_wasm_parse_expr(src, &expr_cbs, cb_data);
}

bool
pt_wasm_parse_function(
  const pt_wasm_buf_t src,
  const pt_wasm_parse_function_cbs_t * const cbs,
  void * const cb_data
) {
  // parse locals, check for error
  const size_t ls_len = pt_wasm_parse_function_locals(src, cbs, cb_data);
  if (!ls_len) {
    return false;
  }

  // build temp expr source
  const pt_wasm_buf_t expr_src = {
    .ptr = src.ptr + ls_len,
    .len = src.len - ls_len,
  };

  // parse expression, check for error
  const size_t expr_len = pt_wasm_parse_function_expr(expr_src, cbs, cb_data);
  if (!expr_len) {
    return false;
  }

  // FIXME: warn about length here?

  // return success
  return true;
}

typedef struct {
  pt_wasm_function_sizes_t sizes;
  bool success;
} pt_wasm_get_function_sizes_t;

static void
pt_wasm_get_function_sizes_on_locals(
  const pt_wasm_local_t * const rows,
  const size_t num,
  void *cb_data
) {
  pt_wasm_get_function_sizes_t *data = cb_data;
  (void) rows;
  data->sizes.num_locals += num;
}

static void
pt_wasm_get_function_sizes_on_insts(
  const pt_wasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pt_wasm_get_function_sizes_t *data = cb_data;
  (void) rows;
  data->sizes.num_insts += num;
}

static void
pt_wasm_get_function_sizes_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_get_function_sizes_t *data = cb_data;
  (void) text;
  data->success = false;
}

static const pt_wasm_parse_function_cbs_t
PT_WASM_GET_FUNCTION_SIZES_CBS = {
  .on_locals = pt_wasm_get_function_sizes_on_locals,
  .on_insts  = pt_wasm_get_function_sizes_on_insts,
  .on_error  = pt_wasm_get_function_sizes_on_error,
};

bool
pt_wasm_get_function_sizes(
  const pt_wasm_buf_t src,
  pt_wasm_function_sizes_t * const ret_sizes
) {
  // build callback data
  pt_wasm_get_function_sizes_t data = {
    .sizes = { 0, 0 },
    .success = true,
  };

  // count sizes
  (void) pt_wasm_parse_function(src, &PT_WASM_GET_FUNCTION_SIZES_CBS, &data);

  if (data.success && ret_sizes) {
    // copy sizes to result
    *ret_sizes = data.sizes;
  }

  // return result
  return data.success;
}

typedef struct {
  pt_wasm_module_sizes_t sizes;
  const pt_wasm_get_module_sizes_cbs_t *cbs;
  void *cb_data;
  bool success;
} pt_wasm_get_module_sizes_t;

static void
pt_wasm_get_module_sizes_on_custom_section(
  const pt_wasm_custom_section_t * const ptr,
  void *cb_data
) {
  pt_wasm_get_module_sizes_t *data = cb_data;
  (void) ptr;
  data->sizes.num_custom_sections++;
}

static void
pt_wasm_get_module_sizes_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_get_module_sizes_t *data = cb_data;
  data->success = false;
  if (data->cbs && data->cbs->on_error) {
    data->cbs->on_error(text, data->cb_data);
  }
}

static void
pt_wasm_get_module_sizes_on_function_types(
  const pt_wasm_function_type_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_function_types += num;

  for (size_t i = 0; i < num; i++) {
    data->sizes.num_function_params += rows[i].params.len;
    data->sizes.num_function_results += rows[i].results.len;
  }
}

static void pt_wasm_get_module_sizes_on_imports(
  const pt_wasm_import_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_imports += num;

  // get number of imports by type
  for (size_t i = 0; i < num; i++) {
    data->sizes.num_import_types[rows[i].type]++;
  }
}

static void
pt_wasm_get_module_sizes_on_globals(
  const pt_wasm_global_t * const globals,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_globals += num;

  for (size_t i = 0; i < num; i++) {
    size_t num_insts = 0;
    if (!pt_wasm_get_expr_size(globals[i].expr.buf, &num_insts)) {
      pt_wasm_get_module_sizes_on_error("get global expr size failed", data);
      return;
    }

    data->sizes.num_global_insts += num_insts;
    data->sizes.num_insts += num_insts;
  }
}

static void
pt_wasm_get_module_sizes_on_function_codes(
  const pt_wasm_buf_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_function_codes += num;
  data->sizes.num_functions += num;

  for (size_t i = 0; i < num; i++) {
    pt_wasm_function_sizes_t sizes;
    if (!pt_wasm_get_function_sizes(rows[i], &sizes)) {
      pt_wasm_get_module_sizes_on_error("get function size failed", data);
      return;
    }

    data->sizes.num_locals += sizes.num_locals;
    data->sizes.num_function_insts += sizes.num_insts;
    data->sizes.num_insts += sizes.num_insts;
  }
}

static void
pt_wasm_get_module_sizes_on_elements(
  const pt_wasm_element_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_elements += num;

  for (size_t i = 0; i < num; i++) {
    data->sizes.num_element_func_ids += rows[i].num_func_ids;

    // count number of instructions in expression
    size_t num_insts = 0;
    if (!pt_wasm_get_expr_size(rows[i].expr.buf, &num_insts)) {
      pt_wasm_get_module_sizes_on_error("get element expr size failed", data);
      return;
    }

    data->sizes.num_element_insts += num_insts;
    data->sizes.num_insts += num_insts;
  }
}

static void
pt_wasm_get_module_sizes_on_data_segments(
  const pt_wasm_data_segment_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_get_module_sizes_t * const data = cb_data;
  data->sizes.num_data_segments += num;

  for (size_t i = 0; i < num; i++) {
    size_t num_insts = 0;
    if (!pt_wasm_get_expr_size(rows[i].expr.buf, &num_insts)) {
      pt_wasm_get_module_sizes_on_error("get data segment expr size failed", data);
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
  SIZE_FN(on_tables, pt_wasm_table_t, num_tables) \
  SIZE_FN(on_memories, pt_wasm_limits_t, num_memories) \
  CUSTOM_SIZE_FN(on_globals) \
  SIZE_FN(on_exports, pt_wasm_export_t, num_exports) \
  CUSTOM_SIZE_FN(on_elements) \
  CUSTOM_SIZE_FN(on_function_codes) \
  CUSTOM_SIZE_FN(on_data_segments)

#define SIZE_FN(fn_name, type, sum) \
  static void pt_wasm_get_module_sizes_ ## fn_name ( \
    const type * const ptr, \
    const size_t num, \
    void * const cb_data \
  ) { \
    pt_wasm_get_module_sizes_t * const data = cb_data; \
    (void) ptr; \
    data->sizes.sum += num; \
  }
#define CUSTOM_SIZE_FN(fn_name)
GET_MOD_SIZES_FNS
#undef SIZE_FN
#undef CUSTOM_SIZE_FN

static const pt_wasm_parse_module_cbs_t
PT_WASM_GET_MOD_SIZES_CBS = {
#define SIZE_FN(fn_name, type, sum) \
  .fn_name = pt_wasm_get_module_sizes_ ## fn_name,
#define CUSTOM_SIZE_FN(fn_name) \
  .fn_name = pt_wasm_get_module_sizes_ ## fn_name,
GET_MOD_SIZES_FNS
#undef CUSTOM_SIZE_FN
#undef SIZE_FN
  .on_error = pt_wasm_get_module_sizes_on_error,
};

_Bool pt_wasm_get_module_sizes(
  pt_wasm_module_sizes_t * const sizes,
  const void * const src,
  const size_t len,
  const pt_wasm_get_module_sizes_cbs_t * const cbs,
  void * const cb_data
) {
  pt_wasm_get_module_sizes_t data = {
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
  if (!pt_wasm_parse_module(src, len, &PT_WASM_GET_MOD_SIZES_CBS, &data)) {
    // return failure
    return false;
  }

  // add import counts to result
  data.sizes.num_functions += data.sizes.num_import_types[PT_WASM_IMPORT_TYPE_FUNC];
  data.sizes.num_tables += data.sizes.num_import_types[PT_WASM_IMPORT_TYPE_TABLE];
  data.sizes.num_memories += data.sizes.num_import_types[PT_WASM_IMPORT_TYPE_MEM];
  data.sizes.num_globals += data.sizes.num_import_types[PT_WASM_IMPORT_TYPE_GLOBAL];

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
pt_wasm_module_alloc(
  pt_wasm_module_t * const ret,
  const pt_wasm_module_sizes_t * const sizes,
  const pt_wasm_module_alloc_cbs_t * const cbs,
  void *cb_data
) {
  if (!cbs || !cbs->on_alloc) {
    // return failure
    return false;
  }

  // calculate total number of bytes needed
  const size_t num_bytes = (
    // custom section handles
    sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +

    // function types
    sizeof(pt_wasm_function_type_t) * sizes->num_function_types +

    // imports
    sizeof(pt_wasm_import_t) * sizes->num_imports +

    // function locals
    sizeof(pt_wasm_local_t) * sizes->num_locals +

    // instructions
    sizeof(pt_wasm_inst_t) * sizes->num_insts +

    // functions
    sizeof(pt_wasm_function_t) * sizes->num_functions +

    // tables
    sizeof(pt_wasm_table_t) * sizes->num_tables +

    // memories
    sizeof(pt_wasm_limits_t) * sizes->num_memories +

    // globals
    sizeof(pt_wasm_module_global_t) * sizes->num_globals +

    // exports
    sizeof(pt_wasm_export_t) * sizes->num_exports +

    // element function IDs
    sizeof(uint32_t) * sizes->num_element_func_ids +

    // elements
    sizeof(pt_wasm_module_element_t) * sizes->num_elements +

    // data segments
    sizeof(pt_wasm_module_data_segment_t) * sizes->num_data_segments +

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
  pt_wasm_module_t mod = {
    // source module and sizes
    .src = sizes->src,
    .sizes = sizes,

    // memory buffer
    .mem = mem,

    // custom sections
    .custom_sections = (pt_wasm_custom_section_t*) mem,
    .num_custom_sections = sizes->num_custom_sections,

    // function types
    .function_types = (pt_wasm_function_type_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections
    )),
    .num_function_types = sizes->num_function_types,

    // imports
    .imports = (pt_wasm_import_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types
    )),
    .num_imports = sizes->num_imports,

    // locals
    .locals = (pt_wasm_local_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports
    )),
    .num_locals = sizes->num_locals,

    // instructions
    .insts = (pt_wasm_inst_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals
    )),
    .num_insts = sizes->num_insts,

    // functions
    .functions = (pt_wasm_function_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts
    )),
    .num_functions = sizes->num_functions,

    // tables
    .tables = (pt_wasm_table_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts +
      sizeof(pt_wasm_function_t) * sizes->num_functions
    )),
    .num_tables = sizes->num_tables,

    // memories
    .memories = (pt_wasm_limits_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts +
      sizeof(pt_wasm_function_t) * sizes->num_functions +
      sizeof(pt_wasm_table_t) * sizes->num_tables
    )),
    .num_memories = sizes->num_memories,

    // globals
    .globals = (pt_wasm_module_global_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts +
      sizeof(pt_wasm_function_t) * sizes->num_functions +
      sizeof(pt_wasm_table_t) * sizes->num_tables +
      sizeof(pt_wasm_limits_t) * sizes->num_memories
    )),
    .num_globals = sizes->num_globals,

    // exports
    .exports = (pt_wasm_export_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts +
      sizeof(pt_wasm_function_t) * sizes->num_functions +
      sizeof(pt_wasm_table_t) * sizes->num_tables +
      sizeof(pt_wasm_limits_t) * sizes->num_memories +
      sizeof(pt_wasm_module_global_t) * sizes->num_globals
    )),
    .num_exports = sizes->num_exports,

    // element function IDs
    .element_func_ids = (uint32_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts +
      sizeof(pt_wasm_function_t) * sizes->num_functions +
      sizeof(pt_wasm_table_t) * sizes->num_tables +
      sizeof(pt_wasm_limits_t) * sizes->num_memories +
      sizeof(pt_wasm_module_global_t) * sizes->num_globals +
      sizeof(pt_wasm_export_t) * sizes->num_exports
    )),
    .num_element_func_ids = sizes->num_element_func_ids,

    // elements
    .elements = (pt_wasm_module_element_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts +
      sizeof(pt_wasm_function_t) * sizes->num_functions +
      sizeof(pt_wasm_table_t) * sizes->num_tables +
      sizeof(pt_wasm_limits_t) * sizes->num_memories +
      sizeof(pt_wasm_module_global_t) * sizes->num_globals +
      sizeof(pt_wasm_export_t) * sizes->num_exports +
      sizeof(uint32_t) * sizes->num_element_func_ids
    )),
    .num_elements = sizes->num_elements,

    // data segments
    .data_segments = (pt_wasm_module_data_segment_t*) (mem + (
      sizeof(pt_wasm_custom_section_t) * sizes->num_custom_sections +
      sizeof(pt_wasm_function_type_t) * sizes->num_function_types +
      sizeof(pt_wasm_import_t) * sizes->num_imports +
      sizeof(pt_wasm_local_t) * sizes->num_locals +
      sizeof(pt_wasm_inst_t) * sizes->num_insts +
      sizeof(pt_wasm_function_t) * sizes->num_functions +
      sizeof(pt_wasm_table_t) * sizes->num_tables +
      sizeof(pt_wasm_limits_t) * sizes->num_memories +
      sizeof(pt_wasm_module_global_t) * sizes->num_globals +
      sizeof(pt_wasm_export_t) * sizes->num_exports +
      sizeof(uint32_t) * sizes->num_element_func_ids +
      sizeof(pt_wasm_module_element_t) * sizes->num_elements
    )),
    .num_data_segments = sizes->num_data_segments,
  };

  if (ret) {
    // copy module to output
    memcpy(ret, &mod, sizeof(pt_wasm_module_t));
  }

  // return success
  return true;
}

typedef struct {
  pt_wasm_module_t *mod;
  pt_wasm_module_sizes_t sizes;
  const pt_wasm_module_init_cbs_t *cbs;
  void *cb_data;
  bool success;
} pt_wasm_module_init_t;

static void
pt_wasm_module_init_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_module_init_t *data = cb_data;
  data->success = false;
  if (data->cbs && data->cbs->on_error) {
    data->cbs->on_error(text, data->cb_data);
  }
}

static void
pt_wasm_module_init_on_custom_section(
  const pt_wasm_custom_section_t * const ptr,
  void *cb_data
) {
  pt_wasm_module_init_t *data = cb_data;
  data->mod->custom_sections[data->sizes.num_custom_sections++] = *ptr;
}

static void
pt_wasm_module_init_on_function_types(
  const pt_wasm_function_type_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_function_type_t * const dst = data->mod->function_types + data->sizes.num_function_types;
  const size_t num_bytes = sizeof(pt_wasm_function_type_t) * num;

  memcpy(dst, rows, num_bytes);
  data->sizes.num_function_types += num;
}

static void
pt_wasm_module_init_on_imports(
  const pt_wasm_import_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pt_wasm_import_t * const dst = mod->imports + data->sizes.num_imports;
  const size_t num_bytes = sizeof(pt_wasm_import_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_imports += num;

  // populate imports
  for (size_t i = 0; i < num; i++) {
    switch (rows[i].type) {
    case PT_WASM_IMPORT_TYPE_FUNC:
      mod->functions[data->sizes.num_functions].source = PT_WASM_SOURCE_IMPORT;
      mod->functions[data->sizes.num_functions].type_id = rows[i].func.id;
      data->sizes.num_functions++;
      break;
    case PT_WASM_IMPORT_TYPE_TABLE:
      mod->tables[data->sizes.num_tables++] = rows[i].table;
      data->sizes.num_tables++;
      break;
    case PT_WASM_IMPORT_TYPE_MEM:
      mod->memories[data->sizes.num_memories++] = rows[i].mem.limits;
      data->sizes.num_memories++;
      break;
    case PT_WASM_IMPORT_TYPE_GLOBAL:
      mod->globals[data->sizes.num_globals].source = PT_WASM_SOURCE_IMPORT;
      mod->globals[data->sizes.num_globals].type = rows[i].global;
      data->sizes.num_globals++;
      break;
    default:
      // never reached
      pt_wasm_module_init_on_error("unknown import type", cb_data);
      return;
    }
  }
}

static void
pt_wasm_module_init_on_functions(
  const uint32_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;

  // populate internal function types
  for (size_t i = 0; i < num; i++) {
    const size_t ofs = data->mod->sizes->num_functions + i;
    data->mod->functions[ofs].source = PT_WASM_SOURCE_MODULE;
    data->mod->functions[ofs].type_id = rows[i];
  }

  // increment functions size
  data->sizes.num_functions += num;
}

static void
pt_wasm_module_init_on_tables(
  const pt_wasm_table_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pt_wasm_table_t * const dst = mod->tables + data->sizes.num_tables;
  const size_t num_bytes = sizeof(pt_wasm_table_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_tables += num;
}

static void
pt_wasm_module_init_on_memories(
  const pt_wasm_limits_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pt_wasm_limits_t * const dst = mod->memories + data->sizes.num_memories;
  const size_t num_bytes = sizeof(pt_wasm_limits_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_memories += num;
}

typedef struct {
  pt_wasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pt_wasm_module_init_on_globals_t;

static void
pt_wasm_module_init_on_globals_on_insts(
  const pt_wasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pt_wasm_module_init_on_globals_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  if (!data->success) {
    return;
  }

  // calculate destination and number of bytes
  pt_wasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pt_wasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  // update expr slice length
  init_data->mod->globals[data->ofs].expr.len += num;
}

static void
pt_wasm_module_init_on_globals_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_module_init_on_globals_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pt_wasm_module_init_on_error(text, init_data);
}

static const pt_wasm_parse_expr_cbs_t
PT_WASM_MODULE_INIT_ON_GLOBALS_CBS = {
  .on_insts = pt_wasm_module_init_on_globals_on_insts,
  .on_error = pt_wasm_module_init_on_globals_on_error,
};

static void
pt_wasm_module_init_on_globals(
  const pt_wasm_global_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_module_t * const mod = data->mod;

  for (size_t i = 0; i < num; i++) {
    const size_t ofs = data->mod->sizes->num_globals + i;
    mod->globals[ofs].source = PT_WASM_SOURCE_MODULE;
    mod->globals[ofs].type = rows[i].type;
    mod->globals[ofs].expr.ofs = data->sizes.num_insts;
    mod->globals[ofs].expr.len = 0;

    // build expr parsing callback data
    pt_wasm_module_init_on_globals_t expr_data = {
      .init_data  = data,
      .ofs        = ofs,
      .success    = true,
    };

    // parse expression
    // FIXME: should limit to constant expr here
    (void) pt_wasm_parse_expr(
      rows[i].expr.buf,
      &PT_WASM_MODULE_INIT_ON_GLOBALS_CBS,
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
pt_wasm_module_init_on_exports(
  const pt_wasm_export_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_module_t * const mod = data->mod;

  // get destination and number of bytes
  pt_wasm_export_t * const dst = mod->exports + data->sizes.num_exports;
  const size_t num_bytes = sizeof(pt_wasm_export_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  data->sizes.num_exports += num;
}

static void
pt_wasm_module_init_on_start(
  const uint32_t start,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  data->mod->has_start = true;
  data->mod->start = start;
}

typedef struct {
  pt_wasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pt_wasm_module_add_element_expr_t;

static void
pt_wasm_module_add_element_expr_on_insts(
  const pt_wasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pt_wasm_module_add_element_expr_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  if (!data->success) {
    return;
  }

  // calculate destination and number of bytes
  pt_wasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pt_wasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  // update expr slice length
  init_data->mod->elements[data->ofs].expr.len += num;
}

static void
pt_wasm_module_add_element_expr_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_module_add_element_expr_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pt_wasm_module_init_on_error(text, init_data);
}

static const pt_wasm_parse_expr_cbs_t
PT_WASM_MODULE_ADD_ELEMENT_EXPR_CBS = {
  .on_insts = pt_wasm_module_add_element_expr_on_insts,
  .on_error = pt_wasm_module_add_element_expr_on_error,
};

static inline bool
pt_wasm_module_add_element_expr(
  pt_wasm_module_t * const mod,
  pt_wasm_module_init_t * const init_data,
  const size_t ofs,
  const pt_wasm_buf_t src
) {
  // populate expression slice offset and length
  mod->elements[ofs].expr.ofs = init_data->sizes.num_insts;
  mod->elements[ofs].expr.len = 0;

  // build expr parsing callback data
  pt_wasm_module_add_element_expr_t expr_data = {
    .init_data  = init_data,
    .ofs        = ofs,
    .success    = true,
  };

  // parse expression
  // FIXME: should limit to constant expr here
  (void) pt_wasm_parse_expr(
    src,
    &PT_WASM_MODULE_ADD_ELEMENT_EXPR_CBS,
    &expr_data
  );

  return expr_data.success;
}

typedef struct {
  pt_wasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pt_wasm_module_add_element_fns_t;

static void
pt_wasm_module_add_element_fns_on_vals(
  const uint32_t * const ids,
  const size_t num,
  void *cb_data
) {
  pt_wasm_module_add_element_fns_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

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
pt_wasm_module_add_element_fns_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_module_add_element_fns_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pt_wasm_module_init_on_error(text, init_data);
}

static const pt_wasm_parse_u32s_cbs_t
PT_WASM_MODULE_ADD_ELEMENT_FNS_CBS = {
  .on_vals  = pt_wasm_module_add_element_fns_on_vals,
  .on_error = pt_wasm_module_add_element_fns_on_error,
};

static inline bool
pt_wasm_module_add_element_fns(
  pt_wasm_module_t * const mod,
  pt_wasm_module_init_t * const init_data,
  const size_t ofs,
  const pt_wasm_buf_t src
) {
  // populate function id slice offset and length
  mod->elements[ofs].func_ids.ofs = init_data->sizes.num_element_func_ids;
  mod->elements[ofs].func_ids.len = 0;

  // build function id parsing callback data
  pt_wasm_module_add_element_fns_t fns_data = {
    .init_data  = init_data,
    .ofs        = ofs,
    .success    = true,
  };

  // parse function IDs
  pt_wasm_parse_u32s(src, &PT_WASM_MODULE_ADD_ELEMENT_FNS_CBS, &fns_data);

  // return result
  return fns_data.success;
}

static void
pt_wasm_module_init_on_elements(
  const pt_wasm_element_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_module_t * const mod = data->mod;

  for (size_t i = 0; i < num; i++) {
    // get offset
    const size_t ofs = data->sizes.num_elements + i;

    // set table ID
    mod->elements[ofs].table_id = rows[i].table_id;

    // add element expression, check for error
    if (!pt_wasm_module_add_element_expr(mod, data, ofs, rows[i].expr.buf)) {
      // add failed, exit
      return;
    }

    // add element function IDs, check for error
    if (!pt_wasm_module_add_element_fns(mod, data, ofs, rows[i].func_ids)) {
      // add failed, exit
      return;
    }
  }

  // increment number of elements
  data->sizes.num_elements += num;
}

typedef struct {
  pt_wasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pt_wasm_module_add_segment_expr_t;

static void
pt_wasm_module_add_segment_expr_on_insts(
  const pt_wasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pt_wasm_module_add_segment_expr_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  if (!data->success) {
    return;
  }

  // calculate destination and number of bytes
  pt_wasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pt_wasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  // update expr slice length
  init_data->mod->data_segments[data->ofs].expr.len += num;
}

static void
pt_wasm_module_add_segment_expr_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_module_add_segment_expr_t * const data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  data->success = false;
  pt_wasm_module_init_on_error(text, init_data);
}

static const pt_wasm_parse_expr_cbs_t
PT_WASM_MODULE_ADD_SEGMENT_EXPR_CBS = {
  .on_insts = pt_wasm_module_add_segment_expr_on_insts,
  .on_error = pt_wasm_module_add_segment_expr_on_error,
};

static inline bool
pt_wasm_module_add_segment_expr(
  pt_wasm_module_t * const mod,
  pt_wasm_module_init_t * const init_data,
  const size_t ofs,
  const pt_wasm_buf_t src
) {
  // populate expression slice offset and length
  mod->data_segments[ofs].expr.ofs = init_data->sizes.num_insts;
  mod->data_segments[ofs].expr.len = 0;

  // build expr parsing callback data
  pt_wasm_module_add_segment_expr_t expr_data = {
    .init_data  = init_data,
    .ofs        = ofs,
    .success    = true,
  };

  // parse expression
  // FIXME: should limit to constant expr here
  (void) pt_wasm_parse_expr(
    src,
    &PT_WASM_MODULE_ADD_SEGMENT_EXPR_CBS,
    &expr_data
  );

  return expr_data.success;
}


static void
pt_wasm_module_init_on_data_segments(
  const pt_wasm_data_segment_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  pt_wasm_module_t * const mod = data->mod;

  for (size_t i = 0; i < num; i++) {
    // get offset
    const size_t ofs = data->sizes.num_data_segments + i;

    // set memory ID
    mod->data_segments[ofs].mem_id = rows[i].mem_id;
    mod->data_segments[ofs].data = rows[i].data;

    // add expression, check for error
    if (!pt_wasm_module_add_segment_expr(mod, data, ofs, rows[i].expr.buf)) {
      // add failed, exit
      return;
    }
  }

  // increment count
  data->sizes.num_data_segments += num;
}

typedef struct {
  pt_wasm_module_init_t * const init_data;
  const size_t ofs;
  bool success;
} pt_wasm_module_add_code_t;

static void
pt_wasm_module_add_code_on_locals(
  const pt_wasm_local_t * const rows,
  const size_t num,
  void *cb_data
) {
  pt_wasm_module_add_code_t *data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  // calculate destination and number of bytes
  pt_wasm_local_t * const dst = init_data->mod->locals + init_data->sizes.num_locals;
  const size_t num_bytes = sizeof(pt_wasm_local_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_locals += num;

  // update function locals slice length
  init_data->mod->functions[data->ofs].locals.len += num;
}

static void
pt_wasm_module_add_code_on_insts(
  const pt_wasm_inst_t * const rows,
  const size_t num,
  void *cb_data
) {
  pt_wasm_module_add_code_t *data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;

  // calculate destination and number of bytes
  pt_wasm_inst_t * const dst = init_data->mod->insts + init_data->sizes.num_insts;
  const size_t num_bytes = sizeof(pt_wasm_inst_t) * num;

  // copy data, increment offset
  memcpy(dst, rows, num_bytes);
  init_data->sizes.num_insts += num;

  // update function insts slice length
  init_data->mod->functions[data->ofs].insts.len += num;
}

static void
pt_wasm_module_add_code_on_error(
  const char * const text,
  void *cb_data
) {
  pt_wasm_module_add_code_t *data = cb_data;
  pt_wasm_module_init_t * const init_data = data->init_data;
  data->success = false;
  pt_wasm_module_init_on_error(text, init_data);
}

static const pt_wasm_parse_function_cbs_t
PT_WASM_MODULE_ADD_CODE_CBS = {
  .on_locals = pt_wasm_module_add_code_on_locals,
  .on_insts  = pt_wasm_module_add_code_on_insts,
  .on_error  = pt_wasm_module_add_code_on_error,
};

bool
pt_wasm_module_add_code(
  pt_wasm_module_init_t * const init_data,
  const size_t ofs,
  const pt_wasm_buf_t src
) {
  // build callback data
  pt_wasm_module_add_code_t data = {
    .init_data = init_data,
    .ofs = ofs,
    .success = true,
  };

  // add code
  pt_wasm_parse_function(src, &PT_WASM_MODULE_ADD_CODE_CBS, &data);

  // return result
  return data.success;
}

static void
pt_wasm_module_init_on_function_codes(
  const pt_wasm_buf_t * const rows,
  const size_t num,
  void * const cb_data
) {
  pt_wasm_module_init_t * const data = cb_data;
  const size_t num_imports = data->sizes.num_import_types[PT_WASM_IMPORT_TYPE_FUNC];

  for (size_t i = 0; i < num; i++) {
    // get offset
    const size_t ofs = num_imports + data->sizes.num_function_codes + i;

    // add function code, check for error
    if (!pt_wasm_module_add_code(data, ofs, rows[i])) {
      return;
    }
  }

  // increment function codes
  data->sizes.num_function_codes += num;
}

static const pt_wasm_parse_module_cbs_t
PT_WASM_MOD_INIT_CBS = {
  .on_error           = pt_wasm_module_init_on_error,
  .on_custom_section  = pt_wasm_module_init_on_custom_section,
  .on_function_types  = pt_wasm_module_init_on_function_types,
  .on_imports         = pt_wasm_module_init_on_imports,
  .on_functions       = pt_wasm_module_init_on_functions,
  .on_tables          = pt_wasm_module_init_on_tables,
  .on_memories        = pt_wasm_module_init_on_memories,
  .on_globals         = pt_wasm_module_init_on_globals,
  .on_exports         = pt_wasm_module_init_on_exports,
  .on_start           = pt_wasm_module_init_on_start,
  .on_elements        = pt_wasm_module_init_on_elements,
  .on_data_segments   = pt_wasm_module_init_on_data_segments,
  .on_function_codes  = pt_wasm_module_init_on_function_codes,
};

bool
pt_wasm_module_init(
  pt_wasm_module_t * const mod,
  const pt_wasm_module_init_cbs_t * const cbs,
  void *cb_data
) {
  mod->has_start = false;
  pt_wasm_module_init_t data = {
    .mod = mod,
    .sizes = {},
    .success = true,
    .cbs = cbs,
    .cb_data = cb_data,
  };

  // parse module, check for error
  if (!pt_wasm_parse_module(mod->src.ptr, mod->src.len, &PT_WASM_MOD_INIT_CBS, &data)) {
    // return failure
    return false;
  }

  // return success
  return true;
}
