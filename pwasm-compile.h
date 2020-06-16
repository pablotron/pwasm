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
 * Compile a module function and return a pointer to the function.
 *
 * @ingroup jit
 *
 * @param env       Execution environment.
 * @param mod       Module.
 * @param func_ofs  Function offset in module.
 *
 * @return Function pointer, or `NULL` on error.
 */
void *pwasm_compile(
  pwasm_env_t *env,
  const pwasm_mod_t *mod,
  const size_t func_ofs
);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PWASM_COMPILE_H */
