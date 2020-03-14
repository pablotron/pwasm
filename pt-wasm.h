#ifndef PT_WASM_H
#define PT_WASM_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h> // size_t

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

const char *pt_wasm_section_type_get_name(
  const pt_wasm_section_type_t
);

typedef struct {
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
