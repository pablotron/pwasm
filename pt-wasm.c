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
    FAIL("empty custom section name");
  }

  // decode name length, check for error
  uint32_t len = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&len, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("bad custom section name length");
  }

  // D("src: %p, src_len = %zu, len = %u, len_ofs = %zu", src, src_len, len, len_ofs);

  // calculate total length, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src.len) {
    FAIL("truncated custom section name");
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

static bool
pt_wasm_parse_type_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // get number of types, check for error
  uint32_t num_types = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_types, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("invalid type section vector length");
  }

  pt_wasm_function_type_t types[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_types; i++) {
    const size_t types_ofs = (i & (PT_WASM_BATCH_SIZE - 1));
    // parse function type, check for error
    const size_t type_len = pt_wasm_parse_function_type(
      types + types_ofs,
      cbs,
      src.ptr + ofs,
      src.len - ofs,
      cb_data
    );

    if (!type_len) {
      // return failure
      return false;
    }

    // increment offset, check for error
    ofs += type_len;
    if (ofs > src.len) {
      FAIL("type section length overflow");
    }

    if ((types_ofs == (LEN(types) - 1)) && cbs && cbs->on_function_types) {
      cbs->on_function_types(types, types_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_types & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_function_types) {
    // flush remaining entries
    cbs->on_function_types(types, num_left, cb_data);
  }

  // return success
  return true;
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

typedef enum {
  PT_WASM_IMM_NONE,
  PT_WASM_IMM_BLOCK,
  PT_WASM_IMM_BR_TABLE,
  PT_WASM_IMM_INDEX,
  PT_WASM_IMM_CALL_INDIRECT,
  PT_WASM_IMM_MEM,
  PT_WASM_IMM_I32_CONST,
  PT_WASM_IMM_I64_CONST,
  PT_WASM_IMM_F32_CONST,
  PT_WASM_IMM_F64_CONST,
  PT_WASM_IMM_LAST,
} pt_wasm_imm_t;

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
    FAIL("invalid expr");
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
    FAIL("unterminated expression");
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

static bool
pt_wasm_parse_import_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // get number of imports, check for error
  uint32_t num_imports = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_imports, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("invalid import section vector length");
  }

  pt_wasm_import_t imports[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_imports; i++) {
    const size_t imports_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse import, check for error
    const size_t import_len = pt_wasm_parse_import(
      imports + imports_ofs,
      cbs,
      src.ptr + ofs,
      src.len - ofs,
      cb_data
    );

    if (!import_len) {
      // return failure
      return false;
    }

    // increment offset, check for error
    ofs += import_len;
    if (ofs > src.len) {
      FAIL("import section length overflow");
    }

    if ((imports_ofs == (LEN(imports) - 1)) && cbs && cbs->on_imports) {
      // flush batch
      cbs->on_imports(imports, imports_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_imports & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_imports) {
    // flush remaining entries
    cbs->on_imports(imports, num_left, cb_data);
  }

  // return success
  return true;
}

static bool
pt_wasm_parse_function_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // get number of fns, check for error
  uint32_t num_fns = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_fns, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("invalid function section vector length");
  }

  // D("num_fns = %u", num_fns);

  uint32_t fns[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_fns; i++) {
    const size_t fns_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse fn, check for error
    const size_t fn_len = pt_wasm_decode_u32(fns + fns_ofs, src.ptr + ofs, src.len - ofs);
    if (!fn_len) {
      FAIL("invalid function index");
    }

    // D("fns[%zu] = %u", fns_ofs, fns[fns_ofs]);

    // increment offset, check for error
    ofs += fn_len;
    if (ofs > src.len) {
      FAIL("function section length overflow");
    }

    if ((fns_ofs == (LEN(fns) - 1)) && cbs && cbs->on_functions) {
      // flush batch
      cbs->on_functions(fns, fns_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_fns & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_functions) {
    // flush remaining entries
    cbs->on_functions(fns, num_left, cb_data);
  }

  // return success
  return true;
}

static bool
pt_wasm_parse_table_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // get number of tables, check for error
  uint32_t num_tbls = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_tbls, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("invalid table section vector length");
  }

  // D("num_tbls = %u", num_tbls);

  pt_wasm_table_t tbls[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_tbls; i++) {
    const size_t tbls_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse fn, check for error
    const size_t tbl_len = pt_wasm_parse_table(tbls + tbls_ofs, cbs, src.ptr + ofs, src.len - ofs, cb_data);
    if (!tbl_len) {
      FAIL("invalid table section entry");
    }

    // D("tbls[%zu] = [%u, %u], has_max: %c", tbls_ofs, tbls[tbls_ofs].limits.min, tbls[tbls_ofs].limits.max, tbls[tbls_ofs].limits.has_max ? 't' : 'f');

    // increment offset, check for error
    ofs += tbl_len;
    if (ofs > src.len) {
      FAIL("table section length overflow");
    }

    if ((tbls_ofs == (LEN(tbls) - 1)) && cbs && cbs->on_tables) {
      // flush batch
      cbs->on_tables(tbls, tbls_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_tbls & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_tables) {
    // flush remaining entries
    cbs->on_tables(tbls, num_left, cb_data);
  }

  // return success
  return true;
}

static bool
pt_wasm_parse_memory_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // get number of mems, check for error
  uint32_t num_mems = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_mems, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("invalid memory section vector length");
  }

  pt_wasm_limits_t mems[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_mems; i++) {
    const size_t mems_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse mem, check for error
    const size_t mem_len = pt_wasm_parse_limits(mems + mems_ofs, cbs, src.ptr + ofs, src.len - ofs, cb_data);
    if (!mem_len) {
      return false;
    }

    // increment offset, check for error
    ofs += mem_len;
    if (ofs > src.len) {
      FAIL("memory section length overflow");
    }

    if ((mems_ofs == (LEN(mems) - 1)) && cbs && cbs->on_memories) {
      // flush batch
      cbs->on_memories(mems, mems_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_mems & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_memories) {
    // flush remaining entries
    cbs->on_memories(mems, num_left, cb_data);
  }

  // return success
  return true;
}

static bool
pt_wasm_parse_global_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // get number of mems, check for error
  uint32_t num_gs = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_gs, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("invalid global section vector length");
  }

  pt_wasm_global_t gs[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_gs; i++) {
    const size_t gs_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse mem, check for error
    const size_t g_len = pt_wasm_parse_global(gs + gs_ofs, cbs, src.ptr + ofs, src.len - ofs, cb_data);
    if (!g_len) {
      return false;
    }

    // increment offset, check for error
    ofs += g_len;
    if (ofs > src.len) {
      FAIL("global section length overflow");
    }

    if ((gs_ofs == (LEN(gs) - 1)) && cbs && cbs->on_globals) {
      // flush batch
      cbs->on_globals(gs, gs_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_gs & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_globals) {
    // flush remaining entries
    cbs->on_globals(gs, num_left, cb_data);
  }

  // return success
  return true;
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

static bool
pt_wasm_parse_export_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // get number of exports, check for error
  uint32_t num_es = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_es, src.ptr, src.len);
  if (!len_ofs) {
    FAIL("invalid export section vector length");
  }

  pt_wasm_export_t es[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_es; i++) {
    const size_t es_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse mem, check for error
    const size_t e_len = pt_wasm_parse_export(es + es_ofs, cbs, src.ptr + ofs, src.len - ofs, cb_data);
    if (!e_len) {
      return false;
    }

    // increment offset, check for error
    ofs += e_len;
    if (ofs > src.len) {
      FAIL("export section length overflow");
    }

    if ((es_ofs == (LEN(es) - 1)) && cbs && cbs->on_exports) {
      // flush batch
      cbs->on_exports(es, es_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_es & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_exports) {
    // flush remaining entries
    cbs->on_exports(es, num_left, cb_data);
  }

  // return success
  return true;
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

static bool
pt_wasm_parse_element_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // check source length
  if (!src.len) {
    FAIL("empty element section");
  }

  // get entry count, check for error
  uint32_t num_es = 0;
  const size_t num_len = pt_wasm_decode_u32(&num_es, src.ptr, src.len);
  if (!num_len) {
    FAIL("bad element section entry count");
  }

  // batch export buffer
  pt_wasm_element_t es[PT_WASM_BATCH_SIZE];

  size_t ofs = num_len;
  for (size_t i = 0; i < num_es; i++) {
    const size_t es_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse element, check for error
    const size_t len = pt_wasm_parse_element(es + es_ofs, cbs, src.ptr + ofs, src.len - ofs, cb_data);
    if (!len) {
      return false;
    }

    // increment offset, check for error
    ofs += len;
    if (ofs > src.len) {
      FAIL("element section length overflow");
    }

    if ((es_ofs == (LEN(es) - 1)) && cbs && cbs->on_elements) {
      // flush batch
      cbs->on_elements(es, es_ofs, cb_data);
    }
  }

  // FIXME: check offset?

  // count remaining entries
  const size_t num_left = num_es & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_elements) {
    // flush remaining entries
    cbs->on_elements(es, num_left, cb_data);
  }

  // return success
  return true;
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

static bool
pt_wasm_parse_code_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // check source length
  if (!src.len) {
    FAIL("empty code section");
  }

  // get entry count, check for error
  uint32_t num_fns = 0;
  const size_t num_len = pt_wasm_decode_u32(&num_fns, src.ptr, src.len);
  if (!num_len) {
    FAIL("bad code section function count");
  }

  // batch function buffer
  pt_wasm_buf_t fns[PT_WASM_BATCH_SIZE];

  size_t ofs = num_len;
  for (size_t i = 0; i < num_fns; i++) {
    const size_t fns_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse entry, check for error
    const size_t len = pt_wasm_parse_fn_code(fns + fns_ofs, cbs, src.ptr + ofs, src.len - ofs, cb_data);
    if (!len) {
      return false;
    }

    // increment offset, check for error
    ofs += len;
    if (ofs > src.len) {
      FAIL("code section length overflow");
    }

    if ((fns_ofs == (LEN(fns) - 1)) && cbs && cbs->on_function_codes) {
      // flush batch
      cbs->on_function_codes(fns, fns_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_fns & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_function_codes) {
    // flush remaining functions
    cbs->on_function_codes(fns, num_left, cb_data);
  }

  // return success
  return true;
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

static bool
pt_wasm_parse_data_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  // check source length
  if (!src.len) {
    FAIL("empty data section");
  }

  // get entry count, check for error
  uint32_t num_ds = 0;
  const size_t num_len = pt_wasm_decode_u32(&num_ds, src.ptr, src.len);
  if (!num_len) {
    FAIL("bad data section segment count");
  }

  // batch function buffer
  pt_wasm_data_segment_t ds[PT_WASM_BATCH_SIZE];

  size_t ofs = num_len;
  for (size_t i = 0; i < num_ds; i++) {
    const size_t ds_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse entry, check for error
    const size_t len = pt_wasm_parse_data_segment(ds + ds_ofs, cbs, src.ptr + ofs, src.len - ofs, cb_data);
    if (!len) {
      return false;
    }

    // increment offset, check for error
    ofs += len;
    if (ofs > src.len) {
      FAIL("code section length overflow");
    }

    if ((ds_ofs == (LEN(ds) - 1)) && cbs && cbs->on_data_segments) {
      // flush batch
      cbs->on_data_segments(ds, ds_ofs, cb_data);
    }
  }

  // count remaining entries
  const size_t num_left = num_ds & (PT_WASM_BATCH_SIZE - 1);
  if (num_left && cbs && cbs->on_data_segments) {
    // flush remaining functions
    cbs->on_data_segments(ds, num_left, cb_data);
  }

  // return success
  return true;
}

static bool
pt_wasm_parse_section(
  const pt_wasm_parse_module_cbs_t * const cbs,
  const pt_wasm_section_type_t sec_type,
  const pt_wasm_buf_t src,
  void * const cb_data
) {
  switch (sec_type) {
  case PT_WASM_SECTION_TYPE_CUSTOM:
    return pt_wasm_parse_custom_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_TYPE:
    return pt_wasm_parse_type_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_IMPORT:
    return pt_wasm_parse_import_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_FUNCTION:
    return pt_wasm_parse_function_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_TABLE:
    return pt_wasm_parse_table_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_MEMORY:
    return pt_wasm_parse_memory_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_GLOBAL:
    return pt_wasm_parse_global_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_EXPORT:
    return pt_wasm_parse_export_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_START:
    return pt_wasm_parse_start_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_ELEMENT:
    return pt_wasm_parse_element_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_CODE:
    return pt_wasm_parse_code_section(cbs, src, cb_data);
  case PT_WASM_SECTION_TYPE_DATA:
    return pt_wasm_parse_data_section(cbs, src, cb_data);
  default:
    FAIL("unknown section type");
    break;
  }

  // return success
  return true;
}

static const uint8_t WASM_HEADER[] = { 0, 0x61, 0x73, 0x6d, 1, 0, 0, 0 };

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
  if (memcmp(src, WASM_HEADER, sizeof(WASM_HEADER))) {
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
  const pt_wasm_parse_function_cbs_t * const cbs,
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
    pt_wasm_inst_t in;
    const size_t len = pt_wasm_parse_inst(ins + ins_ofs, in_ctx, in_src);
    if (!len) {
      return 0;
    }

    if ((ins_ofs == (LEN(ins) - 1)) && cbs && cbs->on_insts) {
      cbs->on_insts(ins, ins_ofs, cb_data);
    }

    // update depth
    depth += pt_wasm_op_is_control(in.op) ? 1 : 0;
    depth -= (in.op == PT_WASM_OP_END) ? 1 : 0;

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
