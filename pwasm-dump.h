#ifndef PWASM_DUMP_H
#define PWASM_DUMP_H

/**
 * Write compiled module function to a file named `dump-MOD-FUNC_OFS.dat`.
 *
 * You can disassemble the contents of a file like so:
 *   objdump -D -b binary -Mintel path
 *   objdump -D -b binary -M x86-64,intel -m i386 ./compiled
 *   or
 *   ndisasm -b 64 ./compiled
 */
void pwasm_dump(
  pwasm_env_t *env,
  const uint32_t mod_id,
  const size_t func_ofs,
  const pwasm_buf_t data
);

#endif /* PWASM_DUMP_H */
