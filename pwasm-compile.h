#ifndef PWASM_COMPILE_H
#define PWASM_COMPILE_H

/**
 * @file
 */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include "pwasm.h"

/**
 * Compile a module function, populate destination buffer with function
 * pointer and size of generated code.
 *
 * @ingroup jit
 *
 * @param[out] dst      Destination buffer.
 * @param[in]  env      Execution environment.
 * @param[in]  mod      Module.
 * @param[in]  func_ofs Function offset in module.
 *
 * @return `true` on success or `false` if an error occurred.
 */
_Bool pwasm_compile(
  pwasm_buf_t *dst, //< destination buffer
  pwasm_env_t *env, //< execution environment
  const pwasm_mod_t *mod, //< module
  const size_t func_ofs //< function offset
);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PWASM_COMPILE_H */
