#include <stdbool.h> // bool
#include <stdlib.h> // size_t
#include <stdio.h> // fopen(), printf()
#include <err.h> // err()
#include "../utils.h" // cli_read_file(), etc
#include "../../pwasm.h" // pwasm_mod_init(), etc

static void
on_mod_data(
  const pwasm_buf_t buf,
  void *data
) {
  FILE *io = data;
  fwrite(buf.ptr, buf.len, 1, io);
}

static void
on_mod(
  const pwasm_mod_t * const mod,
  void *data
) {
  FILE *io = data;

  // write header
  fputs("id,name\n", io);
  for (size_t i = 0; i < mod->num_custom_sections; i++) {
    const pwasm_custom_section_t section = mod->custom_sections[i];

    fprintf(io, "%zu,\"", i);
    cli_write_utf8(mod, section.name, on_mod_data, io);
    fputs("\"\n", io);
  }
}

int cmd_list_custom(
  const int argc,
  const char ** argv
) {
  // check args
  if (argc < 3) {
    fputs("Error: Missing WASM file naem.\nSee help for usage.\n", stderr);
    return -1;
  }

  // create memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  for (int i = 3; i < argc; i++) {
    // load module, invoke callback with module
    cli_with_mod(&mem_ctx, argv[2], on_mod, stdout);
  }

  // return success
  return 0;
}
