#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include <stdlib.h> // malloc()
#include <string.h> // strlen()
#include <stdio.h> // fopen()
#include <err.h> // err()
#include "pt-wasm.h"
#include "tests.h"

static char *
read_file(
  const char * const path,
  size_t * const ret_len
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
  void * const mem = malloc(len);
  if (!mem) {
    // exit with error
    err(EXIT_FAILURE, "malloc()");
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

  if (ret_len) {
    // return length
    *ret_len = len;
  }

  // return pointer to memory
  return mem;
}

static void
dump_limits(
  FILE * fh,
  const pt_wasm_limits_t * const lim
) {
  if (lim->has_max) {
    fprintf(fh, "(min = %u, max = %u)", lim->min, lim->max);
  } else {
    fprintf(fh, "(min = %u)", lim->min);
  }
}

static void
dump_buf(
  FILE * fh,
  const pt_wasm_buf_t buf
) {
  // print header
  fputs("[", fh);

  // print contents
  for (size_t i = 0; i < buf.len; i++) {
    fprintf(fh, "%s0x%02X", ((i > 0) ? ", " : ""), buf.ptr[i]);
  }

  // add footer
  fputs("]", fh);
}

static void
dump_global(
  FILE * fh,
  const pt_wasm_global_t * const g
) {
  // print global attributes
  fprintf(fh, "{type: \"%s\", mut: %c, expr: ",
    pt_wasm_value_type_get_name(g->type.type),
    g->type.mutable ? 't' : 'f'
  );

  // print expr
  dump_buf(fh, g->expr.buf);

  // add footer
  fputs("}", fh);
}

static void
on_test_globals(
  const pt_wasm_global_t * const gs,
  const size_t num_gs,
  void * const data
) {
  (void) data;

  fprintf(stderr, "globals(%zu) = {", num_gs);
  for (size_t i = 0; i < num_gs; i++) {
    fprintf(stderr, "%s", (i > 0) ? ", " : "");
    dump_global(stderr, gs + i);
  }
  fputs("}\n", stderr);
}
static void
on_test_memories(
  const pt_wasm_limits_t * const mems,
  const size_t num_mems,
  void * const data
) {
  (void) data;

  fprintf(stderr, "memories(%zu) = {", num_mems);
  for (size_t i = 0; i < num_mems; i++) {
    fprintf(stderr, "%s", (i > 0) ? ", " : "");
    dump_limits(stderr, mems + i);
  }
  fputs("}\n", stderr);
}

static void
on_test_tables(
  const pt_wasm_table_t * const tbls,
  const size_t num_tbls,
  void * const data
) {
  (void) data;

  fprintf(stderr, "tables(%zu) = {", num_tbls);
  for (size_t i = 0; i < num_tbls; i++) {
    fprintf(stderr, "%s", (i > 0) ? ", " : "");
    dump_limits(stderr, &(tbls[i].limits));
  }
  fputs("}\n", stderr);
}

static void
on_test_functions(
  const uint32_t * const fns,
  const size_t num_fns,
  void * const data
) {
  (void) data;

  fprintf(stderr, "functions(%zu) = {", num_fns);
  for (size_t i = 0; i < num_fns; i++) {
    fprintf(stderr, "%s%u", (i > 0) ? ", " : "", fns[i]);
  }
  fputs("}\n", stderr);
}

static void
dump_import(
  FILE * fh,
  const pt_wasm_import_t * const im
) {
  fputs("\"", fh);
  fwrite(im->module.ptr, im->module.len, 1, fh);
  fputs(".", fh);
  fwrite(im->name.ptr, im->name.len, 1, fh);
  fprintf(fh, "\" (%s): ", pt_wasm_import_type_get_name(im->type));

  switch (im->type) {
  case PT_WASM_IMPORT_TYPE_FUNC:
    fprintf(fh, "id = %u", im->func.id);
    break;
  case PT_WASM_IMPORT_TYPE_TABLE:
    fprintf(fh, "elem_type = %u, ", im->table.elem_type);
    dump_limits(fh, &(im->table.limits));
    break;
  case PT_WASM_IMPORT_TYPE_MEM:
    dump_limits(fh, &(im->mem.limits));
    break;
  case PT_WASM_IMPORT_TYPE_GLOBAL:
    {
      const char * const name = pt_wasm_value_type_get_name(im->global.type);
      const char mut = im->global.mutable ? 't' : 'f';
      fprintf(fh, "type = %s, mutable = %c", name, mut);
    }
    break;
  default:
    errx(EXIT_FAILURE, "invalid import_type: %u", im->type);
  }

  fprintf(fh, "\n");
}

static void
on_test_imports(
  const pt_wasm_import_t * const imports,
  const size_t num_imports,
  void * const data
) {
  (void) data;

  for (size_t i = 0; i < num_imports; i++) {
    fprintf(stderr, "imports[%zu]: ", i);
    dump_import(stderr, imports + i);
  }
}

static void
dump_custom_section(
  FILE * fh,
  const pt_wasm_custom_section_t * const s
) {
  fprintf(fh, "name(%zu) = \"", s->name.len);
  fwrite(s->name.ptr, s->name.len, 1, fh);
  fprintf(fh, "\", data(%zu) = {", s->data.len);
  for (size_t j = 0; j < s->data.len; j++) {
    fprintf(fh, "%s%02x", ((j > 0) ? ", " : ""), s->data.ptr[j]);
  }
  fputs("}\n", fh);
}

static void
on_test_custom_section(
  const pt_wasm_custom_section_t * const s,
  void * const data
) {
  (void) data;

  dump_custom_section(stderr, s);
}

static void
dump_export(
  FILE * fh,
  const pt_wasm_export_t * const e
) {
  const char * const type_name = pt_wasm_export_type_get_name(e->type);
  fputs("{name: \"", fh);
  fwrite(e->name.ptr, e->name.len, 1, fh);
  fprintf(fh, "\", type: %s, id: %u}", type_name, e->id);
}

static void
on_test_exports(
  const pt_wasm_export_t * const exports,
  const size_t num_exports,
  void * const data
) {
  (void) data;

  fprintf(stderr, "exports(%zu): ", num_exports);
  for (size_t i = 0; i < num_exports; i++) {
    fputs((i > 0) ? ", " : "", stderr);
    dump_export(stderr, exports + i);
  }
  fputs("\n", stderr);
}

static void
on_test_function_codes(
  const pt_wasm_buf_t * const fns,
  const size_t num_fns,
  void * const data
) {
  (void) data;

  fprintf(stderr, "function codes(%zu):\n", num_fns);
  for (size_t i = 0; i < num_fns; i++) {
    fprintf(stderr, "function[%zu].len = %zu\n", i, fns[i].len);
  }
}

static void
on_test_data_segments(
  const pt_wasm_data_segment_t * const ds,
  const size_t len,
  void * const data
) {
  (void) data;

  fprintf(stderr, "data segments(%zu):\n", len);
  for (size_t i = 0; i < len; i++) {
    fprintf(stderr, "segment[%zu] = {id: %u, expr: ", i, ds[i].mem_id);
    dump_buf(stderr, ds[i].expr.buf);
    fputs(", data: ", stderr);
    dump_buf(stderr, ds[i].data);
  }
}


static void
on_test_error(const char * const text, void * const data) {
  const test_t * const test = data;
  warnx("test = \"%s\", error = \"%s\"", test->name, text);
}

static const pt_wasm_parse_cbs_t GOOD_TEST_CBS = {
  .on_custom_section  = on_test_custom_section,
  .on_imports         = on_test_imports,
  .on_functions       = on_test_functions,
  .on_tables          = on_test_tables,
  .on_memories        = on_test_memories,
  .on_globals         = on_test_globals,
  .on_exports         = on_test_exports,
  .on_function_codes  = on_test_function_codes,
  .on_data_segments   = on_test_data_segments,
  .on_error           = on_test_error,
};

static bool
run_tests(void) {
  const size_t NUM_TESTS = get_num_tests();
  const test_t * const TESTS = get_tests();
  const uint8_t * const TEST_DATA = get_test_data();
  size_t num_fails = 0;

  for (size_t i = 0; i < NUM_TESTS; i++) {
    // get test, run it, and get result
    const test_t * const test = TESTS + i;
    const bool r = pt_wasm_parse(
      TEST_DATA + test->ofs,
      test->len,
      test->want ? &GOOD_TEST_CBS : NULL,
      (void*) test
    );

    // check result, increment failure count
    const bool ok = (r == test->want);
    num_fails += ok ? 0 : 1;

    if (!ok) {
      // warn on failure
      warnx("FAIL: %s", test->name);
    }
  }

  // print results, return number of failures
  printf("Tests: %zu/%zu\n", NUM_TESTS - num_fails, NUM_TESTS);
  return num_fails;
}

int main(int argc, char *argv[]) {
  // run tests, check for error
  if (run_tests()) {
    // return failure
    return EXIT_FAILURE;
  }

  // loop through files
  for (int i = 1; i < argc; i++) {
    // read file contents
    size_t len;
    void * const mem = read_file(argv[i], &len);

    // free memory
    free(mem);
  }

  // return success
  return 0;
}
