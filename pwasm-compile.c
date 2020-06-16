#include <stdbool.h>
#include "pwasm-compile.h"

static void
fail(
  pwasm_env_t * const env,
  const char * const text
) {
  pwasm_fail(env->mem_ctx, text);
}

bool
pwasm_compile(
  pwasm_buf_t * const dst,
  pwasm_env_t *env,
  const pwasm_mod_t *mod,
  const size_t func_ofs
) {
  const pwasm_func_t func = mod->codes[func_ofs];
  const pwasm_inst_t * const insts = mod->insts + func.expr.ofs;
  (void) dst;

  // size_t ctrl_depth = 0;

  // D("expr = { .ofs = %zu, .len = %zu }, num_insts = %zu", expr.ofs, expr.len, frame.mod->num_insts);

  for (size_t i = 0; i < func.expr.len; i++) {
    const pwasm_inst_t in = insts[i];
    // D("0x%02X %s", in.op, pwasm_op_get_name(in.op));

    switch (in.op) {
    default:
      // log error, return failure
      fail(env, "unimplemented opcode:");
      return false;
    }
  }

  // return failure
  return false;
}
