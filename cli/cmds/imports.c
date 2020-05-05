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

  // write CSV header
  fputs("type,module,name\n", io);
  for (size_t i = 0; i < mod->num_imports; i++) {
    // get import
    const pwasm_import_t import = mod->imports[i];

    // print row
    fprintf(io, "%s,\"", pwasm_import_type_get_name(import.type));
    cli_escape_bytes(mod, import.module, on_mod_data, io);
    fputs("\",\"", io);
    cli_escape_bytes(mod, import.name, on_mod_data, io);
    fputs("\"\n", io);
  }
}

int cmd_imports(
  const int argc,
  const char ** argv
) {
  // check args
  if (argc < 3) {
    fputs("Error: Missing WASM file name.\nSee help for usage.\n", stderr);
    return -1;
  } else if (argc > 3) {
    fputs("Error: Too many arguments.\nSee help for usage.\n", stderr);
  }

  // create memory context
  pwasm_mem_ctx_t mem_ctx = pwasm_mem_ctx_init_defaults(NULL);

  // load module, invoke callback with module
  cli_with_mod(&mem_ctx, argv[2], on_mod, stdout);

  // return success
  return 0;
}
