#ifndef PT_WASM_H
#define PT_WASM_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h> // size_t
#include <stdint.h> // uint8_t, uint32_t, etc

typedef struct {
  const uint8_t *ptr;
  size_t len;
} pt_wasm_buf_t;

#define PT_WASM_SECTION_TYPES \
  PT_WASM_SECTION_TYPE(CUSTOM, "custom") \
  PT_WASM_SECTION_TYPE(TYPE, "type") \
  PT_WASM_SECTION_TYPE(IMPORT, "import") \
  PT_WASM_SECTION_TYPE(FUNCTION, "function") \
  PT_WASM_SECTION_TYPE(TABLE, "table") \
  PT_WASM_SECTION_TYPE(MEMORY, "memory") \
  PT_WASM_SECTION_TYPE(GLOBAL, "global") \
  PT_WASM_SECTION_TYPE(EXPORT, "export") \
  PT_WASM_SECTION_TYPE(START, "start") \
  PT_WASM_SECTION_TYPE(ELEMENT, "element") \
  PT_WASM_SECTION_TYPE(CODE, "code") \
  PT_WASM_SECTION_TYPE(DATA, "data") \
  PT_WASM_SECTION_TYPE(LAST, "unknown section")

#define PT_WASM_SECTION_TYPE(a, b) PT_WASM_SECTION_TYPE_##a,
typedef enum {
PT_WASM_SECTION_TYPES
} pt_wasm_section_type_t;
#undef PT_WASM_SECTION_TYPE

/**
 * Get name of section type.
 *
 * Returns a pointer to the null-terminated name of a section, or the
 * string "unknown section" if the given section type is unknown.
 *
 * Note: The strings returned by this function should not be freed.
 */
const char *pt_wasm_section_type_get_name(const pt_wasm_section_type_t);

typedef struct {
  pt_wasm_buf_t name;
  pt_wasm_buf_t data;
} pt_wasm_custom_section_t;

typedef struct {
  pt_wasm_buf_t params;
  pt_wasm_buf_t results;
} pt_wasm_function_type_t;

typedef struct {
  uint32_t min;
  uint32_t max;
  _Bool has_max;
} pt_wasm_limits_t;

typedef uint32_t pt_wasm_table_elem_type_t;

typedef struct {
  pt_wasm_table_elem_type_t elem_type; /* must be 0x70 */
  pt_wasm_limits_t limits;
} pt_wasm_table_t;

typedef uint32_t pt_wasm_value_type_t;

const char *pt_wasm_value_type_get_name(const pt_wasm_value_type_t);

typedef uint32_t pt_wasm_result_type_t;

const char *pt_wasm_result_type_get_name(const pt_wasm_result_type_t);

#define PT_WASM_OP_DEFS \
  /* 0x00 */ PT_WASM_OP(UNREACHABLE, "unreachable", NONE) \
  /* 0x01 */ PT_WASM_OP(NOP, "nop", NONE) \
  /* 0x02 */ PT_WASM_OP_CONTROL(BLOCK, "block", BLOCK) \
  /* 0x03 */ PT_WASM_OP_CONTROL(LOOP, "loop", BLOCK) \
  /* 0x04 */ PT_WASM_OP_CONTROL(IF, "if", BLOCK) \
  /* 0x05 */ PT_WASM_OP(ELSE, "else", NONE) \
  /* 0x06 */ PT_WASM_OP_RESERVED(RESERVED_06, "06") \
  /* 0x07 */ PT_WASM_OP_RESERVED(RESERVED_07, "07") \
  /* 0x08 */ PT_WASM_OP_RESERVED(RESERVED_08, "08") \
  /* 0x09 */ PT_WASM_OP_RESERVED(RESERVED_09, "09") \
  /* 0x0A */ PT_WASM_OP_RESERVED(RESERVED_0A, "0A") \
  /* 0x0B */ PT_WASM_OP_CONST(END, "end", NONE) \
  /* 0x0C */ PT_WASM_OP(BR, "br", INDEX) \
  /* 0x0D */ PT_WASM_OP(BR_IF, "br_if", INDEX) \
  /* 0x0E */ PT_WASM_OP(BR_TABLE, "br_table", BR_TABLE) \
  /* 0x0F */ PT_WASM_OP(RETURN, "return", NONE) \
  /* 0x10 */ PT_WASM_OP(CALL, "call", INDEX) \
  /* 0x11 */ PT_WASM_OP(CALL_INDIRECT, "call_indirect", CALL_INDIRECT) \
  /* 0x12 */ PT_WASM_OP_RESERVED(RESERVED_12, "12") \
  /* 0x13 */ PT_WASM_OP_RESERVED(RESERVED_13, "13") \
  /* 0x14 */ PT_WASM_OP_RESERVED(RESERVED_14, "14") \
  /* 0x15 */ PT_WASM_OP_RESERVED(RESERVED_15, "15") \
  /* 0x16 */ PT_WASM_OP_RESERVED(RESERVED_16, "16") \
  /* 0x17 */ PT_WASM_OP_RESERVED(RESERVED_17, "17") \
  /* 0x18 */ PT_WASM_OP_RESERVED(RESERVED_18, "18") \
  /* 0x19 */ PT_WASM_OP_RESERVED(RESERVED_19, "19") \
  /* 0x1A */ PT_WASM_OP(DROP, "drop", NONE) \
  /* 0x1B */ PT_WASM_OP(SELECT, "select", NONE) \
  /* 0x1C */ PT_WASM_OP_RESERVED(RESERVED_1C, "1c") \
  /* 0x1D */ PT_WASM_OP_RESERVED(RESERVED_1D, "1d") \
  /* 0x1E */ PT_WASM_OP_RESERVED(RESERVED_1E, "1e") \
  /* 0x1F */ PT_WASM_OP_RESERVED(RESERVED_1F, "1f") \
  /* 0x20 */ PT_WASM_OP(LOCAL_GET, "local.get", INDEX) \
  /* 0x21 */ PT_WASM_OP(LOCAL_SET, "local.set", INDEX) \
  /* 0x22 */ PT_WASM_OP(LOCAL_TEE, "local.tee", INDEX) \
  /* 0x23 */ PT_WASM_OP_CONST(GLOBAL_GET, "global.get", INDEX) \
  /* 0x24 */ PT_WASM_OP(GLOBAL_SET, "global.set", INDEX) \
  /* 0x25 */ PT_WASM_OP_RESERVED(RESERVED_25, "25") \
  /* 0x26 */ PT_WASM_OP_RESERVED(RESERVED_26, "26") \
  /* 0x27 */ PT_WASM_OP_RESERVED(RESERVED_27, "27") \
  /* 0x28 */ PT_WASM_OP(I32_LOAD, "i32.load", MEM) \
  /* 0x29 */ PT_WASM_OP(I64_LOAD, "i64.load", MEM) \
  /* 0x2A */ PT_WASM_OP(F32_LOAD, "f32.load", MEM) \
  /* 0x2B */ PT_WASM_OP(F64_LOAD, "f64.load", MEM) \
  /* 0x2C */ PT_WASM_OP(I32_LOAD8_S, "i32.load8_s", MEM) \
  /* 0x2D */ PT_WASM_OP(I32_LOAD8_U, "i32.load8_u", MEM) \
  /* 0x2E */ PT_WASM_OP(I32_LOAD16_S, "i32.load16_s", MEM) \
  /* 0x2F */ PT_WASM_OP(I32_LOAD16_U, "i32.load16_u", MEM) \
  /* 0x30 */ PT_WASM_OP(I64_LOAD8_S, "i64.load8_s", MEM) \
  /* 0x31 */ PT_WASM_OP(I64_LOAD8_U, "i64.load8_u", MEM) \
  /* 0x32 */ PT_WASM_OP(I64_LOAD16_S, "i64.load16_s", MEM) \
  /* 0x33 */ PT_WASM_OP(I64_LOAD16_U, "i64.load16_u", MEM) \
  /* 0x34 */ PT_WASM_OP(I64_LOAD32_S, "i64.load32_s", MEM) \
  /* 0x35 */ PT_WASM_OP(I64_LOAD32_U, "i64.load32_u", MEM) \
  /* 0x36 */ PT_WASM_OP(I32_STORE, "i32.store", MEM) \
  /* 0x37 */ PT_WASM_OP(I64_STORE, "i64.store", MEM) \
  /* 0x38 */ PT_WASM_OP(F32_STORE, "f32.store", MEM) \
  /* 0x39 */ PT_WASM_OP(F64_STORE, "f64.store", MEM) \
  /* 0x3A */ PT_WASM_OP(I32_STORE8, "i32.store8", MEM) \
  /* 0x3B */ PT_WASM_OP(I32_STORE16, "i32.store16", MEM) \
  /* 0x3C */ PT_WASM_OP(I64_STORE8, "i64.store8", MEM) \
  /* 0x3D */ PT_WASM_OP(I64_STORE16, "i64.store16", MEM) \
  /* 0x3E */ PT_WASM_OP(I64_STORE32, "i64.store32", MEM) \
  /* 0x3F */ PT_WASM_OP(MEMORY_SIZE, "memory.size", NONE) \
  /* 0x40 */ PT_WASM_OP(MEMORY_GROW, "memory.grow", NONE) \
  /* 0x41 */ PT_WASM_OP_CONST(I32_CONST, "i32.const", I32_CONST) \
  /* 0x42 */ PT_WASM_OP_CONST(I64_CONST, "i64.const", I64_CONST) \
  /* 0x43 */ PT_WASM_OP_CONST(F32_CONST, "f32.const", F32_CONST) \
  /* 0x44 */ PT_WASM_OP_CONST(F64_CONST, "f64.const", F64_CONST) \
  /* 0x45 */ PT_WASM_OP(I32_EQZ, "i32.eqz", NONE) \
  /* 0x46 */ PT_WASM_OP(I32_EQ, "i32.eq", NONE) \
  /* 0x47 */ PT_WASM_OP(I32_NE, "i32.ne", NONE) \
  /* 0x48 */ PT_WASM_OP(I32_LT_S, "i32.lt_s", NONE) \
  /* 0x49 */ PT_WASM_OP(I32_LT_U, "i32.lt_u", NONE) \
  /* 0x4A */ PT_WASM_OP(I32_GT_S, "i32.gt_s", NONE) \
  /* 0x4B */ PT_WASM_OP(I32_GT_U, "i32.gt_u", NONE) \
  /* 0x4C */ PT_WASM_OP(I32_LE_S, "i32.le_s", NONE) \
  /* 0x4D */ PT_WASM_OP(I32_LE_U, "i32.le_u", NONE) \
  /* 0x4E */ PT_WASM_OP(I32_GE_S, "i32.ge_s", NONE) \
  /* 0x4F */ PT_WASM_OP(I32_GE_U, "i32.ge_u", NONE) \
  /* 0x50 */ PT_WASM_OP(I64_EQZ, "i64.eqz", NONE) \
  /* 0x51 */ PT_WASM_OP(I64_EQ, "i64.eq", NONE) \
  /* 0x52 */ PT_WASM_OP(I64_NE, "i64.ne", NONE) \
  /* 0x53 */ PT_WASM_OP(I64_LT_S, "i64.lt_s", NONE) \
  /* 0x54 */ PT_WASM_OP(I64_LT_U, "i64.lt_u", NONE) \
  /* 0x55 */ PT_WASM_OP(I64_GT_S, "i64.gt_s", NONE) \
  /* 0x56 */ PT_WASM_OP(I64_GT_U, "i64.gt_u", NONE) \
  /* 0x57 */ PT_WASM_OP(I64_LE_S, "i64.le_s", NONE) \
  /* 0x58 */ PT_WASM_OP(I64_LE_U, "i64.le_u", NONE) \
  /* 0x59 */ PT_WASM_OP(I64_GE_S, "i64.ge_s", NONE) \
  /* 0x5A */ PT_WASM_OP(I64_GE_U, "i64.ge_u", NONE) \
  /* 0x5B */ PT_WASM_OP(F32_EQ, "f32.eq", NONE) \
  /* 0x5C */ PT_WASM_OP(F32_NE, "f32.ne", NONE) \
  /* 0x5D */ PT_WASM_OP(F32_LT, "f32.lt", NONE) \
  /* 0x5E */ PT_WASM_OP(F32_GT, "f32.gt", NONE) \
  /* 0x5F */ PT_WASM_OP(F32_LE, "f32.le", NONE) \
  /* 0x60 */ PT_WASM_OP(F32_GE, "f32.ge", NONE) \
  /* 0x61 */ PT_WASM_OP(F64_EQ, "f64.eq", NONE) \
  /* 0x62 */ PT_WASM_OP(F64_NE, "f64.ne", NONE) \
  /* 0x63 */ PT_WASM_OP(F64_LT, "f64.lt", NONE) \
  /* 0x64 */ PT_WASM_OP(F64_GT, "f64.gt", NONE) \
  /* 0x65 */ PT_WASM_OP(F64_LE, "f64.le", NONE) \
  /* 0x66 */ PT_WASM_OP(F64_GE, "f64.ge", NONE) \
  /* 0x67 */ PT_WASM_OP(I32_CLZ, "i32.clz", NONE) \
  /* 0x68 */ PT_WASM_OP(I32_CTZ, "i32.ctz", NONE) \
  /* 0x69 */ PT_WASM_OP(I32_POPCNT, "i32.popcnt", NONE) \
  /* 0x6A */ PT_WASM_OP(I32_ADD, "i32.add", NONE) \
  /* 0x6B */ PT_WASM_OP(I32_SUB, "i32.sub", NONE) \
  /* 0x6C */ PT_WASM_OP(I32_MUL, "i32.mul", NONE) \
  /* 0x6D */ PT_WASM_OP(I32_DIV_S, "i32.div_s", NONE) \
  /* 0x6E */ PT_WASM_OP(I32_DIV_U, "i32.div_u", NONE) \
  /* 0x6F */ PT_WASM_OP(I32_REM_S, "i32.rem_s", NONE) \
  /* 0x70 */ PT_WASM_OP(I32_REM_U, "i32.rem_u", NONE) \
  /* 0x71 */ PT_WASM_OP(I32_AND, "i32.and", NONE) \
  /* 0x72 */ PT_WASM_OP(I32_OR, "i32.or", NONE) \
  /* 0x73 */ PT_WASM_OP(I32_XOR, "i32.xor", NONE) \
  /* 0x74 */ PT_WASM_OP(I32_SHL, "i32.shl", NONE) \
  /* 0x75 */ PT_WASM_OP(I32_SHR_S, "i32.shr_s", NONE) \
  /* 0x76 */ PT_WASM_OP(I32_SHR_U, "i32.shr_u", NONE) \
  /* 0x77 */ PT_WASM_OP(I32_ROTL, "i32.rotl", NONE) \
  /* 0x78 */ PT_WASM_OP(I32_ROTR, "i32.rotr", NONE) \
  /* 0x79 */ PT_WASM_OP(I64_CLZ, "i64.clz", NONE) \
  /* 0x7A */ PT_WASM_OP(I64_CTZ, "i64.ctz", NONE) \
  /* 0x7B */ PT_WASM_OP(I64_POPCNT, "i64.popcnt", NONE) \
  /* 0x7C */ PT_WASM_OP(I64_ADD, "i64.add", NONE) \
  /* 0x7D */ PT_WASM_OP(I64_SUB, "i64.sub", NONE) \
  /* 0x7E */ PT_WASM_OP(I64_MUL, "i64.mul", NONE) \
  /* 0x7F */ PT_WASM_OP(I64_DIV_S, "i64.div_s", NONE) \
  /* 0x80 */ PT_WASM_OP(I64_DIV_U, "i64.div_u", NONE) \
  /* 0x81 */ PT_WASM_OP(I64_REM_S, "i64.rem_s", NONE) \
  /* 0x82 */ PT_WASM_OP(I64_REM_U, "i64.rem_u", NONE) \
  /* 0x83 */ PT_WASM_OP(I64_AND, "i64.and", NONE) \
  /* 0x84 */ PT_WASM_OP(I64_OR, "i64.or", NONE) \
  /* 0x85 */ PT_WASM_OP(I64_XOR, "i64.xor", NONE) \
  /* 0x86 */ PT_WASM_OP(I64_SHL, "i64.shl", NONE) \
  /* 0x87 */ PT_WASM_OP(I64_SHR_S, "i64.shr_s", NONE) \
  /* 0x88 */ PT_WASM_OP(I64_SHR_U, "i64.shr_u", NONE) \
  /* 0x89 */ PT_WASM_OP(I64_ROTL, "i64.rotl", NONE) \
  /* 0x8A */ PT_WASM_OP(I64_ROTR, "i64.rotr", NONE) \
  /* 0x8B */ PT_WASM_OP(F32_ABS, "f32.abs", NONE) \
  /* 0x8C */ PT_WASM_OP(F32_NEG, "f32.neg", NONE) \
  /* 0x8D */ PT_WASM_OP(F32_CEIL, "f32.ceil", NONE) \
  /* 0x8E */ PT_WASM_OP(F32_FLOOR, "f32.floor", NONE) \
  /* 0x8F */ PT_WASM_OP(F32_TRUNC, "f32.trunc", NONE) \
  /* 0x90 */ PT_WASM_OP(F32_NEAREST, "f32.nearest", NONE) \
  /* 0x91 */ PT_WASM_OP(F32_SQRT, "f32.sqrt", NONE) \
  /* 0x92 */ PT_WASM_OP(F32_ADD, "f32.add", NONE) \
  /* 0x93 */ PT_WASM_OP(F32_SUB, "f32.sub", NONE) \
  /* 0x94 */ PT_WASM_OP(F32_MUL, "f32.mul", NONE) \
  /* 0x95 */ PT_WASM_OP(F32_DIV, "f32.div", NONE) \
  /* 0x96 */ PT_WASM_OP(F32_MIN, "f32.min", NONE) \
  /* 0x97 */ PT_WASM_OP(F32_MAX, "f32.max", NONE) \
  /* 0x98 */ PT_WASM_OP(F32_COPYSIGN, "f32.copysign", NONE) \
  /* 0x99 */ PT_WASM_OP(F64_ABS, "f64.abs", NONE) \
  /* 0x9A */ PT_WASM_OP(F64_NEG, "f64.neg", NONE) \
  /* 0x9B */ PT_WASM_OP(F64_CEIL, "f64.ceil", NONE) \
  /* 0x9C */ PT_WASM_OP(F64_FLOOR, "f64.floor", NONE) \
  /* 0x9D */ PT_WASM_OP(F64_TRUNC, "f64.trunc", NONE) \
  /* 0x9E */ PT_WASM_OP(F64_NEAREST, "f64.nearest", NONE) \
  /* 0x9F */ PT_WASM_OP(F64_SQRT, "f64.sqrt", NONE) \
  /* 0xA0 */ PT_WASM_OP(F64_ADD, "f64.add", NONE) \
  /* 0xA1 */ PT_WASM_OP(F64_SUB, "f64.sub", NONE) \
  /* 0xA2 */ PT_WASM_OP(F64_MUL, "f64.mul", NONE) \
  /* 0xA3 */ PT_WASM_OP(F64_DIV, "f64.div", NONE) \
  /* 0xA4 */ PT_WASM_OP(F64_MIN, "f64.min", NONE) \
  /* 0xA5 */ PT_WASM_OP(F64_MAX, "f64.max", NONE) \
  /* 0xA6 */ PT_WASM_OP(F64_COPYSIGN, "f64.copysign", NONE) \
  /* 0xA7 */ PT_WASM_OP(I32_WRAP_I64, "i32.wrap_i64", NONE) \
  /* 0xA8 */ PT_WASM_OP(I32_TRUNC_F32_S, "i32.trunc_f32_s", NONE) \
  /* 0xA9 */ PT_WASM_OP(I32_TRUNC_F32_U, "i32.trunc_f32_u", NONE) \
  /* 0xAA */ PT_WASM_OP(I32_TRUNC_F64_S, "i32.trunc_f64_s", NONE) \
  /* 0xAB */ PT_WASM_OP(I32_TRUNC_F64_U, "i32.trunc_f64_u", NONE) \
  /* 0xAC */ PT_WASM_OP(I64_EXTEND_I32_S, "i64.extend_i32_s", NONE) \
  /* 0xAD */ PT_WASM_OP(I64_EXTEND_I32_U, "i64.extend_i32_u", NONE) \
  /* 0xAE */ PT_WASM_OP(I64_TRUNC_F32_S, "i64.trunc_f32_s", NONE) \
  /* 0xAF */ PT_WASM_OP(I64_TRUNC_F32_U, "i64.trunc_f32_u", NONE) \
  /* 0xB0 */ PT_WASM_OP(I64_TRUNC_F64_S, "i64.trunc_f64_s", NONE) \
  /* 0xB1 */ PT_WASM_OP(I64_TRUNC_F64_U, "i64.trunc_f64_u", NONE) \
  /* 0xB2 */ PT_WASM_OP(F32_CONVERT_I32_S, "f32.convert_i32_s", NONE) \
  /* 0xB3 */ PT_WASM_OP(F32_CONVERT_I32_U, "f32.convert_i32_u", NONE) \
  /* 0xB4 */ PT_WASM_OP(F32_CONVERT_I64_S, "f32.convert_i64_s", NONE) \
  /* 0xB5 */ PT_WASM_OP(F32_CONVERT_I64_U, "f32.convert_i64_u", NONE) \
  /* 0xB6 */ PT_WASM_OP(F32_DEMOTE_F64, "f32.demote_f64", NONE) \
  /* 0xB7 */ PT_WASM_OP(F64_CONVERT_I32_S, "f64.convert_i32_s", NONE) \
  /* 0xB8 */ PT_WASM_OP(F64_CONVERT_I32_U, "f64.convert_i32_u", NONE) \
  /* 0xB9 */ PT_WASM_OP(F64_CONVERT_I64_S, "f64.convert_i64_s", NONE) \
  /* 0xBA */ PT_WASM_OP(F64_CONVERT_I64_U, "f64.convert_i64_u", NONE) \
  /* 0xBB */ PT_WASM_OP(F64_PROMOTE_F32, "f64.promote_f32", NONE) \
  /* 0xBC */ PT_WASM_OP(I32_REINTERPRET_F32, "i32.reinterpret_f32", NONE) \
  /* 0xBD */ PT_WASM_OP(I64_REINTERPRET_F64, "i64.reinterpret_f64", NONE) \
  /* 0xBE */ PT_WASM_OP(F32_REINTERPRET_I32, "f32.reinterpret_i32", NONE) \
  /* 0xBF */ PT_WASM_OP(F64_REINTERPRET_I64, "f64.reinterpret_i64", NONE) \
  /* 0xC0 */ PT_WASM_OP_RESERVED(RESERVED_C0, "c0") \
  /* 0xC1 */ PT_WASM_OP_RESERVED(RESERVED_C1, "c1") \
  /* 0xC2 */ PT_WASM_OP_RESERVED(RESERVED_C2, "c2") \
  /* 0xC3 */ PT_WASM_OP_RESERVED(RESERVED_C3, "c3") \
  /* 0xC4 */ PT_WASM_OP_RESERVED(RESERVED_C4, "c4") \
  /* 0xC5 */ PT_WASM_OP_RESERVED(RESERVED_C5, "c5") \
  /* 0xC6 */ PT_WASM_OP_RESERVED(RESERVED_C6, "c6") \
  /* 0xC7 */ PT_WASM_OP_RESERVED(RESERVED_C7, "c7") \
  /* 0xC8 */ PT_WASM_OP_RESERVED(RESERVED_C8, "c8") \
  /* 0xC9 */ PT_WASM_OP_RESERVED(RESERVED_C9, "c9") \
  /* 0xCA */ PT_WASM_OP_RESERVED(RESERVED_CA, "ca") \
  /* 0xCB */ PT_WASM_OP_RESERVED(RESERVED_CB, "cb") \
  /* 0xCC */ PT_WASM_OP_RESERVED(RESERVED_CC, "cc") \
  /* 0xCD */ PT_WASM_OP_RESERVED(RESERVED_CD, "cd") \
  /* 0xCE */ PT_WASM_OP_RESERVED(RESERVED_CE, "ce") \
  /* 0xCF */ PT_WASM_OP_RESERVED(RESERVED_CF, "cf") \
  /* 0xD0 */ PT_WASM_OP_RESERVED(RESERVED_D0, "d0") \
  /* 0xD1 */ PT_WASM_OP_RESERVED(RESERVED_D1, "d1") \
  /* 0xD2 */ PT_WASM_OP_RESERVED(RESERVED_D2, "d2") \
  /* 0xD3 */ PT_WASM_OP_RESERVED(RESERVED_D3, "d3") \
  /* 0xD4 */ PT_WASM_OP_RESERVED(RESERVED_D4, "d4") \
  /* 0xD5 */ PT_WASM_OP_RESERVED(RESERVED_D5, "d5") \
  /* 0xD6 */ PT_WASM_OP_RESERVED(RESERVED_D6, "d6") \
  /* 0xD7 */ PT_WASM_OP_RESERVED(RESERVED_D7, "d7") \
  /* 0xD8 */ PT_WASM_OP_RESERVED(RESERVED_D8, "d8") \
  /* 0xD9 */ PT_WASM_OP_RESERVED(RESERVED_D9, "d9") \
  /* 0xDA */ PT_WASM_OP_RESERVED(RESERVED_DA, "da") \
  /* 0xDB */ PT_WASM_OP_RESERVED(RESERVED_DB, "db") \
  /* 0xDC */ PT_WASM_OP_RESERVED(RESERVED_DC, "dc") \
  /* 0xDD */ PT_WASM_OP_RESERVED(RESERVED_DD, "dd") \
  /* 0xDE */ PT_WASM_OP_RESERVED(RESERVED_DE, "de") \
  /* 0xDF */ PT_WASM_OP_RESERVED(RESERVED_DF, "df") \
  /* 0xE0 */ PT_WASM_OP_RESERVED(RESERVED_E0, "e0") \
  /* 0xE1 */ PT_WASM_OP_RESERVED(RESERVED_E1, "e1") \
  /* 0xE2 */ PT_WASM_OP_RESERVED(RESERVED_E2, "e2") \
  /* 0xE3 */ PT_WASM_OP_RESERVED(RESERVED_E3, "e3") \
  /* 0xE4 */ PT_WASM_OP_RESERVED(RESERVED_E4, "e4") \
  /* 0xE5 */ PT_WASM_OP_RESERVED(RESERVED_E5, "e5") \
  /* 0xE6 */ PT_WASM_OP_RESERVED(RESERVED_E6, "e6") \
  /* 0xE7 */ PT_WASM_OP_RESERVED(RESERVED_E7, "e7") \
  /* 0xE8 */ PT_WASM_OP_RESERVED(RESERVED_E8, "e8") \
  /* 0xE9 */ PT_WASM_OP_RESERVED(RESERVED_E9, "e9") \
  /* 0xEA */ PT_WASM_OP_RESERVED(RESERVED_EA, "ea") \
  /* 0xEB */ PT_WASM_OP_RESERVED(RESERVED_EB, "eb") \
  /* 0xEC */ PT_WASM_OP_RESERVED(RESERVED_EC, "ec") \
  /* 0xED */ PT_WASM_OP_RESERVED(RESERVED_ED, "ed") \
  /* 0xEE */ PT_WASM_OP_RESERVED(RESERVED_EE, "ee") \
  /* 0xEF */ PT_WASM_OP_RESERVED(RESERVED_EF, "ef") \
  /* 0xF0 */ PT_WASM_OP_RESERVED(RESERVED_F0, "f0") \
  /* 0xF1 */ PT_WASM_OP_RESERVED(RESERVED_F1, "f1") \
  /* 0xF2 */ PT_WASM_OP_RESERVED(RESERVED_F2, "f2") \
  /* 0xF3 */ PT_WASM_OP_RESERVED(RESERVED_F3, "f3") \
  /* 0xF4 */ PT_WASM_OP_RESERVED(RESERVED_F4, "f4") \
  /* 0xF5 */ PT_WASM_OP_RESERVED(RESERVED_F5, "f5") \
  /* 0xF6 */ PT_WASM_OP_RESERVED(RESERVED_F6, "f6") \
  /* 0xF7 */ PT_WASM_OP_RESERVED(RESERVED_F7, "f7") \
  /* 0xF8 */ PT_WASM_OP_RESERVED(RESERVED_F8, "f8") \
  /* 0xF9 */ PT_WASM_OP_RESERVED(RESERVED_F9, "f9") \
  /* 0xFA */ PT_WASM_OP_RESERVED(RESERVED_FA, "fa") \
  /* 0xFB */ PT_WASM_OP_RESERVED(RESERVED_FB, "fb") \
  /* 0xFC */ PT_WASM_OP_RESERVED(RESERVED_FC, "fc") \
  /* 0xFD */ PT_WASM_OP_RESERVED(RESERVED_FD, "fd") \
  /* 0xFE */ PT_WASM_OP_RESERVED(RESERVED_FE, "fe") \
  /* 0xFF */ PT_WASM_OP_RESERVED(RESERVED_FF, "ff")

#define PT_WASM_OP(a, b, c) PT_WASM_OP_##a,
#define PT_WASM_OP_CONST(a, b, c) PT_WASM_OP_##a,
#define PT_WASM_OP_CONTROL(a, b, c) PT_WASM_OP##a,
#define PT_WASM_OP_RESERVED(a, b) PT_WASM_OP##a,
typedef enum {
PT_WASM_OP_DEFS
} pt_wasm_op_t;
#undef PT_WASM_OP
#undef PT_WASM_OP_CONST
#undef PT_WASM_OP_CONTROL
#undef PT_WASM_OP_RESERVED

typedef struct {
  pt_wasm_buf_t buf;
} pt_wasm_expr_t;

typedef struct {
  pt_wasm_op_t op;

  union {
    /* block, loop */
    struct {
      pt_wasm_result_type_t type;
    } v_block;

    /* br_table */
    struct {
      pt_wasm_buf_t labels;
    } v_br_table;

    /* br, br_if, call, call_indirect, local.{get,set,tee}, global.{get,set} */
    struct {
      uint32_t id;
    } v_index;

    /* {i32,i64,f32,f64}.{load,store}* */
    struct {
      uint32_t align;
      uint32_t offset;
    } v_mem;

    /* const.i32 */
    struct {
      uint32_t val;
    } v_i32;

    /* const.i64 */
    struct {
      uint64_t val;
    } v_i64;

    /* const.f32 */
    struct {
      float val;
    } v_f32;

    /* const.f64 */
    struct {
      double val;
    } v_f64;
  };
} pt_wasm_inst_t;

typedef struct {
  pt_wasm_value_type_t type;
  _Bool mutable;
} pt_wasm_global_type_t;

typedef struct {
  pt_wasm_global_type_t type;
  pt_wasm_expr_t expr;
} pt_wasm_global_t;

#define PT_WASM_IMPORT_TYPES \
  PT_WASM_IMPORT_TYPE(FUNC, "funcion") \
  PT_WASM_IMPORT_TYPE(TABLE, "table") \
  PT_WASM_IMPORT_TYPE(MEM, "memory") \
  PT_WASM_IMPORT_TYPE(GLOBAL, "global") \
  PT_WASM_IMPORT_TYPE(LAST, "unknown import desc")

#define PT_WASM_IMPORT_TYPE(a, b) PT_WASM_IMPORT_TYPE_##a,
typedef enum {
PT_WASM_IMPORT_TYPES
} pt_wasm_import_type_t;
#undef PT_WASM_IMPORT_TYPE

const char *pt_wasm_import_type_get_name(const pt_wasm_import_type_t);

typedef struct {
  pt_wasm_buf_t module;
  pt_wasm_buf_t name;
  pt_wasm_import_type_t type;

  union {
    struct {
      /* type index */
      uint32_t id;
    } func;

    pt_wasm_table_t table;

    struct {
      pt_wasm_limits_t limits;
    } mem;

    pt_wasm_global_type_t global;
  };
} pt_wasm_import_t;

#define PT_WASM_EXPORT_TYPES \
  PT_WASM_EXPORT_TYPE(FUNC, "function") \
  PT_WASM_EXPORT_TYPE(TABLE, "table") \
  PT_WASM_EXPORT_TYPE(MEM, "memory") \
  PT_WASM_EXPORT_TYPE(GLOBAL, "global") \
  PT_WASM_EXPORT_TYPE(LAST, "unknown export type")

#define PT_WASM_EXPORT_TYPE(a, b) PT_WASM_EXPORT_TYPE_##a,
typedef enum {
PT_WASM_EXPORT_TYPES
} pt_wasm_export_type_t;
#undef PT_WASM_EXPORT_TYPE

const char *pt_wasm_export_type_get_name(const pt_wasm_export_type_t);

typedef struct {
  pt_wasm_buf_t name;
  pt_wasm_export_type_t type;
  uint32_t id;
} pt_wasm_export_t;

typedef struct {
  uint32_t table_id;
  pt_wasm_expr_t expr;

  /* number of elements in func_ids */
  size_t num_func_ids;

  /* packed buffer of function ids */
  pt_wasm_buf_t func_ids;
} pt_wasm_element_t;

typedef struct {
  uint32_t mem_id;
  pt_wasm_expr_t expr;
  pt_wasm_buf_t data;
} pt_wasm_data_segment_t;

typedef struct {
  void (*on_custom_section)(const pt_wasm_custom_section_t *, void *);
  void (*on_function_types)(const pt_wasm_function_type_t *, const size_t, void *);
  void (*on_imports)(const pt_wasm_import_t *, const size_t, void *);
  void (*on_functions)(const uint32_t *, const size_t, void *);
  void (*on_tables)(const pt_wasm_table_t *, const size_t, void *);
  void (*on_memories)(const pt_wasm_limits_t *, const size_t, void *);
  void (*on_globals)(const pt_wasm_global_t *, const size_t, void *);
  void (*on_exports)(const pt_wasm_export_t *, const size_t, void *);
  void (*on_elements)(const pt_wasm_element_t *, const size_t, void *);
  void (*on_start)(const uint32_t, void *);
  void (*on_function_codes)(const pt_wasm_buf_t *, const size_t, void *);
  void (*on_data_segments)(const pt_wasm_data_segment_t *, const size_t, void *);

  void (*on_error)(const char *, void *);
} pt_wasm_parse_cbs_t;

_Bool pt_wasm_parse(
  const void * const src_ptr,
  const size_t src_len,
  const pt_wasm_parse_cbs_t * const cbs,
  void * const data
);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PT_WASM_H */
