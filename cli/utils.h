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

#endif /* CLI_UTILS_H */
