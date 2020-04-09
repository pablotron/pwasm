#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include <stdlib.h> // malloc()
#include <string.h> // strlen()
#include <stdio.h> // fopen()
#include <err.h> // err()
#include "pwasm.h"
#include "mod-tests.h"
#include "func-tests.h"

typedef struct {
  size_t num_fails;
  size_t num_tests;
} result_t;

static inline result_t
result(
  const size_t num_fails,
  const size_t num_tests
) {
  const result_t r = { num_fails, num_tests };
  return r;
}

static inline result_t
add_results(
  const result_t a,
  const result_t b
) {
  const result_t r = {
    .num_fails = a.num_fails + b.num_fails,
    .num_tests = a.num_tests + b.num_tests,
  };

  return r;
}

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
  const pwasm_limits_t * const lim
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
  const pwasm_buf_t buf
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
  const pwasm_global_t * const g
) {
  // print global attributes
  fprintf(fh, "{type: \"%s\", mut: %c, expr: ",
    pwasm_value_type_get_name(g->type.type),
    g->type.mutable ? 't' : 'f'
  );

  // print expr
  dump_buf(fh, g->expr.buf);

  // add footer
  fputs("}", fh);
}

static void
parse_mod_test_on_globals(
  const pwasm_global_t * const gs,
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
parse_mod_test_on_memories(
  const pwasm_limits_t * const mems,
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
parse_mod_test_on_tables(
  const pwasm_table_t * const tbls,
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
parse_mod_test_on_functions(
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
  const pwasm_import_t * const im
) {
  fputs("\"", fh);
  fwrite(im->module.ptr, im->module.len, 1, fh);
  fputs(".", fh);
  fwrite(im->name.ptr, im->name.len, 1, fh);
  fprintf(fh, "\" (%s): ", pwasm_import_type_get_name(im->type));

  switch (im->type) {
  case PWASM_IMPORT_TYPE_FUNC:
    fprintf(fh, "id = %u", im->func.id);
    break;
  case PWASM_IMPORT_TYPE_TABLE:
    fprintf(fh, "elem_type = %u, ", im->table.elem_type);
    dump_limits(fh, &(im->table.limits));
    break;
  case PWASM_IMPORT_TYPE_MEM:
    dump_limits(fh, &(im->mem.limits));
    break;
  case PWASM_IMPORT_TYPE_GLOBAL:
    {
      const char * const name = pwasm_value_type_get_name(im->global.type);
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
parse_mod_test_on_imports(
  const pwasm_import_t * const imports,
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
  const pwasm_custom_section_t * const s
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
parse_mod_test_on_custom_section(
  const pwasm_custom_section_t * const s,
  void * const data
) {
  (void) data;

  dump_custom_section(stderr, s);
}

static void
dump_export(
  FILE * fh,
  const pwasm_export_t * const e
) {
  const char * const type_name = pwasm_export_type_get_name(e->type);
  fputs("{name: \"", fh);
  fwrite(e->name.ptr, e->name.len, 1, fh);
  fprintf(fh, "\", type: %s, id: %u}", type_name, e->id);
}

static void
parse_mod_test_on_exports(
  const pwasm_export_t * const exports,
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
parse_mod_test_on_function_codes(
  const pwasm_buf_t * const fns,
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
parse_mod_test_on_data_segments(
  const pwasm_data_segment_t * const ds,
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
    fputs("\n", stderr);
  }
}

static void
parse_mod_test_on_error(const char * const text, void * const data) {
  const test_t * const test = data;
  warnx("parse mod test = \"%s\", error = \"%s\"", test->name, text);
}

static const pwasm_parse_module_cbs_t
PARSE_MOD_TEST_CBS = {
  .on_custom_section  = parse_mod_test_on_custom_section,
  .on_imports         = parse_mod_test_on_imports,
  .on_functions       = parse_mod_test_on_functions,
  .on_tables          = parse_mod_test_on_tables,
  .on_memories        = parse_mod_test_on_memories,
  .on_globals         = parse_mod_test_on_globals,
  .on_exports         = parse_mod_test_on_exports,
  .on_function_codes  = parse_mod_test_on_function_codes,
  .on_data_segments   = parse_mod_test_on_data_segments,
  .on_error           = parse_mod_test_on_error,
};

static result_t
run_parse_mod_tests(void) {
  const suite_t suite = get_mod_tests();
  size_t num_fails = 0;

  for (size_t i = 0; i < suite.num_tests; i++) {
    // get test, run it, and get result
    const test_t * const test = suite.tests + i;

    // run test, get result
    const bool r = pwasm_parse_module(
      (suite.data + test->ofs),
      test->len,
      test->want ? &PARSE_MOD_TEST_CBS : NULL,
      (void*) test
    );

    // check result, increment failure count
    const bool ok = (r == test->want);
    num_fails += ok ? 0 : 1;

    if (!ok) {
      // warn on failure
      warnx("FAIL parse_module test: %s", test->name);
    }
  }

  // return results
  return result(num_fails, suite.num_tests);
}

static void
dump_inst(
  FILE *fh,
  const pwasm_inst_t in
) {
  fprintf(fh, "(%s)", pwasm_op_get_name(in.op));
}

static void
parse_func_test_on_insts(
  const pwasm_inst_t * const ins,
  const size_t len,
  void * const data
) {
  (void) data;

  fprintf(stderr, "insts(%zu) = ", len);
  for (size_t i = 0; i < len; i++) {
    fputs((i > 0) ? ", " : "", stderr);
    dump_inst(stderr, ins[i]);
  }
  fprintf(stderr, "\n");
}

static void
parse_func_test_on_error(const char * const text, void * const data) {
  const test_t * const test = data;
  warnx("parse func test = \"%s\", error = \"%s\"", test->name, text);
}

static const pwasm_parse_function_cbs_t
PARSE_FUNC_TEST_CBS = {
  .on_insts = parse_func_test_on_insts,
  .on_error = parse_func_test_on_error,
};

static result_t
run_parse_func_tests(void) {
  const suite_t suite = get_func_tests();
  size_t num_fails = 0;

  for (size_t i = 0; i < suite.num_tests; i++) {
    // get test, run it, and get result
    const test_t * const test = suite.tests + i;

    const pwasm_buf_t buf = {
      .ptr = suite.data + test->ofs,
      .len = test->len,
    };

    fprintf(stderr, "tests[%zu].data = ", i);
    dump_buf(stderr, buf);
    fprintf(stderr, "\n");

    // run test, get result
    const bool r = pwasm_parse_function(
      buf,
      test->want ? &PARSE_FUNC_TEST_CBS : NULL,
      (void*) test
    );

    // check result, increment failure count
    const bool ok = (r == test->want);
    num_fails += ok ? 0 : 1;

    if (!ok) {
      // warn on failure
      warnx("FAIL: parse_function test: %s", test->name);
    }
  }

  // return results
  return result(num_fails, suite.num_tests);
}

static void
dump_mod_sizes(
  FILE * fh,
  const char * const test_name,
  const pwasm_module_sizes_t * const sizes
) {
  fprintf(fh,
    "\"%s\" = {\n"
    "  src = {\n"
    "    ptr = %p,\n"
    "    len = %zu,\n"
    "  },\n"
    "  num_custom_sections = %zu,\n"
    "  num_function_params = %zu,\n"
    "  num_function_results = %zu,\n"
    "  num_function_types = %zu,\n"
    "  num_import_types = {\n"
    "    [%u] = %zu, // %s\n"
    "    [%u] = %zu, // %s\n"
    "    [%u] = %zu, // %s\n"
    "    [%u] = %zu, // %s\n"
    "  },\n"
    "  num_imports = %zu,\n"
    "  num_functions = %zu,\n"
    "  num_tables = %zu,\n"
    "  num_memories = %zu,\n"
    "  num_global_insts = %zu,\n"
    "  num_globals = %zu,\n"
    "  num_exports = %zu,\n"
    "  num_element_func_ids = %zu,\n"
    "  num_element_insts = %zu,\n"
    "  num_elements = %zu,\n"
    "  num_locals = %zu,\n"
    "  num_function_insts = %zu,\n"
    "  num_function_codes = %zu,\n"
    "  num_data_segment_insts = %zu,\n"
    "  num_data_segments = %zu,\n"
    "  num_insts = %zu,\n"
    "}\n",
    test_name,

    sizes->src.ptr,
    sizes->src.len,

    sizes->num_custom_sections,
    sizes->num_function_params,
    sizes->num_function_results,
    sizes->num_function_types,

    PWASM_IMPORT_TYPE_FUNC,
    sizes->num_import_types[PWASM_IMPORT_TYPE_FUNC],
    pwasm_import_type_get_name(PWASM_IMPORT_TYPE_FUNC),

    PWASM_IMPORT_TYPE_TABLE,
    sizes->num_import_types[PWASM_IMPORT_TYPE_TABLE],
    pwasm_import_type_get_name(PWASM_IMPORT_TYPE_TABLE),

    PWASM_IMPORT_TYPE_MEM,
    sizes->num_import_types[PWASM_IMPORT_TYPE_MEM],
    pwasm_import_type_get_name(PWASM_IMPORT_TYPE_MEM),

    PWASM_IMPORT_TYPE_GLOBAL,
    sizes->num_import_types[PWASM_IMPORT_TYPE_GLOBAL],
    pwasm_import_type_get_name(PWASM_IMPORT_TYPE_GLOBAL),

    sizes->num_imports,
    sizes->num_functions,
    sizes->num_tables,
    sizes->num_memories,
    sizes->num_global_insts,
    sizes->num_globals,
    sizes->num_exports,
    sizes->num_element_func_ids,
    sizes->num_element_insts,
    sizes->num_elements,
    sizes->num_locals,
    sizes->num_function_insts,
    sizes->num_function_codes,
    sizes->num_data_segment_insts,
    sizes->num_data_segments,
    sizes->num_insts
  );
}

static void
get_mod_sizes_test_on_error(
  const char * const text,
  void * const data
) {
  const test_t * const test = data;
  warnx("get mod sizes test = \"%s\", error = \"%s\"", test->name, text);
}

static const pwasm_get_module_sizes_cbs_t
GET_MOD_SIZES_TEST_CBS = {
  .on_error = get_mod_sizes_test_on_error,
};

static result_t
run_get_mod_sizes_tests(void) {
  const suite_t suite = get_mod_tests();
  size_t num_fails = 0;

  for (size_t i = 0; i < suite.num_tests; i++) {
    // get test, run it, and get result
    const test_t * const test = suite.tests + i;

    // run test, get result
    pwasm_module_sizes_t sizes;
    const bool r = pwasm_get_module_sizes(
      &sizes,
      (suite.data + test->ofs),
      test->len,
      test->want ? &GET_MOD_SIZES_TEST_CBS : NULL,
      (void*) test
    );

    // check result, increment failure count
    const bool ok = (r == test->want);
    num_fails += ok ? 0 : 1;

    if (ok && test->want) {
      dump_mod_sizes(stderr, test->name, &sizes);
    } else if (!ok) {
      // warn on failure
      warnx("FAIL get_module_sizes test: %s", test->name);
    }
  }

  // return results
  return result(num_fails, suite.num_tests);
}

static result_t
run_mod_init_tests(void) {
  pwasm_mem_ctx_t ctx = pwasm_mem_ctx_init_defaults(NULL);
  const suite_t suite = get_mod_tests();
  size_t num_fails = 0;

  for (size_t i = 0; i < suite.num_tests; i++) {
    // get test, run it, and get result
    const test_t * const test = suite.tests + i;
    const pwasm_buf_t buf = {
      (suite.data + test->ofs),
      test->len,
    };

    warnx("running mod_init test: %s", test->name);
    // run test, get result
    pwasm_mod_t mod;
    const size_t len = pwasm_mod_init(&ctx, &mod, buf);

    // check result, increment failure count
    const bool ok = ((len > 0) == test->want);
    num_fails += ok ? 0 : 1;

    if (!ok) {
      // warn on failure
      warnx("FAIL mod_init test: %s", test->name);
    }

    // free mod
    pwasm_mod_fini(&mod);
  }

  // return results
  return result(num_fails, suite.num_tests);
}

static result_t (*SUITES[])(void) = {
  run_parse_mod_tests,
  run_parse_func_tests,
  run_get_mod_sizes_tests,
  run_mod_init_tests,
};

static bool
run_tests(void) {
  // run all test suites
  result_t sum = { 0, 0 };
  for (size_t i = 0; i < (sizeof(SUITES) / sizeof(SUITES[0])); i++) {
    // run suite, add to results
    sum = add_results(sum, SUITES[i]());
  }

  // print results
  const size_t num_passed = sum.num_tests - sum.num_fails;
  printf("Tests: %zu/%zu\n", num_passed, sum.num_tests);

  // return result
  return sum.num_fails > 0;
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
