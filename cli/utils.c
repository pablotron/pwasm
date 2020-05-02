#include <stdbool.h> // bool
#include <stdlib.h> // size_t
#include <stdio.h> // fopen(), printf()
#include <err.h> // err()
#include "utils.h"

/**
 * Read contents of file and return result as a buffer.
 *
 * Note: This method calls err() and exits if the file can not be read
 * or memory allocation fails.
 */
pwasm_buf_t
cli_read_file(
  pwasm_mem_ctx_t * const mem_ctx,
  const char * const path
) {
  // open file, check for error
  FILE *fh = fopen(path, "rb");
  if (!fh) {
    // exit with error
    err(EXIT_FAILURE, "fopen(\"%s\")", path);
  }

  // seek to end, check for error
  if (fseek(fh, 0, SEEK_END)) {
    // exit with error
    err(EXIT_FAILURE, "fseek()");
  }

  // get length, check for error
  long len = ftell(fh);
  if (len < 0) {
    // exit with error
    err(EXIT_FAILURE, "ftell()");
  }

  // seek to start, check for error
  if (fseek(fh, 0, SEEK_SET)) {
    // exit with error
    err(EXIT_FAILURE, "fseek()");
  }

  // alloc memory, check for error
  void * const mem = pwasm_realloc(mem_ctx, NULL, len);
  if (!mem) {
    // exit with error
    err(EXIT_FAILURE, "malloc(%zu)", len);
  }

  // read file
  if (!fread(mem, len, 1, fh)) {
    // exit with error
    err(EXIT_FAILURE, "fread()");
  }

  // close file, check for error
  if (fclose(fh)) {
    // log error, continue
    warn("fclose()");
  }

  // return result
  return (pwasm_buf_t) {
    .ptr = mem,
    .len = len,
  };
}
