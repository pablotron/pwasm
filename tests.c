#include <stdbool.h> // bool
#include <stdint.h> // uint8_t
#include "tests.h"

#define NUM_ITEMS(ary) (sizeof(ary) / sizeof(ary[0]))

static const uint8_t TEST_DATA[] = {
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

  // memory section: blank (fail, ofs: 582, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x05, 0x00,

  // memory section: empty (pass, ofs: 592, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x05, 0x01, 0x00,

  // memory section: one (pass, ofs: 603, len: 13)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x05, 0x03, 0x01, 0x00, 0x00,

  // memory section: 3 pairs (pass, ofs: 616, len: 44)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x05, 0x22, 0x03, 0x01, 0x80, 0x80, 0x80, 0x80,
  0x08, 0x81, 0x80, 0x80, 0x80, 0x08, 0x01, 0x82,
  0x80, 0x80, 0x80, 0x08, 0x83, 0x80, 0x80, 0x80,
  0x08, 0x01, 0x84, 0x80, 0x80, 0x80, 0x08, 0x85,
  0x80, 0x80, 0x80, 0x08,

  // global section: blank (fail, ofs: 660, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x06, 0x00,

  // global section: empty (pass, ofs: 670, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x06, 0x01, 0x00,

  // global section: one mut i32 (pass, ofs: 681, len: 16)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x06, 0x06, 0x01, 0x7F, 0x01, 0x41, 0x02, 0x0B,

  // global section: one i64 (pass, ofs: 697, len: 16)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x06, 0x06, 0x01, 0x7E, 0x00, 0x42, 0x28, 0x0B,

  // global section: one f32 (pass, ofs: 713, len: 19)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x06, 0x09, 0x01, 0x7D, 0x00, 0x43, 0x00, 0x00,
  0x00, 0x00, 0x0B,

  // global section: one f64 (pass, ofs: 732, len: 23)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x06, 0x0D, 0x01, 0x7C, 0x00, 0x44, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0B,

  // global section: f32 pi, e (pass, ofs: 755, len: 27)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x06, 0x11, 0x02, 0x7D, 0x00, 0x43, 0xDB, 0x0F,
  0x49, 0x40, 0x0B, 0x7D, 0x00, 0x43, 0x54, 0xF8,
  0x2D, 0x40, 0x0B,

  // export section: blank (fail, ofs: 782, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x07, 0x00,

  // export section: empty (pass, ofs: 792, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x07, 0x01, 0x00,

  // exports: foo, bar, baz, blum (pass, ofs: 803, len: 36)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x07, 0x1A, 0x03, 0x03, 'f',  'o',  'o',  0x00,
  0x01, 0x03, 'b',  'a',  'r',  0x01, 0x02, 0x03,
  'b',  'a',  'z',  0x02, 0x03, 0x04, 'b',  'l',
  'u',  'm',  0x03, 0x04,

  // duplicate section test (fail, ofs: 839, len: 14)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x07, 0x01, 0x00, 0x07, 0x01, 0x00,

  // element section: blank (fail, ofs: 853, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x08, 0x00,

  // element section: empty (pass, ofs: 863, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x09, 0x01, 0x00,

  // element section: one (pass, ofs: 874, len: 14)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x09, 0x04, 0x01, 0x00, 0x0B, 0x00,

  // element section: two fns (pass, ofs: 888, len: 16)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x09, 0x06, 0x01, 0x01, 0x0B, 0x02, 0x02, 0x03,

  // two elements, two fns (pass, ofs: 904, len: 21)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x09, 0x0B, 0x02, 0x01, 0x0B, 0x02, 0x02, 0x03,
  0x04, 0x0B, 0x02, 0x05, 0x06,

  // two elements, two i32s (pass, ofs: 925, len: 25)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x09, 0x0F, 0x02, 0x01, 0x41, 0x00, 0x0B, 0x02,
  0x02, 0x03, 0x04, 0x41, 0x01, 0x0B, 0x02, 0x05,
  0x06,

  // code section: blank (fail, ofs: 950, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x0A, 0x00,

  // code section: empty (pass, ofs: 960, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x0A, 0x01, 0x00,

  // data section: blank (fail, ofs: 971, len: 10)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x0B, 0x00,

  // data section: empty (pass, ofs: 981, len: 11)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x0B, 0x01, 0x00,

  // data section: 10 bytes (pass, ofs: 992, len: 26)
  0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
  0x0B, 0x10, 0x01, 0x00, 0x41, 0x2A, 0x0B, 0x0A,
  0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
  0x08, 0x09,
};

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
  { "memory section: blank",              false,  582,   10 },
  { "memory section: empty",              true,   592,   11 },
  { "memory section: one",                true,   603,   13 },
  { "memory section: 3 pairs",            true,   616,   44 },
  { "global section: blank",              false,  660,   10 },
  { "global section: empty",              true,   670,   11 },
  { "global section: one mut i32",        true,   681,   16 },
  { "global section: one i64",            true,   697,   16 },
  { "global section: one f32",            true,   713,   19 },
  { "global section: one f64",            true,   732,   23 },
  { "global section: f32 pi, e",          true,   755,   27 },
  { "export section: blank",              false,  782,   10 },
  { "export section: empty",              true,   792,   11 },
  { "exports: foo, bar, baz, blum",       true,   803,   36 },
  { "duplicate section test",             false,  839,   14 },
  { "element section: blank",             false,  853,   10 },
  { "element section: empty",             true,   863,   11 },
  { "element section: one",               true,   874,   14 },
  { "element section: two fns",           true,   888,   16 },
  { "two elements, two fns",              true,   904,   21 },
  { "two elements, two i32s",             true,   925,   25 },
  { "code section: blank",                false,  950,   10 },
  { "code section: empty",                true,   960,   11 },
  { "data section: blank",                false,  971,   10 },
  { "data section: empty",                true,   981,   11 },
  { "data section: 10 bytes",             true,   992,   26 },
};

size_t get_num_tests(void) {
  return NUM_ITEMS(TESTS);
}

const test_t *get_tests(void) {
  return TESTS;
}

const uint8_t *get_test_data(void) {
  return TEST_DATA;
}
