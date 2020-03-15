#ifndef PT_WASM_H
#define PT_WASM_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h> // size_t
#include <stdint.h> // uint8_t, uint32_t, etc

#define PT_WASM_SECTION_TYPES \
  PT_WASM_SECTION_TYPE(CUSTOM, "custom") \
  PT_WASM_SECTION_TYPE(TYPE, "type") \
  PT_WASM_SECTION_TYPE(IMPORT, "import") \
  PT_WASM_SECTION_TYPE(FUNCTION, "function") \
  PT_WASM_SECTION_TYPE(TABLE, "table") \
  PT_WASM_SECTION_TYPE(MEMORY, "memory") \
  PT_WASM_SECTION_TYPE(GLOBAL, "global") \
  PT_WASM_SECTION_TYPE(EXPORT, "export") \
  PT_WASM_SECTION_TYPE(START, "start") \
  PT_WASM_SECTION_TYPE(ELEMENT, "element") \
  PT_WASM_SECTION_TYPE(CODE, "code") \
  PT_WASM_SECTION_TYPE(DATA, "data") \
  PT_WASM_SECTION_TYPE(LAST, "unknown section")

#define PT_WASM_SECTION_TYPE(a, b) PT_WASM_SECTION_TYPE_##a,
typedef enum {
PT_WASM_SECTION_TYPES
} pt_wasm_section_type_t;
#undef PT_WASM_SECTION_TYPE

/**
 * Get name of section type.
 *
 * Returns a pointer to the null-terminated name of a section, or the
 * string "unknown section" if the given section type is unknown.
 *
 * Note: The strings returned by this function should not be freed.
 */
const char *pt_wasm_section_type_get_name(const pt_wasm_section_type_t);

typedef struct {
  const uint8_t *ptr;
  size_t len;
} pt_wasm_buf_t;

typedef struct {
  pt_wasm_buf_t name;
  pt_wasm_buf_t data;
} pt_wasm_custom_section_t;

typedef struct {
  pt_wasm_buf_t params;
  pt_wasm_buf_t results;
} pt_wasm_function_type_t;

typedef struct {
  uint32_t min;
  uint32_t max;
  _Bool has_max;
} pt_wasm_limits_t;

typedef uint32_t pt_wasm_table_elem_type_t;

typedef struct {
  pt_wasm_table_elem_type_t elem_type; /* must be 0x70 */
  pt_wasm_limits_t limits;
} pt_wasm_table_t;

typedef uint32_t pt_wasm_value_type_t;

const char *pt_wasm_value_type_get_name(const pt_wasm_value_type_t);

typedef struct {
  pt_wasm_value_type_t type;
  _Bool mutable;
} pt_wasm_global_type_t;

typedef struct {
  pt_wasm_buf_t buf;
} pt_wasm_expr_t;

typedef struct {
  pt_wasm_global_type_t type;
  pt_wasm_expr_t expr;
} pt_wasm_global_t;

#define PT_WASM_IMPORT_DESCS \
  PT_WASM_IMPORT_DESC(FUNC, "func") \
  PT_WASM_IMPORT_DESC(TABLE, "table") \
  PT_WASM_IMPORT_DESC(MEM, "mem") \
  PT_WASM_IMPORT_DESC(GLOBAL, "global") \
  PT_WASM_IMPORT_DESC(LAST, "unknown import desc")

#define PT_WASM_IMPORT_DESC(a, b) PT_WASM_IMPORT_DESC_##a,
typedef enum {
PT_WASM_IMPORT_DESCS
} pt_wasm_import_desc_t;
#undef PT_WASM_IMPORT_DESC

const char *pt_wasm_import_desc_get_name(const pt_wasm_import_desc_t);

typedef struct {
  pt_wasm_buf_t module;
  pt_wasm_buf_t name;
  pt_wasm_import_desc_t import_desc;

  union {
    struct {
      /* type index */
      uint32_t id;
    } func;

    pt_wasm_table_t table;

    struct {
      pt_wasm_limits_t limits;
    } mem;

    pt_wasm_global_type_t global;
  };
} pt_wasm_import_t;

typedef struct {
  void (*on_custom_section)(const pt_wasm_custom_section_t *, void *);
  void (*on_function_types)(const pt_wasm_function_type_t *, const size_t, void *);
  void (*on_imports)(const pt_wasm_import_t *, const size_t, void *);
  void (*on_functions)(const uint32_t *, const size_t, void *);
  void (*on_tables)(const pt_wasm_table_t *, const size_t, void *);
  void (*on_memories)(const pt_wasm_limits_t *, const size_t, void *);
  void (*on_globals)(const pt_wasm_global_t *, const size_t, void *);

  void (*on_error)(const char *, void *);
} pt_wasm_parse_cbs_t;

_Bool pt_wasm_parse(
  const void * const src_ptr,
  const size_t src_len,
  const pt_wasm_parse_cbs_t * const cbs,
  void * const data
);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PT_WASM_H */
