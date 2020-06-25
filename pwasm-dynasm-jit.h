#ifndef PWASM_DYNASM_JIT_H
#define PWASM_DYNASM_JIT_H

/**
 * @file
 *
 * PWASM JIT compiler, implemented via DynASM.
 */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "pwasm.h"

/**
 * Initialize DynASM JIT compiler.
 *
 * @note This function is architecture and operating system specific.
 *
 * @ingroup jit
 *
 * @param[out] jit     Destination JIT compiler.
 * @param[in]  mem_ctx Memory context.
 *
 * @return `true` on success or `false` if an error occurred.
 */
_Bool pwasm_dynasm_jit_init(
  pwasm_jit_t *jit, ///< destination JIT compiler
  pwasm_mem_ctx_t *mem_ctx  ///< memory context
);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PWASM_DYNASM_JIT_H */
