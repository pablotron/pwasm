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

#define FLUSH() do { \
  if (tmp_ofs) { \
    on_data((pwasm_buf_t) { tmp, tmp_ofs }, data); \
    tmp_ofs = 0; \
  } \
} while (0)

#define PUSH(b) do { \
  tmp[tmp_ofs++] = (b); \
  if (tmp_ofs == sizeof(tmp)) { \
    FLUSH(); \
  } \
} while (0)

const uint8_t HEX[] = {
  '0', '1', '2', '3', '4', '5', '6', '7',
  '8', '9', 'A', 'B', 'C', 'D', 'E', 'F',
};

/**
 * Escape string data and pass it to given callback.
 */
void cli_escape_bytes(
  const pwasm_mod_t * const mod,
  const pwasm_slice_t slice,
  void (*on_data)(const pwasm_buf_t, void *),
  void *data
) {
  uint8_t tmp[1024];
  size_t tmp_ofs = 0;

  for (size_t i = 0 ; i < slice.len; i++) {
    const uint8_t byte = mod->bytes[slice.ofs + i];

    switch (byte) {
    case '\t':
      PUSH('\\');
      PUSH('t');
      break;
    case '\n':
      PUSH('\\');
      PUSH('n');
      break;
    case '\r':
      PUSH('\\');
      PUSH('r');
      break;
    case '"':
      PUSH('\\');
      PUSH('"');
      break;
    case '\'':
      PUSH('\\');
      PUSH('\'');
      break;
    case '\\':
      PUSH('\\');
      PUSH('\\');
      break;
    default:
      if (byte >= 32 && byte <= 126) {
        // push unescaped byte
        PUSH(byte);
      } else {
        // push hex-encoded byte
        PUSH('\\');
        PUSH(HEX[byte >> 16]);
        PUSH(HEX[byte & 0x0F]);
      }
    }
  }

  FLUSH();
}

#undef FLUSH
#undef PUSH

void cli_with_mod(
  pwasm_mem_ctx_t * const mem_ctx,
  const char * const path,
  void (*on_mod)(const pwasm_mod_t *, void *),
  void *data
) {
  // read file data
  pwasm_buf_t buf = cli_read_file(mem_ctx, path);

  // parse mod, check for error
  pwasm_mod_t mod;
  if (!pwasm_mod_init(mem_ctx, &mod, buf)) {
    errx(EXIT_FAILURE, "%s: pwasm_mod_init() failed", path);
  }

  // write module to output
  on_mod(&mod, data);

  // free mod
  pwasm_mod_fini(&mod);

  // free file data
  pwasm_realloc(mem_ctx, (void*) buf.ptr, 0);
}
