#ifndef PWASM_H
#define PWASM_H

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h> // size_t
#include <stdint.h> // uint8_t, uint32_t, etc

typedef struct {
  const uint8_t *ptr;
  size_t len;
} pwasm_buf_t;

typedef struct {
  size_t ofs;
  size_t len;
} pwasm_slice_t;

#define PWASM_SECTION_TYPES \
  PWASM_SECTION_TYPE(CUSTOM, custom) \
  PWASM_SECTION_TYPE(TYPE, type) \
  PWASM_SECTION_TYPE(IMPORT, import) \
  PWASM_SECTION_TYPE(FUNCTION, func) \
  PWASM_SECTION_TYPE(TABLE, table) \
  PWASM_SECTION_TYPE(MEMORY, mem) \
  PWASM_SECTION_TYPE(GLOBAL, global) \
  PWASM_SECTION_TYPE(EXPORT, export) \
  PWASM_SECTION_TYPE(START, start) \
  PWASM_SECTION_TYPE(ELEMENT, elem) \
  PWASM_SECTION_TYPE(CODE, code) \
  PWASM_SECTION_TYPE(SEGMENT, segment) \
  PWASM_SECTION_TYPE(LAST, invalid)

#define PWASM_SECTION_TYPE(a, b) PWASM_SECTION_TYPE_##a,
typedef enum {
PWASM_SECTION_TYPES
} pwasm_section_type_t;
#undef PWASM_SECTION_TYPE

/**
 * Get name of section type.
 *
 * Returns a pointer to the null-terminated name of a section, or the
 * string "unknown section" if the given section type is unknown.
 *
 * Note: The strings returned by this function should not be freed.
 */
const char *pwasm_section_type_get_name(const pwasm_section_type_t);

typedef struct {
  uint32_t min;
  uint32_t max;
  _Bool has_max;
} pwasm_limits_t;

typedef uint32_t pwasm_table_elem_type_t;

typedef struct {
  pwasm_table_elem_type_t elem_type; /* must be 0x70 */
  pwasm_limits_t limits;
} pwasm_table_t;

#define PWASM_VALUE_TYPE_DEFS \
  PWASM_VALUE_TYPE(0x7F, I32, "i32") \
  PWASM_VALUE_TYPE(0x7D, I64, "i64") \
  PWASM_VALUE_TYPE(0x7E, F32, "f32") \
  PWASM_VALUE_TYPE(0x7C, F64, "f64") \
  PWASM_VALUE_TYPE(0x00, LAST, "unknown type")

typedef enum {
#define PWASM_VALUE_TYPE(a, b, c) PWASM_VALUE_TYPE_ ## b = (a),
PWASM_VALUE_TYPE_DEFS
#undef PWASM_VALUE_TYPE
} pwasm_value_type_t;

// typedef uint32_t pwasm_value_type_t;

const char *pwasm_value_type_get_name(const pwasm_value_type_t);

typedef uint32_t pwasm_result_type_t;

const char *pwasm_result_type_get_name(const pwasm_result_type_t);

#define PWASM_IMM_DEFS \
  PWASM_IMM(NONE, "none") \
  PWASM_IMM(BLOCK, "block") \
  PWASM_IMM(BR_TABLE, "br_table") \
  PWASM_IMM(INDEX, "index") \
  PWASM_IMM(CALL_INDIRECT, "call_indirect") \
  PWASM_IMM(MEM, "mem") \
  PWASM_IMM(I32_CONST, "i32_const") \
  PWASM_IMM(I64_CONST, "i64_const") \
  PWASM_IMM(F32_CONST, "f32_const") \
  PWASM_IMM(F64_CONST, "f64_const") \
  PWASM_IMM(LAST, "invalid")

typedef enum {
#define PWASM_IMM(a, b) PWASM_IMM_##a,
PWASM_IMM_DEFS
#undef PWASM_IMM
} pwasm_imm_t;

const char *pwasm_imm_get_name(const pwasm_imm_t);

#define PWASM_OP_DEFS \
  /* 0x00 */ PWASM_OP(UNREACHABLE, "unreachable", NONE) \
  /* 0x01 */ PWASM_OP(NOP, "nop", NONE) \
  /* 0x02 */ PWASM_OP_CONTROL(BLOCK, "block", BLOCK) \
  /* 0x03 */ PWASM_OP_CONTROL(LOOP, "loop", BLOCK) \
  /* 0x04 */ PWASM_OP_CONTROL(IF, "if", BLOCK) \
  /* 0x05 */ PWASM_OP(ELSE, "else", NONE) \
  /* 0x06 */ PWASM_OP_RESERVED(_06, "06") \
  /* 0x07 */ PWASM_OP_RESERVED(_07, "07") \
  /* 0x08 */ PWASM_OP_RESERVED(_08, "08") \
  /* 0x09 */ PWASM_OP_RESERVED(_09, "09") \
  /* 0x0A */ PWASM_OP_RESERVED(_0A, "0A") \
  /* 0x0B */ PWASM_OP_CONST(END, "end", NONE) \
  /* 0x0C */ PWASM_OP(BR, "br", INDEX) \
  /* 0x0D */ PWASM_OP(BR_IF, "br_if", INDEX) \
  /* 0x0E */ PWASM_OP(BR_TABLE, "br_table", BR_TABLE) \
  /* 0x0F */ PWASM_OP(RETURN, "return", NONE) \
  /* 0x10 */ PWASM_OP(CALL, "call", INDEX) \
  /* 0x11 */ PWASM_OP(CALL_INDIRECT, "call_indirect", CALL_INDIRECT) \
  /* 0x12 */ PWASM_OP_RESERVED(_12, "12") \
  /* 0x13 */ PWASM_OP_RESERVED(_13, "13") \
  /* 0x14 */ PWASM_OP_RESERVED(_14, "14") \
  /* 0x15 */ PWASM_OP_RESERVED(_15, "15") \
  /* 0x16 */ PWASM_OP_RESERVED(_16, "16") \
  /* 0x17 */ PWASM_OP_RESERVED(_17, "17") \
  /* 0x18 */ PWASM_OP_RESERVED(_18, "18") \
  /* 0x19 */ PWASM_OP_RESERVED(_19, "19") \
  /* 0x1A */ PWASM_OP(DROP, "drop", NONE) \
  /* 0x1B */ PWASM_OP(SELECT, "select", NONE) \
  /* 0x1C */ PWASM_OP_RESERVED(_1C, "1c") \
  /* 0x1D */ PWASM_OP_RESERVED(_1D, "1d") \
  /* 0x1E */ PWASM_OP_RESERVED(_1E, "1e") \
  /* 0x1F */ PWASM_OP_RESERVED(_1F, "1f") \
  /* 0x20 */ PWASM_OP(LOCAL_GET, "local.get", INDEX) \
  /* 0x21 */ PWASM_OP(LOCAL_SET, "local.set", INDEX) \
  /* 0x22 */ PWASM_OP(LOCAL_TEE, "local.tee", INDEX) \
  /* 0x23 */ PWASM_OP_CONST(GLOBAL_GET, "global.get", INDEX) \
  /* 0x24 */ PWASM_OP(GLOBAL_SET, "global.set", INDEX) \
  /* 0x25 */ PWASM_OP_RESERVED(_25, "25") \
  /* 0x26 */ PWASM_OP_RESERVED(_26, "26") \
  /* 0x27 */ PWASM_OP_RESERVED(_27, "27") \
  /* 0x28 */ PWASM_OP(I32_LOAD, "i32.load", MEM) \
  /* 0x29 */ PWASM_OP(I64_LOAD, "i64.load", MEM) \
  /* 0x2A */ PWASM_OP(F32_LOAD, "f32.load", MEM) \
  /* 0x2B */ PWASM_OP(F64_LOAD, "f64.load", MEM) \
  /* 0x2C */ PWASM_OP(I32_LOAD8_S, "i32.load8_s", MEM) \
  /* 0x2D */ PWASM_OP(I32_LOAD8_U, "i32.load8_u", MEM) \
  /* 0x2E */ PWASM_OP(I32_LOAD16_S, "i32.load16_s", MEM) \
  /* 0x2F */ PWASM_OP(I32_LOAD16_U, "i32.load16_u", MEM) \
  /* 0x30 */ PWASM_OP(I64_LOAD8_S, "i64.load8_s", MEM) \
  /* 0x31 */ PWASM_OP(I64_LOAD8_U, "i64.load8_u", MEM) \
  /* 0x32 */ PWASM_OP(I64_LOAD16_S, "i64.load16_s", MEM) \
  /* 0x33 */ PWASM_OP(I64_LOAD16_U, "i64.load16_u", MEM) \
  /* 0x34 */ PWASM_OP(I64_LOAD32_S, "i64.load32_s", MEM) \
  /* 0x35 */ PWASM_OP(I64_LOAD32_U, "i64.load32_u", MEM) \
  /* 0x36 */ PWASM_OP(I32_STORE, "i32.store", MEM) \
  /* 0x37 */ PWASM_OP(I64_STORE, "i64.store", MEM) \
  /* 0x38 */ PWASM_OP(F32_STORE, "f32.store", MEM) \
  /* 0x39 */ PWASM_OP(F64_STORE, "f64.store", MEM) \
  /* 0x3A */ PWASM_OP(I32_STORE8, "i32.store8", MEM) \
  /* 0x3B */ PWASM_OP(I32_STORE16, "i32.store16", MEM) \
  /* 0x3C */ PWASM_OP(I64_STORE8, "i64.store8", MEM) \
  /* 0x3D */ PWASM_OP(I64_STORE16, "i64.store16", MEM) \
  /* 0x3E */ PWASM_OP(I64_STORE32, "i64.store32", MEM) \
  /* 0x3F */ PWASM_OP(MEMORY_SIZE, "memory.size", NONE) \
  /* 0x40 */ PWASM_OP(MEMORY_GROW, "memory.grow", NONE) \
  /* 0x41 */ PWASM_OP_CONST(I32_CONST, "i32.const", I32_CONST) \
  /* 0x42 */ PWASM_OP_CONST(I64_CONST, "i64.const", I64_CONST) \
  /* 0x43 */ PWASM_OP_CONST(F32_CONST, "f32.const", F32_CONST) \
  /* 0x44 */ PWASM_OP_CONST(F64_CONST, "f64.const", F64_CONST) \
  /* 0x45 */ PWASM_OP(I32_EQZ, "i32.eqz", NONE) \
  /* 0x46 */ PWASM_OP(I32_EQ, "i32.eq", NONE) \
  /* 0x47 */ PWASM_OP(I32_NE, "i32.ne", NONE) \
  /* 0x48 */ PWASM_OP(I32_LT_S, "i32.lt_s", NONE) \
  /* 0x49 */ PWASM_OP(I32_LT_U, "i32.lt_u", NONE) \
  /* 0x4A */ PWASM_OP(I32_GT_S, "i32.gt_s", NONE) \
  /* 0x4B */ PWASM_OP(I32_GT_U, "i32.gt_u", NONE) \
  /* 0x4C */ PWASM_OP(I32_LE_S, "i32.le_s", NONE) \
  /* 0x4D */ PWASM_OP(I32_LE_U, "i32.le_u", NONE) \
  /* 0x4E */ PWASM_OP(I32_GE_S, "i32.ge_s", NONE) \
  /* 0x4F */ PWASM_OP(I32_GE_U, "i32.ge_u", NONE) \
  /* 0x50 */ PWASM_OP(I64_EQZ, "i64.eqz", NONE) \
  /* 0x51 */ PWASM_OP(I64_EQ, "i64.eq", NONE) \
  /* 0x52 */ PWASM_OP(I64_NE, "i64.ne", NONE) \
  /* 0x53 */ PWASM_OP(I64_LT_S, "i64.lt_s", NONE) \
  /* 0x54 */ PWASM_OP(I64_LT_U, "i64.lt_u", NONE) \
  /* 0x55 */ PWASM_OP(I64_GT_S, "i64.gt_s", NONE) \
  /* 0x56 */ PWASM_OP(I64_GT_U, "i64.gt_u", NONE) \
  /* 0x57 */ PWASM_OP(I64_LE_S, "i64.le_s", NONE) \
  /* 0x58 */ PWASM_OP(I64_LE_U, "i64.le_u", NONE) \
  /* 0x59 */ PWASM_OP(I64_GE_S, "i64.ge_s", NONE) \
  /* 0x5A */ PWASM_OP(I64_GE_U, "i64.ge_u", NONE) \
  /* 0x5B */ PWASM_OP(F32_EQ, "f32.eq", NONE) \
  /* 0x5C */ PWASM_OP(F32_NE, "f32.ne", NONE) \
  /* 0x5D */ PWASM_OP(F32_LT, "f32.lt", NONE) \
  /* 0x5E */ PWASM_OP(F32_GT, "f32.gt", NONE) \
  /* 0x5F */ PWASM_OP(F32_LE, "f32.le", NONE) \
  /* 0x60 */ PWASM_OP(F32_GE, "f32.ge", NONE) \
  /* 0x61 */ PWASM_OP(F64_EQ, "f64.eq", NONE) \
  /* 0x62 */ PWASM_OP(F64_NE, "f64.ne", NONE) \
  /* 0x63 */ PWASM_OP(F64_LT, "f64.lt", NONE) \
  /* 0x64 */ PWASM_OP(F64_GT, "f64.gt", NONE) \
  /* 0x65 */ PWASM_OP(F64_LE, "f64.le", NONE) \
  /* 0x66 */ PWASM_OP(F64_GE, "f64.ge", NONE) \
  /* 0x67 */ PWASM_OP(I32_CLZ, "i32.clz", NONE) \
  /* 0x68 */ PWASM_OP(I32_CTZ, "i32.ctz", NONE) \
  /* 0x69 */ PWASM_OP(I32_POPCNT, "i32.popcnt", NONE) \
  /* 0x6A */ PWASM_OP(I32_ADD, "i32.add", NONE) \
  /* 0x6B */ PWASM_OP(I32_SUB, "i32.sub", NONE) \
  /* 0x6C */ PWASM_OP(I32_MUL, "i32.mul", NONE) \
  /* 0x6D */ PWASM_OP(I32_DIV_S, "i32.div_s", NONE) \
  /* 0x6E */ PWASM_OP(I32_DIV_U, "i32.div_u", NONE) \
  /* 0x6F */ PWASM_OP(I32_REM_S, "i32.rem_s", NONE) \
  /* 0x70 */ PWASM_OP(I32_REM_U, "i32.rem_u", NONE) \
  /* 0x71 */ PWASM_OP(I32_AND, "i32.and", NONE) \
  /* 0x72 */ PWASM_OP(I32_OR, "i32.or", NONE) \
  /* 0x73 */ PWASM_OP(I32_XOR, "i32.xor", NONE) \
  /* 0x74 */ PWASM_OP(I32_SHL, "i32.shl", NONE) \
  /* 0x75 */ PWASM_OP(I32_SHR_S, "i32.shr_s", NONE) \
  /* 0x76 */ PWASM_OP(I32_SHR_U, "i32.shr_u", NONE) \
  /* 0x77 */ PWASM_OP(I32_ROTL, "i32.rotl", NONE) \
  /* 0x78 */ PWASM_OP(I32_ROTR, "i32.rotr", NONE) \
  /* 0x79 */ PWASM_OP(I64_CLZ, "i64.clz", NONE) \
  /* 0x7A */ PWASM_OP(I64_CTZ, "i64.ctz", NONE) \
  /* 0x7B */ PWASM_OP(I64_POPCNT, "i64.popcnt", NONE) \
  /* 0x7C */ PWASM_OP(I64_ADD, "i64.add", NONE) \
  /* 0x7D */ PWASM_OP(I64_SUB, "i64.sub", NONE) \
  /* 0x7E */ PWASM_OP(I64_MUL, "i64.mul", NONE) \
  /* 0x7F */ PWASM_OP(I64_DIV_S, "i64.div_s", NONE) \
  /* 0x80 */ PWASM_OP(I64_DIV_U, "i64.div_u", NONE) \
  /* 0x81 */ PWASM_OP(I64_REM_S, "i64.rem_s", NONE) \
  /* 0x82 */ PWASM_OP(I64_REM_U, "i64.rem_u", NONE) \
  /* 0x83 */ PWASM_OP(I64_AND, "i64.and", NONE) \
  /* 0x84 */ PWASM_OP(I64_OR, "i64.or", NONE) \
  /* 0x85 */ PWASM_OP(I64_XOR, "i64.xor", NONE) \
  /* 0x86 */ PWASM_OP(I64_SHL, "i64.shl", NONE) \
  /* 0x87 */ PWASM_OP(I64_SHR_S, "i64.shr_s", NONE) \
  /* 0x88 */ PWASM_OP(I64_SHR_U, "i64.shr_u", NONE) \
  /* 0x89 */ PWASM_OP(I64_ROTL, "i64.rotl", NONE) \
  /* 0x8A */ PWASM_OP(I64_ROTR, "i64.rotr", NONE) \
  /* 0x8B */ PWASM_OP(F32_ABS, "f32.abs", NONE) \
  /* 0x8C */ PWASM_OP(F32_NEG, "f32.neg", NONE) \
  /* 0x8D */ PWASM_OP(F32_CEIL, "f32.ceil", NONE) \
  /* 0x8E */ PWASM_OP(F32_FLOOR, "f32.floor", NONE) \
  /* 0x8F */ PWASM_OP(F32_TRUNC, "f32.trunc", NONE) \
  /* 0x90 */ PWASM_OP(F32_NEAREST, "f32.nearest", NONE) \
  /* 0x91 */ PWASM_OP(F32_SQRT, "f32.sqrt", NONE) \
  /* 0x92 */ PWASM_OP(F32_ADD, "f32.add", NONE) \
  /* 0x93 */ PWASM_OP(F32_SUB, "f32.sub", NONE) \
  /* 0x94 */ PWASM_OP(F32_MUL, "f32.mul", NONE) \
  /* 0x95 */ PWASM_OP(F32_DIV, "f32.div", NONE) \
  /* 0x96 */ PWASM_OP(F32_MIN, "f32.min", NONE) \
  /* 0x97 */ PWASM_OP(F32_MAX, "f32.max", NONE) \
  /* 0x98 */ PWASM_OP(F32_COPYSIGN, "f32.copysign", NONE) \
  /* 0x99 */ PWASM_OP(F64_ABS, "f64.abs", NONE) \
  /* 0x9A */ PWASM_OP(F64_NEG, "f64.neg", NONE) \
  /* 0x9B */ PWASM_OP(F64_CEIL, "f64.ceil", NONE) \
  /* 0x9C */ PWASM_OP(F64_FLOOR, "f64.floor", NONE) \
  /* 0x9D */ PWASM_OP(F64_TRUNC, "f64.trunc", NONE) \
  /* 0x9E */ PWASM_OP(F64_NEAREST, "f64.nearest", NONE) \
  /* 0x9F */ PWASM_OP(F64_SQRT, "f64.sqrt", NONE) \
  /* 0xA0 */ PWASM_OP(F64_ADD, "f64.add", NONE) \
  /* 0xA1 */ PWASM_OP(F64_SUB, "f64.sub", NONE) \
  /* 0xA2 */ PWASM_OP(F64_MUL, "f64.mul", NONE) \
  /* 0xA3 */ PWASM_OP(F64_DIV, "f64.div", NONE) \
  /* 0xA4 */ PWASM_OP(F64_MIN, "f64.min", NONE) \
  /* 0xA5 */ PWASM_OP(F64_MAX, "f64.max", NONE) \
  /* 0xA6 */ PWASM_OP(F64_COPYSIGN, "f64.copysign", NONE) \
  /* 0xA7 */ PWASM_OP(I32_WRAP_I64, "i32.wrap_i64", NONE) \
  /* 0xA8 */ PWASM_OP(I32_TRUNC_F32_S, "i32.trunc_f32_s", NONE) \
  /* 0xA9 */ PWASM_OP(I32_TRUNC_F32_U, "i32.trunc_f32_u", NONE) \
  /* 0xAA */ PWASM_OP(I32_TRUNC_F64_S, "i32.trunc_f64_s", NONE) \
  /* 0xAB */ PWASM_OP(I32_TRUNC_F64_U, "i32.trunc_f64_u", NONE) \
  /* 0xAC */ PWASM_OP(I64_EXTEND_I32_S, "i64.extend_i32_s", NONE) \
  /* 0xAD */ PWASM_OP(I64_EXTEND_I32_U, "i64.extend_i32_u", NONE) \
  /* 0xAE */ PWASM_OP(I64_TRUNC_F32_S, "i64.trunc_f32_s", NONE) \
  /* 0xAF */ PWASM_OP(I64_TRUNC_F32_U, "i64.trunc_f32_u", NONE) \
  /* 0xB0 */ PWASM_OP(I64_TRUNC_F64_S, "i64.trunc_f64_s", NONE) \
  /* 0xB1 */ PWASM_OP(I64_TRUNC_F64_U, "i64.trunc_f64_u", NONE) \
  /* 0xB2 */ PWASM_OP(F32_CONVERT_I32_S, "f32.convert_i32_s", NONE) \
  /* 0xB3 */ PWASM_OP(F32_CONVERT_I32_U, "f32.convert_i32_u", NONE) \
  /* 0xB4 */ PWASM_OP(F32_CONVERT_I64_S, "f32.convert_i64_s", NONE) \
  /* 0xB5 */ PWASM_OP(F32_CONVERT_I64_U, "f32.convert_i64_u", NONE) \
  /* 0xB6 */ PWASM_OP(F32_DEMOTE_F64, "f32.demote_f64", NONE) \
  /* 0xB7 */ PWASM_OP(F64_CONVERT_I32_S, "f64.convert_i32_s", NONE) \
  /* 0xB8 */ PWASM_OP(F64_CONVERT_I32_U, "f64.convert_i32_u", NONE) \
  /* 0xB9 */ PWASM_OP(F64_CONVERT_I64_S, "f64.convert_i64_s", NONE) \
  /* 0xBA */ PWASM_OP(F64_CONVERT_I64_U, "f64.convert_i64_u", NONE) \
  /* 0xBB */ PWASM_OP(F64_PROMOTE_F32, "f64.promote_f32", NONE) \
  /* 0xBC */ PWASM_OP(I32_REINTERPRET_F32, "i32.reinterpret_f32", NONE) \
  /* 0xBD */ PWASM_OP(I64_REINTERPRET_F64, "i64.reinterpret_f64", NONE) \
  /* 0xBE */ PWASM_OP(F32_REINTERPRET_I32, "f32.reinterpret_i32", NONE) \
  /* 0xBF */ PWASM_OP(F64_REINTERPRET_I64, "f64.reinterpret_i64", NONE) \
  /* 0xC0 */ PWASM_OP_RESERVED(_C0, "c0") \
  /* 0xC1 */ PWASM_OP_RESERVED(_C1, "c1") \
  /* 0xC2 */ PWASM_OP_RESERVED(_C2, "c2") \
  /* 0xC3 */ PWASM_OP_RESERVED(_C3, "c3") \
  /* 0xC4 */ PWASM_OP_RESERVED(_C4, "c4") \
  /* 0xC5 */ PWASM_OP_RESERVED(_C5, "c5") \
  /* 0xC6 */ PWASM_OP_RESERVED(_C6, "c6") \
  /* 0xC7 */ PWASM_OP_RESERVED(_C7, "c7") \
  /* 0xC8 */ PWASM_OP_RESERVED(_C8, "c8") \
  /* 0xC9 */ PWASM_OP_RESERVED(_C9, "c9") \
  /* 0xCA */ PWASM_OP_RESERVED(_CA, "ca") \
  /* 0xCB */ PWASM_OP_RESERVED(_CB, "cb") \
  /* 0xCC */ PWASM_OP_RESERVED(_CC, "cc") \
  /* 0xCD */ PWASM_OP_RESERVED(_CD, "cd") \
  /* 0xCE */ PWASM_OP_RESERVED(_CE, "ce") \
  /* 0xCF */ PWASM_OP_RESERVED(_CF, "cf") \
  /* 0xD0 */ PWASM_OP_RESERVED(_D0, "d0") \
  /* 0xD1 */ PWASM_OP_RESERVED(_D1, "d1") \
  /* 0xD2 */ PWASM_OP_RESERVED(_D2, "d2") \
  /* 0xD3 */ PWASM_OP_RESERVED(_D3, "d3") \
  /* 0xD4 */ PWASM_OP_RESERVED(_D4, "d4") \
  /* 0xD5 */ PWASM_OP_RESERVED(_D5, "d5") \
  /* 0xD6 */ PWASM_OP_RESERVED(_D6, "d6") \
  /* 0xD7 */ PWASM_OP_RESERVED(_D7, "d7") \
  /* 0xD8 */ PWASM_OP_RESERVED(_D8, "d8") \
  /* 0xD9 */ PWASM_OP_RESERVED(_D9, "d9") \
  /* 0xDA */ PWASM_OP_RESERVED(_DA, "da") \
  /* 0xDB */ PWASM_OP_RESERVED(_DB, "db") \
  /* 0xDC */ PWASM_OP_RESERVED(_DC, "dc") \
  /* 0xDD */ PWASM_OP_RESERVED(_DD, "dd") \
  /* 0xDE */ PWASM_OP_RESERVED(_DE, "de") \
  /* 0xDF */ PWASM_OP_RESERVED(_DF, "df") \
  /* 0xE0 */ PWASM_OP_RESERVED(_E0, "e0") \
  /* 0xE1 */ PWASM_OP_RESERVED(_E1, "e1") \
  /* 0xE2 */ PWASM_OP_RESERVED(_E2, "e2") \
  /* 0xE3 */ PWASM_OP_RESERVED(_E3, "e3") \
  /* 0xE4 */ PWASM_OP_RESERVED(_E4, "e4") \
  /* 0xE5 */ PWASM_OP_RESERVED(_E5, "e5") \
  /* 0xE6 */ PWASM_OP_RESERVED(_E6, "e6") \
  /* 0xE7 */ PWASM_OP_RESERVED(_E7, "e7") \
  /* 0xE8 */ PWASM_OP_RESERVED(_E8, "e8") \
  /* 0xE9 */ PWASM_OP_RESERVED(_E9, "e9") \
  /* 0xEA */ PWASM_OP_RESERVED(_EA, "ea") \
  /* 0xEB */ PWASM_OP_RESERVED(_EB, "eb") \
  /* 0xEC */ PWASM_OP_RESERVED(_EC, "ec") \
  /* 0xED */ PWASM_OP_RESERVED(_ED, "ed") \
  /* 0xEE */ PWASM_OP_RESERVED(_EE, "ee") \
  /* 0xEF */ PWASM_OP_RESERVED(_EF, "ef") \
  /* 0xF0 */ PWASM_OP_RESERVED(_F0, "f0") \
  /* 0xF1 */ PWASM_OP_RESERVED(_F1, "f1") \
  /* 0xF2 */ PWASM_OP_RESERVED(_F2, "f2") \
  /* 0xF3 */ PWASM_OP_RESERVED(_F3, "f3") \
  /* 0xF4 */ PWASM_OP_RESERVED(_F4, "f4") \
  /* 0xF5 */ PWASM_OP_RESERVED(_F5, "f5") \
  /* 0xF6 */ PWASM_OP_RESERVED(_F6, "f6") \
  /* 0xF7 */ PWASM_OP_RESERVED(_F7, "f7") \
  /* 0xF8 */ PWASM_OP_RESERVED(_F8, "f8") \
  /* 0xF9 */ PWASM_OP_RESERVED(_F9, "f9") \
  /* 0xFA */ PWASM_OP_RESERVED(_FA, "fa") \
  /* 0xFB */ PWASM_OP_RESERVED(_FB, "fb") \
  /* 0xFC */ PWASM_OP_RESERVED(_FC, "fc") \
  /* 0xFD */ PWASM_OP_RESERVED(_FD, "fd") \
  /* 0xFE */ PWASM_OP_RESERVED(_FE, "fe") \
  /* 0xFF */ PWASM_OP_RESERVED(_FF, "ff")

#define PWASM_OP(a, b, c) PWASM_OP_ ## a,
#define PWASM_OP_CONST(a, b, c) PWASM_OP_ ## a,
#define PWASM_OP_CONTROL(a, b, c) PWASM_OP_ ## a,
#define PWASM_OP_RESERVED(a, b) PWASM_OP_RESERVED ## a,
typedef enum {
PWASM_OP_DEFS
} pwasm_op_t;
#undef PWASM_OP
#undef PWASM_OP_CONST
#undef PWASM_OP_CONTROL
#undef PWASM_OP_RESERVED

const char *pwasm_op_get_name(const pwasm_op_t);

typedef struct {
  pwasm_buf_t buf;
} pwasm_expr_t;

typedef struct {
  pwasm_op_t op;

  union {
    /* block, loop */
    struct {
      pwasm_result_type_t type;
    } v_block;

    /* br_table */
    struct {
      union {
        pwasm_buf_t buf;
        pwasm_slice_t slice;
      } labels;
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
} pwasm_inst_t;

typedef struct {
  pwasm_value_type_t type;
  _Bool mutable;
} pwasm_global_type_t;

// FIXME: s/function/func/, s/memory/mem/
#define PWASM_IMPORT_TYPES \
  PWASM_IMPORT_TYPE(FUNC, "function", function) \
  PWASM_IMPORT_TYPE(TABLE, "table", table) \
  PWASM_IMPORT_TYPE(MEM, "memory", memory) \
  PWASM_IMPORT_TYPE(GLOBAL, "global", global) \
  PWASM_IMPORT_TYPE(LAST, "unknown import desc", invalid)

#define PWASM_IMPORT_TYPE(a, b, c) PWASM_IMPORT_TYPE_##a,
typedef enum {
PWASM_IMPORT_TYPES
} pwasm_import_type_t;
#undef PWASM_IMPORT_TYPE

const char *pwasm_import_type_get_name(const pwasm_import_type_t);

#define PWASM_EXPORT_TYPES \
  PWASM_EXPORT_TYPE(FUNC, "function") \
  PWASM_EXPORT_TYPE(TABLE, "table") \
  PWASM_EXPORT_TYPE(MEM, "memory") \
  PWASM_EXPORT_TYPE(GLOBAL, "global") \
  PWASM_EXPORT_TYPE(LAST, "unknown export type")

#define PWASM_EXPORT_TYPE(a, b) PWASM_EXPORT_TYPE_##a,
typedef enum {
PWASM_EXPORT_TYPES
} pwasm_export_type_t;
#undef PWASM_EXPORT_TYPE

const char *pwasm_export_type_get_name(const pwasm_export_type_t);

typedef struct {
  uint32_t num;
  pwasm_value_type_t type;
} pwasm_local_t;

typedef struct {
  void *(*on_realloc)(void *, size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_mem_cbs_t;

typedef struct {
  const pwasm_mem_cbs_t *cbs;
  void *cb_data;
} pwasm_mem_ctx_t;

pwasm_mem_ctx_t pwasm_mem_ctx_init_defaults(void *);

void *pwasm_realloc(pwasm_mem_ctx_t *, void *, const size_t);

typedef struct {
  pwasm_mem_ctx_t * const mem_ctx;
  uint8_t *rows;
  size_t stride,
         num_rows,
         max_rows;
} pwasm_vec_t;

_Bool pwasm_vec_init(
  pwasm_mem_ctx_t * const,
  pwasm_vec_t *,
  const size_t
);

_Bool pwasm_vec_fini(pwasm_vec_t *);

size_t pwasm_vec_get_size(const pwasm_vec_t *);
const void *pwasm_vec_get_data(const pwasm_vec_t *);

typedef struct {
  pwasm_section_type_t type;
  uint32_t len;
} pwasm_header_t;

typedef struct {
  pwasm_slice_t name;
  pwasm_slice_t data;
} pwasm_custom_section_t;

typedef struct {
  pwasm_slice_t params;
  pwasm_slice_t results;
} pwasm_type_t;

typedef struct {
  pwasm_slice_t module;
  pwasm_slice_t name;
  pwasm_import_type_t type;

  union {
    /* type index */
    uint32_t func;

    pwasm_table_t table;

    pwasm_limits_t mem;

    pwasm_global_type_t global;
  };
} pwasm_import_t;

typedef struct {
  pwasm_global_type_t type;

  // slice of insts with offset init instructions
  pwasm_slice_t expr;
} pwasm_global_t;

typedef struct {
  // offset of function prototype in function_types
  size_t type_id;

  // local variable slots (only used for module functions)
  //
  // NOTE: each local has a num parameter so you cannot use .len to
  // calculate the total number of locals; use num_locals instead.
  pwasm_slice_t locals;

  // total number of local slots
  size_t num_locals;

  // instructions
  pwasm_slice_t expr;
} pwasm_func_t;

typedef struct {
  pwasm_slice_t name;
  pwasm_export_type_t type;
  uint32_t id;
} pwasm_export_t;

typedef struct {
  uint32_t table_id;

  // slice of insts with expr instructions
  pwasm_slice_t expr;

  // slice of u32s with function IDs
  pwasm_slice_t funcs;
} pwasm_elem_t;

typedef struct {
  uint32_t mem_id;

  // slice of insts with expr instructions
  pwasm_slice_t expr;

  // slice of bytes with data
  pwasm_slice_t data;
} pwasm_segment_t;

typedef struct {
  pwasm_slice_t (*on_u32s)(const uint32_t *, const size_t, void *);
  pwasm_slice_t (*on_bytes)(const uint8_t *, const size_t, void *);
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);

  void (*on_section)(const pwasm_header_t *, void *);
  void (*on_custom_section)(const pwasm_custom_section_t *, void *);
  void (*on_types)(const pwasm_type_t *, const size_t, void *);
  void (*on_imports)(const pwasm_import_t *, const size_t, void *);
  void (*on_funcs)(const uint32_t *, const size_t, void *);

  void (*on_tables)(const pwasm_table_t *, const size_t, void *);
  void (*on_mems)(const pwasm_limits_t *, const size_t, void *);
  void (*on_globals)(const pwasm_global_t *, const size_t, void *);
  void (*on_exports)(const pwasm_export_t *, const size_t, void *);
  void (*on_elems)(const pwasm_elem_t *, const size_t, void *);
  void (*on_start)(const uint32_t, void *);

  pwasm_slice_t (*on_locals)(const pwasm_local_t *, const size_t, void *);
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);

  void (*on_codes)(const pwasm_func_t *, const size_t, void *);
  void (*on_segments)(const pwasm_segment_t *, const size_t, void *);

  void (*on_error)(const char *, void *);
} pwasm_mod_parse_cbs_t;

size_t pwasm_mod_parse(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t *,
  void *cb_data
);

typedef struct {
  pwasm_mem_ctx_t * const mem_ctx;

  // single block of contiguous memory that serves as the backing store
  // for all of the items below
  pwasm_buf_t mem;

  // internal array of u32s, referenced by slices from several other
  // sections below
  const uint32_t * const u32s;
  const size_t num_u32s;

  const uint32_t * const sections;
  const size_t num_sections;

  const pwasm_custom_section_t * const custom_sections;
  const size_t num_custom_sections;

  const pwasm_type_t * const types;
  const size_t num_types;

  const pwasm_import_t * const imports;
  const size_t num_imports;

  size_t num_import_types[PWASM_IMPORT_TYPE_LAST];

  const pwasm_inst_t * const insts;
  const size_t num_insts;

  const pwasm_global_t * const globals;
  const size_t num_globals;

  const uint32_t * const funcs;
  const size_t num_funcs;

  const pwasm_table_t * const tables;
  const size_t num_tables;

  const pwasm_limits_t * const mems;
  const size_t num_mems;

  const pwasm_export_t * const exports;
  const size_t num_exports;

  const pwasm_local_t * const locals;
  const size_t num_locals;

  const pwasm_func_t * const codes;
  const size_t num_codes;

  const pwasm_elem_t * const elems;
  const size_t num_elems;

  const pwasm_segment_t * const segments;
  const size_t num_segments;

  const uint8_t * const bytes;
  const size_t num_bytes;
} pwasm_mod_t;

/**
 * Parse a module from source +src+ into the module +mod+.
 *
 * Returns 0 on error.
 */
size_t pwasm_mod_init(
  pwasm_mem_ctx_t * const mem_ctx,
  pwasm_mod_t * const mod,
  pwasm_buf_t src
);

/**
 * Free memory associated with module.
 */
void pwasm_mod_fini(pwasm_mod_t *);

typedef struct {
  pwasm_mem_ctx_t * const mem_ctx;

  size_t num_import_types[PWASM_IMPORT_TYPE_LAST];
  size_t num_export_types[PWASM_EXPORT_TYPE_LAST];

  // custom section name/data, import mod/name, export name, segment
  // and data
  pwasm_vec_t bytes;

  // contains type params/results, local num/type pairs, br_table
  // labels, function IDs, and element function IDs
  pwasm_vec_t u32s;

  // section IDs
  pwasm_vec_t sections;

  pwasm_vec_t custom_sections;
  pwasm_vec_t types;
  pwasm_vec_t imports;

  // contains function code, element offset exprs, and global init
  // exprs, and segment offset init exprs
  pwasm_vec_t insts;

  pwasm_vec_t tables;
  pwasm_vec_t mems;
  pwasm_vec_t funcs;

  pwasm_vec_t globals;
  pwasm_vec_t exports;
  pwasm_vec_t locals;
  pwasm_vec_t codes;
  pwasm_vec_t elems;
  pwasm_vec_t segments;

  _Bool has_start;
  uint32_t start;
} pwasm_builder_t;

_Bool pwasm_builder_init(
  pwasm_mem_ctx_t *,
  pwasm_builder_t *
);

void pwasm_builder_fini(pwasm_builder_t *);

_Bool pwasm_build(
  const pwasm_builder_t *,
  pwasm_mod_t *
);

typedef union {
  uint32_t i32;
  uint64_t i64;
  float    f32;
  double   f64;
} pwasm_val_t;

typedef struct {
  pwasm_val_t * const ptr;
  const size_t len;
  size_t pos;
} pwasm_stack_t;

typedef struct pwasm_env_t pwasm_env_t;

typedef _Bool (*pwasm_native_func_cb_t)(
  pwasm_env_t *,
  pwasm_stack_t *
);

typedef struct {
  struct {
    const pwasm_value_type_t *ptr;
    const size_t len;
  } params;

  struct {
    const pwasm_value_type_t *ptr;
    const size_t len;
  } results;
} pwasm_native_type_t;

typedef struct {
  const char * const name;
  const pwasm_native_func_cb_t func;
  const pwasm_native_type_t type;
} pwasm_native_func_t;

typedef struct {
  const size_t num_funcs;
  const pwasm_native_func_t * const funcs;

  size_t num_mems;
  pwasm_buf_t * const mems;

  size_t num_globals;
  const pwasm_val_t * const globals;

  // TODO: tables
} pwasm_native_t;

typedef struct {
  // init env (alloc memory)
  _Bool (*init)(pwasm_env_t *);

  // finalize env (free memory)
  void (*fini)(pwasm_env_t *);

  // add native module with given name
  uint32_t (*add_native)(pwasm_env_t *, const char *, const pwasm_native_t *);

  // add module
  uint32_t (*add_mod)(pwasm_env_t *, const char *, const pwasm_mod_t *);

  uint32_t (*find_global)(pwasm_env_t *, const char *, const char *);
  pwasm_val_t (*get_global)(pwasm_env_t *, const uint32_t);
  uint32_t (*set_global)(pwasm_env_t *, const uint32_t, const pwasm_val_t);

  // uint32_t (*find_table)(const char *, const char *);
  // pwasm_val_t (*get_table)(const uint32_t);

  // uint32_t (*find_func)(pwasm_env_t *, const char *, const char *);
  // _Bool (*call_func)(pwasm_env_t *, uint32_t);
  _Bool (*call)(pwasm_env_t *, const char *, const char *);

  pwasm_buf_t (*find_mem)(pwasm_env_t *, const char *, const char *);
} pwasm_env_cbs_t;

struct pwasm_env_t {
  pwasm_mem_ctx_t *mem_ctx;
  const pwasm_env_cbs_t *cbs;
  pwasm_stack_t *stack;
  void *env_data;
  void *user_data;
};

const pwasm_env_cbs_t *pwasm_interpreter_get_cbs(void);

_Bool pwasm_env_init(
  pwasm_env_t *,
  pwasm_mem_ctx_t *,
  const pwasm_env_cbs_t *,
  pwasm_stack_t * stack,
  void *user_data
);

void pwasm_env_fini(pwasm_env_t *);

uint32_t pwasm_env_add_mod(
  pwasm_env_t *,
  const char * const,
  const pwasm_mod_t *
);

uint32_t pwasm_env_add_native(
  pwasm_env_t *,
  const char * const,
  const pwasm_native_t *
);

_Bool pwasm_env_call(
  pwasm_env_t *,
  const char * const,
  const char * const
);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PWASM_H */
