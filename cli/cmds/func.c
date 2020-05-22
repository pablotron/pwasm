#include <stdbool.h> // bool
#include <stdlib.h> // size_t
#include <stdio.h> // fopen(), printf()
#include <string.h> // strlen()
#include <err.h> // err()
#include "../utils.h" // cli_read_file(), etc
#include "../../pwasm.h" // pwasm_mod_init(), etc

typedef struct {
  FILE *io;
  const int argc;
  const char ** argv;
} cmd_func_t;

static uint32_t
find_func_export(
  const pwasm_mod_t * const mod,
  const char * const name
) {
  const size_t name_len = strlen(name);

  for (size_t i = 0; i < mod->num_exports; i++) {
    const pwasm_export_t export = mod->exports[i];
    const char * const export_name = (char*) mod->bytes + export.name.ofs;

    if (
      (export.type == PWASM_IMPORT_TYPE_FUNC) &&
      (name_len == export.name.len) &&
      !memcmp(export_name, name, export.name.len)
    ) {
      // return export offset
      return i;
    }
  }

  // print error and exit
  errx(EXIT_FAILURE, "Error: unknown export function: %s", name);

  // return failure (never reached)
  return mod->num_exports;
}

static void
on_mod(
  const pwasm_mod_t * const mod,
  void *data_ptr
) {
  cmd_func_t * const data = data_ptr;

  // pass through all the arguments to look for errors
  for (int i = 3; i < data->argc; i++) {
    find_func_export(mod, data->argv[i]);
  }

  fputs("function,class,sort,type\n", data->io);
  // pass through all the arguments to look for errors
  for (int i = 3; i < data->argc; i++) {
    const char * const name = data->argv[i];
    const uint32_t export_id = find_func_export(mod, name);
    const pwasm_type_t type = mod->types[mod->exports[export_id].id];
    const uint32_t * const params = mod->u32s + type.params.ofs;
    const uint32_t * const results = mod->u32s + type.results.ofs;

    // print function parameters
    for (size_t j = 0; j < type.params.len; j++) {
      const char * const val_type_name = pwasm_value_type_get_name(params[j]);
      fprintf(data->io, "\"%s\",param,%zu,%s\n", name, j, val_type_name);
    }

    // print function results
    for (size_t j = 0; j < type.results.len; j++) {
      const char * const val_type_name = pwasm_value_type_get_name(results[j]);
      fprintf(data->io, "\"%s\",result,%zu,%s\n", name, j, val_type_name);
    }
  }
}

int cmd_func(
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
  cmd_func_t data = {
    .io   = stdout,
    .argc = argc,
    .argv = argv,
  };

  // load module, invoke callback with module
  cli_with_mod(&mem_ctx, argv[2], on_mod, &data);

  // return success
  return 0;
}
