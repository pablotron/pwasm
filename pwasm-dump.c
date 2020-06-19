#include <stdlib.h> // EXIT_FAILURE
#include <stdio.h> // fopen()
#include <err.h> // err()
#include <string.h> // memcpy()
#include "pwasm.h"

/**
 * Write buffer to file.
 */
static void
pwasm_buf_save(
  const pwasm_buf_t buf,
  const char * const path
) {
  // open file
  FILE *fh = fopen(path, "wb");
  if (!fh) {
    err(EXIT_FAILURE, "fopen(\"%s\", \"wb\")", path);
  }

  // write data
  if (!fwrite(buf.ptr, buf.len, 1, fh)) {
    err(EXIT_FAILURE, "fwrite()");
  }
  
  // close file (ignore error)
  fclose(fh);
}

/**
 * Get name for compiled module function.
 *
 * Populates `dst` with a name like "compiled-MOD-FUNC_OFS.dat", where MOD
 * is the name of the module instance and FUNC_OFS is the offset of the
 * function in the module.
 */
static void
pwasm_dump_get_name(
  char * const dst,
  pwasm_env_t *env,
  const uint32_t mod_id,
  const uint32_t func_ofs
) {
  // get module instance name
  const pwasm_buf_t * const mod_name = pwasm_env_get_mod_name(env, mod_id);
  if (!mod_name) {
    errx(EXIT_FAILURE, "pwasm_env_get_mod_name() failed");
  }

  // fill destination buffer
  memcpy(dst, "dump-", 5);
  memcpy(dst + 5, mod_name->ptr, mod_name->len);
  sprintf(dst + 5 + mod_name->len, "-%02u.dat", func_ofs);
}

/**
 * Write compiled module function to a file.
 *
 * You can disassemble the contents of a file like so:
 *   objdump -D -b binary -Mintel path
 *   objdump -D -b binary -M x86-64,intel -m i386 ./compiled
 *   or
 *   ndisasm -b 64 ./compiled
 */
void
pwasm_dump(
  pwasm_env_t *env,
  const uint32_t mod_id,
  const size_t func_ofs,
  const pwasm_buf_t data
) {
  // get output file name
  char name_buf[512];
  pwasm_dump_get_name(name_buf, env, mod_id, func_ofs);

  // save data to file
  pwasm_buf_save(data, name_buf);
}

