#include <stdbool.h> // bool
#include <stdint.h>  // uint32_t, int32_t, etc
#include <string.h> // memcmp()
#include "pt-wasm.h"

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define LEN(ary) (sizeof(ary) / sizeof((ary)[0]))

/**
 * Batch size.
 *
 * Used to batch up function types, imports, functions,
 * etc, when dispatching to parsing callbacks.
 *
 * Note: must be a power of two.
 */
#define PT_WASM_BATCH_SIZE 128

// FIXME: limit to DEBUG
#include <stdio.h>
#define D(fmt, ...) fprintf( \
  stderr, \
  "D %s:%d %s(): " fmt "\n", \
  __FILE__, __LINE__, __func__, __VA_ARGS__ \
)

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

#define PT_WASM_IMPORT_DESC(a, b) b,
static const char *PT_WASM_IMPORT_DESC_NAMES[] = {
PT_WASM_IMPORT_DESCS
};
#undef PT_WASM_IMPORT_DESC

const char *
pt_wasm_import_desc_get_name(
  const pt_wasm_import_desc_t v
) {
  const size_t ofs = MIN(PT_WASM_IMPORT_DESC_LAST, v);
  return PT_WASM_IMPORT_DESC_NAMES[ofs];
}

static const char *PT_WASM_VALUE_TYPE_NAMES[] = {
  "i32",
  "i64",
  "f32",
  "f64",
  "unknown type",
};

const char *
pt_wasm_value_type_get_name(
  const pt_wasm_value_type_t v
) {
  const size_t ofs = MIN(0x7F - v, LEN(PT_WASM_VALUE_TYPE_NAMES));
  return PT_WASM_VALUE_TYPE_NAMES[ofs];
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

#define SIZE_FAIL(msg) do { \
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
    SIZE_FAIL("empty custom section name");
  }

  // decode name length, check for error
  uint32_t len = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&len, src, src_len);
  if (!len_ofs) {
    SIZE_FAIL("bad custom section name length");
  }

  // D("src: %p, src_len = %zu, len = %u, len_ofs = %zu", src, src_len, len, len_ofs);

  // calculate total length, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src_len) {
    SIZE_FAIL("truncated custom section name");
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
    SIZE_FAIL("empty value type list");
  }

  // decode buffer length, check for error
  uint32_t len = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&len, src, src_len);
  if (!len_ofs) {
    SIZE_FAIL("bad value type list length");
  }

  // calculate total number of bytes, check for error
  const size_t num_bytes = len_ofs + len;
  if (num_bytes > src_len) {
    SIZE_FAIL("value type list length too long");
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
      SIZE_FAIL("bad value type list entry");
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

  pt_wasm_function_type_t types[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_types; i++) {
    const size_t types_ofs = (i & (PT_WASM_BATCH_SIZE - 1));
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

    if ((types_ofs == LEN(types)) && cbs && cbs->on_function_types) {
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
 * Parse limits into +dst+ from buffer +src+ of length +src_len+.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_limits(
  pt_wasm_limits_t * const dst,
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  if (src_len < 2) {
    SIZE_FAIL("truncated limits");
  }

  // check limits flag
  if ((src[0] != 0) && (src[0] != 1)) {
    SIZE_FAIL("bad limits flag");
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
    SIZE_FAIL("bad limits minimum");
  }

  // build return value
  size_t num_bytes = 1 + min_len;

  if (src[0] == 1) {
    // parse max, check for error
    const size_t max_len = pt_wasm_decode_u32(&(tmp.max), src + num_bytes, src_len - num_bytes);
    if (!max_len) {
      SIZE_FAIL("bad limits maximum");
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
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  if (src_len < 3) {
    SIZE_FAIL("incomplete table type");
  }

  // get element type, check for error
  // NOTE: at the moment only one element type is supported
  const pt_wasm_table_elem_type_t elem_type = src[0];
  if (elem_type != 0x70) {
    SIZE_FAIL("invalid table element type");
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

/**
 * Parse import into +dst_import+ from source buffer +src+, consuming a
 * maximum of +src_len+ bytes.
 *
 * Returns number of bytes consumed, or 0 on error.
 */
static size_t
pt_wasm_parse_import(
  pt_wasm_import_t * const dst_import,
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // parse module name, check for error
  pt_wasm_buf_t mod;
  const size_t mod_len = pt_wasm_parse_name(&mod, cbs, src, src_len, cb_data);
  if (!mod_len) {
    return false;
  }

  // parse name, check for error
  pt_wasm_buf_t name;
  const size_t name_len = pt_wasm_parse_name(&name, cbs, src + mod_len, src_len - mod_len, cb_data);
  if (!name_len) {
    return false;
  }

  // get import descriptor
  const pt_wasm_import_desc_t desc = src[mod_len + name_len];

  pt_wasm_import_t tmp = {
    .module = mod,
    .name = name,
    .import_desc = desc,
  };

  // calculate number of bytes consumed so far
  size_t num_bytes = mod_len + name_len + 1;

  // check length
  if (num_bytes >= src_len) {
    FAIL("incomplete import descriptor");
  }

  const uint8_t * const data_ptr = src + num_bytes;
  const size_t data_len = src_len - num_bytes;

  switch (desc) {
  case PT_WASM_IMPORT_DESC_FUNC:
    {
      const size_t len = pt_wasm_decode_u32(&(tmp.func.id), data_ptr, data_len);
      if (!len) {
        FAIL("invalid function import type");
      }

      // add length to result
      num_bytes += len;
    }

    break;
  case PT_WASM_IMPORT_DESC_TABLE:
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
  case PT_WASM_IMPORT_DESC_MEM:
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
  case PT_WASM_IMPORT_DESC_GLOBAL:
    {
      // parse global, check for error
      const size_t len = pt_wasm_decode_u32(&(tmp.global.type), data_ptr, data_len);
      if (!len) {
        FAIL("invalid import descriptor global value type");
      }

      // get mutable flag, check for error
      const uint8_t mut = data_ptr[len];
      if ((mut != 0) && (mut != 1)) {
        FAIL("invalid global import mutable flag");
      }

      // save mutable flag
      tmp.global.mutable = (mut == 1);

      // add length to result
      num_bytes += len + 1;
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
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // get number of imports, check for error
  uint32_t num_imports = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_imports, src, src_len);
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
      src + ofs,
      src_len - ofs,
      cb_data
    );

    if (!import_len) {
      // return failure
      return false;
    }

    // increment offset, check for error
    ofs += import_len;
    if (ofs > src_len) {
      FAIL("import section length overflow");
    }

    if ((imports_ofs == LEN(imports)) && cbs && cbs->on_imports) {
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
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // get number of fns, check for error
  uint32_t num_fns = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_fns, src, src_len);
  if (!len_ofs) {
    FAIL("invalid function section vector length");
  }

  // D("num_fns = %u", num_fns);

  uint32_t fns[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_fns; i++) {
    const size_t fns_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse fn, check for error
    const size_t fn_len = pt_wasm_decode_u32(fns + fns_ofs, src + ofs, src_len - ofs);
    if (!fn_len) {
      FAIL("invalid function index");
    }

    // D("fns[%zu] = %u", fns_ofs, fns[fns_ofs]);

    // increment offset, check for error
    ofs += fn_len;
    if (ofs > src_len) {
      FAIL("function section length overflow");
    }

    if ((fns_ofs == LEN(fns)) && cbs && cbs->on_functions) {
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
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // get number of tables, check for error
  uint32_t num_tbls = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_tbls, src, src_len);
  if (!len_ofs) {
    FAIL("invalid table section vector length");
  }

  // D("num_tbls = %u", num_tbls);

  pt_wasm_table_t tbls[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_tbls; i++) {
    const size_t tbls_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse fn, check for error
    const size_t tbl_len = pt_wasm_parse_table(tbls + tbls_ofs, cbs, src + ofs, src_len - ofs, cb_data);
    if (!tbl_len) {
      FAIL("invalid table section entry");
    }

    // D("tbls[%zu] = [%u, %u], has_max: %c", tbls_ofs, tbls[tbls_ofs].limits.min, tbls[tbls_ofs].limits.max, tbls[tbls_ofs].limits.has_max ? 't' : 'f');

    // increment offset, check for error
    ofs += tbl_len;
    if (ofs > src_len) {
      FAIL("table section length overflow");
    }

    if ((tbls_ofs == LEN(tbls)) && cbs && cbs->on_tables) {
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
  const pt_wasm_parse_cbs_t * const cbs,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  // get number of mems, check for error
  uint32_t num_mems = 0;
  const size_t len_ofs = pt_wasm_decode_u32(&num_mems, src, src_len);
  if (!len_ofs) {
    FAIL("invalid memory section vector length");
  }

  pt_wasm_limits_t mems[PT_WASM_BATCH_SIZE];

  for (size_t i = 0, ofs = len_ofs; i < num_mems; i++) {
    const size_t mems_ofs = (i & (PT_WASM_BATCH_SIZE - 1));

    // parse mem, check for error
    const size_t mem_len = pt_wasm_parse_limits(mems + mems_ofs, cbs, src + ofs, src_len - ofs, cb_data);
    if (!mem_len) {
      return 0;
    }

    // increment offset, check for error
    ofs += mem_len;
    if (ofs > src_len) {
      FAIL("memory section length overflow");
    }

    if ((mems_ofs == LEN(mems)) && cbs && cbs->on_memories) {
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
pt_wasm_parse_section(
  const pt_wasm_parse_cbs_t * const cbs,
  const pt_wasm_section_type_t sec_type,
  const uint8_t * const src,
  const size_t src_len,
  void * const cb_data
) {
  switch (sec_type) {
  case PT_WASM_SECTION_TYPE_CUSTOM:
    return pt_wasm_parse_custom_section(cbs, src, src_len, cb_data);
  case PT_WASM_SECTION_TYPE_TYPE:
    return pt_wasm_parse_type_section(cbs, src, src_len, cb_data);
  case PT_WASM_SECTION_TYPE_IMPORT:
    return pt_wasm_parse_import_section(cbs, src, src_len, cb_data);
  case PT_WASM_SECTION_TYPE_FUNCTION:
    return pt_wasm_parse_function_section(cbs, src, src_len, cb_data);
  case PT_WASM_SECTION_TYPE_TABLE:
    return pt_wasm_parse_table_section(cbs, src, src_len, cb_data);
  case PT_WASM_SECTION_TYPE_MEMORY:
    return pt_wasm_parse_memory_section(cbs, src, src_len, cb_data);
  default:
    FAIL("unknown section type");
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
