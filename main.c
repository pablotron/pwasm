#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include <stdlib.h> // malloc()
#include <string.h> // strlen()
#include <stdio.h> // fopen()
#include <err.h> // err()
#include "pt-wasm.h"

#define NUM_ITEMS(ary) (sizeof(ary) / sizeof(ary[0]))

static const uint8_t
TEST_DATA[] = {
  // bad header (fail, ofs: 0, len: 8)
  0x01, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,

  // good header (pass, ofs: 8, len: 8)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,

  // custom section: blank (pass, ofs: 16, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x01, 0x00,

  // custom section: no length (fail, ofs: 27, len: 9)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x00,

  // custom section: name truncated (fail, ofs: 36, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x02, 0x01,

  // custom section: hello (pass, ofs: 47, len: 16)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x06, 0x05, 'h',  'e',  'l',  'l',  'o',

  // custom section: hello, there (pass, ofs: 63, len: 21)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x0b, 0x05, 'h',  'e',  'l',  'l',  'o',
  't',  'h',  'e',  'r',  'e',

  // custom section: "", world (pass, ofs: 84, len: 16)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x00, 0x06, 0x00, 'w',  'o',  'r',  'l',  'd',

  // type section: partial (fail, ofs: 100, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x00,

  // type section: empty (pass, ofs: 110, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x01, 0x00,

  // type section: i32 -> void (pass, ofs: 121, len: 15)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x01, 0x7F, 0x00,

  // type section: junk -> void (fail, ofs: 136, len: 15)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x01, 0x00, 0x00,

  // type section: void -> i32 (pass, ofs: 151, len: 15)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7F,

  // type section: void -> junk (fail, ofs: 166, len: 15)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x01,

  // type section: i64, f32 -> void (pass, ofs: 181, len: 16)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x06, 0x01, 0x60, 0x02, 0x7E, 0x7D, 0x00,

  // type section: void -> void (pass, ofs: 197, len: 14)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x04, 0x01, 0x60, 0x00, 0x00,

  // type section: i32, i64 -> f32, f64 (pass, ofs: 211, len: 18)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x01, 0x08, 0x01, 0x60, 0x02, 0X7F, 0x7E, 0x02,
  0x7D, 0x7C,

  // import section: blank (pass, ofs: 229, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x02, 0x01, 0x00,

  // import func: ".", id: 0 (pass, ofs: 240, len: 15)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x02, 0x05, 0x01, 0x00, 0x00, 0x00, 0x00,

  // import func: "foo.bar", id: 1 (pass, ofs: 255, len: 21)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x02, 0x0b, 0x01, 0x03, 'f',  'o',  'o',  0x03,
  'b',  'a',  'r',  0x00, 0x01,

  // import funcs: foo.bar, bar.blum (pass, ofs: 276, len: 32)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x02, 0x16, 0x02, 0x03, 'f',  'o',  'o',  0x03,
  'b',  'a',  'r',  0x00, 0x00, 0x02, 'h',  'i',
  0x05, 't',  'h',  'e', 'r',   'e',  0x00, 0x01,

  // import table: ".", min: 0 (pass, ofs: 308, len: 17)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x02, 0x07, 0x01, 0x00, 0x00, 0x01, 0x70, 0x00,
  0x00,

  // import mem: uh.oh, 10-20 (pass, ofs: 325, len: 22)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x02, 0x0C, 0x01, 0x02, 'u',  'h',  0x02, 'o',
  'h',  0x02, 0x01, 0x0A, 0x80, 0x01,

  // import globals: z.a, z.b, z.c (pass, ofs: 347, len: 32)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x02, 0x16, 0x03, 0x01, 'z',  0x01, 'a',  0x03,
  0x7F, 0x00, 0x01, 'z',  0x01, 'b',  0x03, 0x7E,
  0x01, 0x01, 'z',  0x01, 'c',  0x03, 0x7D, 0x00,

  // function section: blank (pass, ofs: 379, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x03, 0x01, 0x00,

  // function section: 1 (pass, ofs: 390, len: 12)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x03, 0x02, 0x01, 0x00,

  // function section: 3 long (pass, ofs: 402, len: 26)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x03, 0x10, 0x03, 0x80, 0x80, 0x80, 0x80, 0x01,
  0x81, 0x80, 0x80, 0x80, 0x01, 0x82, 0x80, 0x80,
  0x80, 0x01,

  // function section: bad long (fail, ofs: 428, len: 16)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x03, 0x06, 0x01, 0x80, 0x80, 0x80, 0x80, 0x81,

  // table section: blank (fail, ofs: 444, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x04, 0x00,

  // table section: one short (fail, ofs: 454, len: 12)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x04, 0x02, 0x01, 0x00,

  // table section: one bad type (fail, ofs: 466, len: 14)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x04, 0x04, 0x01, 0x00, 0x00, 0x00,

  // table section: one (pass, ofs: 480, len: 14)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x04, 0x04, 0x01, 0x70, 0x00, 0x00,

  // table section: one big (pass, ofs: 494, len: 18)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x04, 0x08, 0x01, 0x70, 0x00, 0x80, 0x80, 0x80,
  0x80, 0x01,

  // table section: one big pair (pass, ofs: 512, len: 23)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x04, 0x0D, 0x01, 0x70, 0x01, 0x80, 0x80, 0x80,
  0x80, 0x01, 0x80, 0x80, 0x80, 0x80, 0x01,

  // table section: 3 pairs (pass, ofs: 535, len: 47)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x04, 0x25, 0x03, 0x70, 0x01, 0x80, 0x80, 0x80,
  0x80, 0x01, 0x81, 0x80, 0x80, 0x80, 0x01, 0x70,
  0x01, 0x82, 0x80, 0x80, 0x80, 0x01, 0x83, 0x80,
  0x80, 0x80, 0x01, 0x70, 0x01, 0x83, 0x80, 0x80,
  0x80, 0x01, 0x84, 0x80, 0x80, 0x80, 0x01,
};

typedef struct {
  char *name;
  bool want;
  size_t ofs;
  size_t len;
} test_t;

static const test_t TESTS[] = {
  { "short length",                       false,    0,    0 },
  { "bad header",                         false,    0,    8 },
  { "good header",                        true,     8,    8 },
  { "custom section: blank",              true,    16,   11 },
  { "custom section: no length",          false,   27,    9 },
  { "custom section: name truncated",     false,   36,   11 },
  { "custom section: hello",              true,    47,   16 },
  { "custom section: hello, there",       true,    63,   21 },
  { "custom section: \"\", world",        true,    84,   16 },
  { "type section: partial",              false,  100,   10 },
  { "type section: empty",                true,   110,   11 },
  { "type section: i32 -> void",          true,   121,   15 },
  { "type section: junk -> void",         false,  136,   15 },
  { "type section: void -> i32",          true,   151,   15 },
  { "type section: void -> junk",         false,  166,   15 },
  { "type section: i32, f32 -> void",     true,   181,   16 },
  { "type section: void -> void",         true,   197,   14 },
  { "type section: i32, i64 -> f32, f64", true,   211,   18 },
  { "import section: blank",              true,   229,   11 },
  { "import func: \".\", id: 0",          true,   240,   15 },
  { "import func: \"foo.bar\", id: 1",    true,   255,   21 },
  { "import funcs: foo.bar, bar.blum",    true,   276,   32 },
  { "import table: \".\", min: 0",        true,   308,   17 },
  { "import mem: uh.oh, 10-20",           true,   325,   22 },
  { "import globals: z.a, z.b, z.c",      true,   347,   32 },
  { "function section: blank",            true,   379,   11 },
  { "function section: 1",                true,   390,   12 },
  { "function section: 3 long",           true,   402,   26 },
  { "function section: bad long",         false,  428,   16 },
  { "table section: blank",               false,  444,   10 },
  { "table section: one short",           false,  454,   12 },
  { "table section: one bad type",        false,  466,   14 },
  { "table section: one",                 true,   480,   14 },
  { "table section: one big",             true,   494,   18 },
  { "table section: one big pair",        true,   512,   23 },
  { "table section: 3 pairs",             true,   535,   47 },
};

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
  fprintf(fh, "\" (%s): ", pt_wasm_import_desc_get_name(im->import_desc));

  switch (im->import_desc) {
  case PT_WASM_IMPORT_DESC_FUNC:
    fprintf(fh, "id = %u", im->func.id);
    break;
  case PT_WASM_IMPORT_DESC_TABLE:
    fprintf(fh, "elem_type = %u, ", im->table.elem_type);
    dump_limits(fh, &(im->table.limits));
    break;
  case PT_WASM_IMPORT_DESC_MEM:
    dump_limits(fh, &(im->mem.limits));
    break;
  case PT_WASM_IMPORT_DESC_GLOBAL:
    {
      const char * const name = pt_wasm_value_type_get_name(im->global.type);
      const char mut = im->global.mutable ? 't' : 'f';
      fprintf(fh, "type = %s, mutable = %c", name, mut);
    }
    break;
  default:
    errx(EXIT_FAILURE, "invalid import_desc: %u", im->import_desc);
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
  .on_error           = on_test_error,
};

static bool
run_tests(void) {
  const size_t NUM_TESTS = NUM_ITEMS(TESTS);
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
