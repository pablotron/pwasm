#ifndef CLI_UTILS_H
#define  CLI_UTILS_H

#include "../pwasm.h" // pwasm_buf_t, etc

/**
 * Read contents of file and return result as a buffer.
 *
 * Note: This method calls err() and exits if the file can not be read
 * or memory allocation fails.
 */
pwasm_buf_t cli_read_file(
  pwasm_mem_ctx_t * const,
  const char * const
);

/**
 * write escaped UTF-8 data from mod to file handle.
 */
void cli_write_utf8(
  const pwasm_mod_t * const,
  const pwasm_slice_t,
  void (*)(const pwasm_buf_t, void *),
  void *
);

/**
 * Load module in given file, invoke callback with parsed mod, then free
 * the memory associated with the module and return.
 *
 */
void cli_with_mod(
  pwasm_mem_ctx_t * const mem_ctx,
  const char * const path,
  void (*on_mod)(const pwasm_mod_t *, void *),
  void *data
);

#endif /* CLI_UTILS_H */
