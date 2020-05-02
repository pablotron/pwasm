#include <stdbool.h> // bool
#include <stdlib.h> // size_t
#include <stdio.h> // fopen(), printf()
#include <err.h> // err()
#include "../utils.h" // cli_read_file(), etc
#include "../../pwasm.h" // pwasm_mod_init(), etc

typedef struct {
  FILE *io;
  const int argc;
  const char ** argv;
} cmd_cat_custom_t;

static void
on_mod(
  const pwasm_mod_t * const mod,
  void *data_ptr
) {
  cmd_cat_custom_t * const data = data_ptr;

  for (int i = 3; i < data->argc; i++) {
    // FIXME: use strtol() instead to detect errors
    const int id = atoi(data->argv[i]);

    if (!id && data->argv[i][0] != '0') {
      errx(EXIT_FAILURE, "Invalid custom section ID: %s\n", data->argv[i]);
    } else if ((id < 0) || (size_t) id >= mod->num_custom_sections) {
      errx(EXIT_FAILURE, "Custom section ID out of bounds: %d\n", id);
    } else {
      const pwasm_slice_t slice = mod->custom_sections[id].data;
      if (!fwrite(mod->bytes + slice.ofs, slice.len, 1, data->io)) {
        err(EXIT_FAILURE, "fwrite()");
      }
    }
  }
}

int cmd_cat_custom(
  const int argc,
  const char ** argv
) {
  // check args
  if (argc < 3) {
    fputs("Error: Missing WASM path.\nSee help for usage.\n", stderr);
    return -1;
  }

  // create memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  // build callback data
  cmd_cat_custom_t data = {
    .io   = stdout,
    .argc = argc,
    .argv = argv,
  };

  // load module, invoke callback with module
  cli_with_mod(&mem_ctx, argv[2], on_mod, &data);

  // return success
  return 0;
}
