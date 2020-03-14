#ifndef PT_WASM_H
#define PT_WASM_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h> // size_t

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
