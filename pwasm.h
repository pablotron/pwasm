#ifndef PWASM_H
#define PWASM_H

/**
 * @file
 */

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <stddef.h> // size_t
#include <stdint.h> // uint8_t, uint32_t, etc

/**
 * @defgroup util Utilities
 */

/**
 * Buffer structure a memory pointer and a length, in bytes.
 *
 * @ingroup util
 */
typedef struct {
  const uint8_t *ptr; //< Pointer to backing memory for this buffer.
  size_t len;         //< Size of this buffer, in bytes.
} pwasm_buf_t;

/**
 * Structure used to represent a subset of a larger memory buffer or
 * group of values.
 *
 * @ingroup util
 */
typedef struct {
  size_t ofs; //< Offset of first element.
  size_t len; //< Total length of slice.
} pwasm_slice_t;

/**
 * @defgroup type Types
 */

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

/**
 * Section types
 * @ingroup type
 */
typedef enum {
#define PWASM_SECTION_TYPE(a, b) PWASM_SECTION_TYPE_##a,
PWASM_SECTION_TYPES
#undef PWASM_SECTION_TYPE
} pwasm_section_type_t;

/**
 * Get name of section type.
 *
 * @ingroup type
 *
 * @return Pointer to the null-terminated name of a section, or a
 * pointer to the string "unknown section" if the given section type is
 * unknown.
 *
 * @note The strings returned by this function should not be freed.
 */
const char *pwasm_section_type_get_name(const pwasm_section_type_t);

/**
 * Representation of WebAssembly limits.
 * @ingroup mod
 */
typedef struct {
  uint32_t min;   //< Lower bound.
  uint32_t max;   //< Upper bound.
  _Bool has_max;  //< Does this structure have an upper bound?
} pwasm_limits_t;

/**
 * Table element type.
 * @ingroup mod
 */
typedef uint32_t pwasm_elem_type_t;

/**
 * Parsed Web Assembly table.
 * @ingroup mod
 */
typedef struct {
  pwasm_elem_type_t elem_type;  //< Table type.  Must be `0x70`.
  pwasm_limits_t limits;        //< Limits for this table.
} pwasm_table_t;

#define PWASM_VALUE_TYPE_DEFS \
  PWASM_VALUE_TYPE(0x7F, I32, "i32") \
  PWASM_VALUE_TYPE(0x7E, I64, "i64") \
  PWASM_VALUE_TYPE(0x7D, F32, "f32") \
  PWASM_VALUE_TYPE(0x7C, F64, "f64") \
  PWASM_VALUE_TYPE(0x00, LAST, "unknown value type")

/**
 * Value type
 * @ingroup type
 */
typedef enum {
#define PWASM_VALUE_TYPE(a, b, c) PWASM_VALUE_TYPE_ ## b = (a),
PWASM_VALUE_TYPE_DEFS
#undef PWASM_VALUE_TYPE
} pwasm_value_type_t;

/**
 * Get name of value type.
 *
 * @ingroup type
 *
 * @param type Value type
 *
 * @return Pointer to the `NULL~-terminated value type name, or a
 * pointer to the string "unknown value type" if given an invalid value
 * type.
 *
 * @note The strings returned by this function should not be freed.
 * @note This function never returns a `NULL` pointer.
 */
const char *pwasm_value_type_get_name(const pwasm_value_type_t);

#define PWASM_RESULT_TYPE_DEFS \
  PWASM_RESULT_TYPE(0x7F, I32, "i32") \
  PWASM_RESULT_TYPE(0x7D, I64, "i64") \
  PWASM_RESULT_TYPE(0x7E, F32, "f32") \
  PWASM_RESULT_TYPE(0x7C, F64, "f64") \
  PWASM_RESULT_TYPE(0x40, VOID, "void") \
  PWASM_RESULT_TYPE(0x00, LAST, "unknown result type")

/**
 * Block result types
 * @ingroup type
 */
typedef enum {
#define PWASM_RESULT_TYPE(a, b, c) PWASM_RESULT_TYPE_ ## b = (a),
PWASM_RESULT_TYPE_DEFS
#undef PWASM_RESULT_TYPE
} pwasm_result_type_t;

/**
 * Get name of result type.
 *
 * @ingroup type
 *
 * @param type Block result type
 *
 * @return Pointer to the `NULL`-terminated result type name, or a
 * pointer to the string "unknown result type" if given an invalid
 * result type.
 *
 * @note The strings returned by this function should not be freed.
 * @note This function never returns a `NULL` pointer.
 */
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

/**
 * Immediate type.
 * @ingroup type
 */
typedef enum {
#define PWASM_IMM(a, b) PWASM_IMM_##a,
PWASM_IMM_DEFS
#undef PWASM_IMM
} pwasm_imm_t;

/**
 * Get name of an immediate type.
 *
 * @ingroup type
 *
 * @param type Immediate type
 *
 * @return Pointer to the `NULL`-terminated immediate type name, or a
 * pointer to the string "unknown result type" if given an invalid
 * immediate type.
 *
 * @note The strings returned by this function should not be freed.
 * @note This function never returns a `NULL` pointer.
 */
const char *pwasm_imm_get_name(const pwasm_imm_t);

#define PWASM_OP_DEFS \
  /* 0x00 */ PWASM_OP(UNREACHABLE, "unreachable", NONE) \
  /* 0x01 */ PWASM_OP(NOP, "nop", NONE) \
  /* 0x02 */ PWASM_OP(BLOCK, "block", BLOCK) \
  /* 0x03 */ PWASM_OP(LOOP, "loop", BLOCK) \
  /* 0x04 */ PWASM_OP(IF, "if", BLOCK) \
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

/**
 * Opcode
 * @ingroup type
 */
typedef enum {
#define PWASM_OP(a, b, c) PWASM_OP_ ## a,
#define PWASM_OP_CONST(a, b, c) PWASM_OP_ ## a,
#define PWASM_OP_RESERVED(a, b) PWASM_OP_RESERVED ## a,
PWASM_OP_DEFS
#undef PWASM_OP
#undef PWASM_OP_CONST
#undef PWASM_OP_RESERVED
} pwasm_op_t;

/**
 * Get opcode name.
 *
 * @ingroup type
 *
 * @param op Opcode number
 *
 * @return Pointer to the `NULL`-terminated opcode name, or a pointer to
 * the string "unknown result type" if given an invalid opcode.
 *
 * @note The strings returned by this function should not be freed.
 * @note This function never returns a `NULL` pointer.
 */
const char *pwasm_op_get_name(const pwasm_op_t);

/**
 * Get immediate type of opcode.
 *
 * @ingroup type
 *
 * @param op Opocode
 *
 * @return Immediate type of opcode.
 */
pwasm_imm_t pwasm_op_get_imm(const pwasm_op_t);

/**
 * Memory immediate.
 */
typedef struct {
  uint32_t align;
  uint32_t offset;
} pwasm_mem_imm_t;

/**
 * Decoded instruction.
 */
typedef struct {
  /// Instruction opcode.
  pwasm_op_t op;

  union {
    /**
     * Data for `block`, `loop`, and `if` instructions.
     */
    struct {
      /// block result type
      pwasm_result_type_t type;

      /// offset to `else` instruction (`if` only).
      size_t else_ofs;

      /// Offset to `end` instruction.
      size_t end_ofs;
    } v_block;

    /**
     * Data for `br_table` instruction.
     *
     * Slice of `u32s` containing branch targets.
     */
    pwasm_slice_t v_br_table;

    /**
     * Data for `br`, `br_if`, `call`, `call_indirect`, `local.get`,
     * `local.set`, `local.tee`, `global.get`, and `global.set`
     * instructions.
     */
    struct {
      uint32_t id; //< index immediate
    } v_index;

    /**
     * Memory immediate for `*.load` and `*.store` instructions.
     */
    pwasm_mem_imm_t v_mem;

    /**
     * Immediate value for `const.i32` instructions.
     */
    struct {
      uint32_t val; //< immediate i32 value
    } v_i32;

    /**
     * Immediate value for `const.i64` instructions.
     */
    struct {
      uint64_t val; //< immediate i64 value
    } v_i64;

    /**
     * Immediate value for `const.f32` instructions.
     */
    struct {
      float val; //< immediate f32 value
    } v_f32;

    /**
     * Immediate for `const.f64` instructions.
     */
    struct {
      double val; //< immediate f64 value
    } v_f64;
  };
} pwasm_inst_t;

/**
 * Global variable attributes (type and mutable flag).
 */
typedef struct {
  pwasm_value_type_t type; //< Type of global variable.
  _Bool mutable; //< Is this global variable mutable?
} pwasm_global_type_t;

// FIXME: s/function/func/, s/memory/mem/
#define PWASM_IMPORT_TYPES \
  PWASM_IMPORT_TYPE(FUNC, "func", function) \
  PWASM_IMPORT_TYPE(TABLE, "table", table) \
  PWASM_IMPORT_TYPE(MEM, "memory", memory) \
  PWASM_IMPORT_TYPE(GLOBAL, "global", global) \
  PWASM_IMPORT_TYPE(LAST, "unknown import type", invalid)

typedef enum {
#define PWASM_IMPORT_TYPE(a, b, c) PWASM_IMPORT_TYPE_##a,
PWASM_IMPORT_TYPES
#undef PWASM_IMPORT_TYPE
} pwasm_import_type_t;

/**
 * Get the name of the given import type.
 */
const char *pwasm_import_type_get_name(const pwasm_import_type_t);

#define PWASM_EXPORT_TYPES \
  PWASM_EXPORT_TYPE(FUNC, "func", func) \
  PWASM_EXPORT_TYPE(TABLE, "table", table) \
  PWASM_EXPORT_TYPE(MEM, "memory", mem) \
  PWASM_EXPORT_TYPE(GLOBAL, "global", global) \
  PWASM_EXPORT_TYPE(LAST, "unknown export type", invalid)

typedef enum {
#define PWASM_EXPORT_TYPE(a, b, c) PWASM_EXPORT_TYPE_##a,
PWASM_EXPORT_TYPES
#undef PWASM_EXPORT_TYPE
} pwasm_export_type_t;

/**
 * Get the name of the given export type.
 */
const char *pwasm_export_type_get_name(const pwasm_export_type_t);

/**
 * Local variable attributes (count and type).
 */
typedef struct {
  uint32_t num; //< Number of local variables of this value type.
  pwasm_value_type_t type; //< Value type of local variable.
} pwasm_local_t;

/**
 * Memory context callbacks.
 */
typedef struct {
  void *(*on_realloc)(void *, size_t, void *);
  void (*on_error)(const char *, void *);
} pwasm_mem_cbs_t;

/**
 * Memory context.
 */
typedef struct {
  const pwasm_mem_cbs_t *cbs;
  void *cb_data;
} pwasm_mem_ctx_t;

/**
 * Create a new memory context initialized with default values.
 */
pwasm_mem_ctx_t pwasm_mem_ctx_init_defaults(void *);

/**
 * Allocate memory from the given memory context.
 *
 * This function behaves similar to realloc():
 *
 * 1. If the pointer is NULL and the size is not zero, then this
 *    function is equivalent to malloc().
 * 2. If the pointer is non-NULL and the size is not zero, then this
 *    function is equivalent to realloc().
 * 3. If the pointer is non-NULL and the size is zero, then this
 *    function is equivalent to free().
 *
 * This function returns NULL if cases #1 or #2 fail.  It always returns
 * NULL in case #3, so you should check for error by checking for a NULL
 * return value AND a non-zero size, like so:
 *
 *     // resize memory, check for error
 *     void *new_ptr = pwasm_realloc(mem_ctx, old_ptr, new_size);
 *     if (!new_ptr && new_size > 0) {
 *       // handle error here
 *     }
 */
void *pwasm_realloc(pwasm_mem_ctx_t *, void *, const size_t);

/**
 * @defgroup vec Vectors
 */

/**
 * Resizable vector of data.
 * @ingroup vec
 */
typedef struct {
  /** memory context */
  pwasm_mem_ctx_t * const mem_ctx;

  /** entries */
  uint8_t *rows;

  /** width of each entry, in bytes */
  size_t stride;

  /** number of entries */
  size_t num_rows;

  /** capacity */
  size_t max_rows;
} pwasm_vec_t;

/**
 * Create a new vector.
 *
 * Create a new vector bound to the given memory context with the given
 * stride.
 *
 * @ingroup vec
 *
 * @param[in]   mem_ctx Memory context
 * @param[out]  vec     Destination vector
 * @param[in]   stride  Size of individual entries, in bytes
 *
 * @return `true` on success, or `false` if memory could not be
 * allocated.
 */
_Bool pwasm_vec_init(
  pwasm_mem_ctx_t *mem_ctx,
  pwasm_vec_t *,
  const size_t
);

/**
 * Finalize a vector.
 *
 * Finalize a vector and free any memory associated with it.
 *
 * @ingroup vec
 *
 * @param vec Vector
 *
 * @return `true` on success, or `false` if an error occurred.
 */
_Bool pwasm_vec_fini(pwasm_vec_t *);

/**
 * Get the number of entries in this vector.
 *
 * @ingroup vec
 *
 * @param vec Vector
 *
 * @return Number of entries.
 */
size_t pwasm_vec_get_size(const pwasm_vec_t *);

/**
 * Get a pointer to the data (rows) for this vector.
 *
 * @ingroup vec
 *
 * @param vec Vector
 *
 * @return Pointer to internal elements.
 */
const void *pwasm_vec_get_data(const pwasm_vec_t *);

/**
 * Append new entries to this vector.
 *
 * @ingroup vec
 *
 * @param[in] vec Destination vector.
 * @param[in] num_rows Number of entries to append.
 * @param[in] src Pointer to array of new entries.
 * @param[out] ret_ofs Optional pointer to output variable to write the
 * offset of the first entry in the new set of entries.
 *
 * @return `true` if the new entries, and `false` if memory could not be
 * allocated from the backing memory context.
 */
_Bool pwasm_vec_push(
  pwasm_vec_t * const vec,
  const size_t num_entries,
  const void *entries,
  size_t *ret_ofs
);

/**
 * Pop last element of vector.
 *
 * @ingroup vec
 *
 * @param[in]   vec Vector
 * @param[out]  ret Pointer to output memory of size `stride` to write
 * removed value (optional, may be `NULL`).
 *
 * @return `true` on success, or `false` on error.
 */
_Bool pwasm_vec_pop(pwasm_vec_t *vec, void *ret);

/**
 * Clear vector.
 *
 * @ingroup vec
 *
 * @param[in]   vec Vector
 */
void pwasm_vec_clear(pwasm_vec_t *);

/**
 * @defgroup mod Modules
 */

/**
 * Module section header.
 * @ingroup mod
 */
typedef struct {
  pwasm_section_type_t type; //< section type
  uint32_t len; //< section length, in bytes
} pwasm_header_t;

/**
 * Module custom section.
 * @ingroup mod
 */
typedef struct {
  pwasm_slice_t name; //< custom section name
  pwasm_slice_t data; //< custom section data
} pwasm_custom_section_t;

/**
 * Function type
 * @ingroup mod
 */
typedef struct {
  pwasm_slice_t params; //< function parameters
  pwasm_slice_t results; //< function results
} pwasm_type_t;

/**
 * Module import.
 * @ingroup mod
 */
typedef struct {
  pwasm_slice_t module; //< import module name
  pwasm_slice_t name; //< import name
  pwasm_import_type_t type; //< import type

  union {
    /** import function type index*/
    uint32_t func;

    /** import table properties */
    pwasm_table_t table;

    /** import memory limits */
    pwasm_limits_t mem;

    /** import global type */
    pwasm_global_type_t global;
  };
} pwasm_import_t;

/**
 * Module global variable.
 * @ingroup mod
 */
typedef struct {
  /** global variable type */
  pwasm_global_type_t type;

  /** global variable init constant expresssion */
  pwasm_slice_t expr;
} pwasm_global_t;

/**
 * Module function.
 * @ingroup mod
 */
typedef struct {
  /// offset of function prototype in function_types
  size_t type_id;

  /**
   * local variable slots (only used for module functions)
   *
   * @note Each local has a `num` parameter, so you cannot use .len to
   * calculate the total number of locals without iterating through /
   * the elements; use `max_locals` or `frame_size` instead.
   */
  pwasm_slice_t locals;

  /** number of local slots, excluding parameters */
  size_t max_locals;

  /** total number of local slots, including parameters */
  size_t frame_size;

  /** function instructions */
  pwasm_slice_t expr;
} pwasm_func_t;

/**
 * Module export.
 * @ingroup mod
 */
typedef struct {
  pwasm_slice_t name; //< Export name
  pwasm_export_type_t type; //< Export type
  uint32_t id; //< Export index
} pwasm_export_t;

/**
 * Module table element
 * @ingroup mod
 */
typedef struct {
  /** Table ID */
  uint32_t table_id;

  /** Offset init constant expression */
  pwasm_slice_t expr;

  /** Slice of u32s `with` function IDs */
  pwasm_slice_t funcs;
} pwasm_elem_t;

/**
 * Module memory segment
 * @ingroup mod
 */
typedef struct {
  /** Memory ID */
  uint32_t mem_id;

  /** Offset init constant expression */
  pwasm_slice_t expr;

  /** Slice of `bytes` with segment data */
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

  // TODO (https://webassembly.github.io/spec/core/appendix/custom.html)
  // void (*on_module_name)(const pwasm_buf_t, void *);
  // void (*on_func_names)(const pwasm_func_name_t *, const size_t, void *);
  // void (*on_local_names)(const pwasm_local_name_t *, const size_t, void *);

  void (*on_error)(const char *, void *);
} pwasm_mod_parse_cbs_t;

/**
 * Parse the module contained in the buffer +src+ with the given module
 * parsing callbacks and return the number of bytes consumed.
 *
 * Returns 0 on error.
 *
 * Note: You shouldn't need to call this function directly; it is called
 * by `pwasm_mod_init()`.
 *
 * @ingroup mod
 */
size_t pwasm_mod_parse(
  const pwasm_buf_t,
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

  size_t max_indices[PWASM_IMPORT_TYPE_LAST];

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

  const _Bool has_start;
  const uint32_t start;
} pwasm_mod_t;

/**
 * Parse a module from source +src+ into the module +mod+.
 *
 * Returns 0 on error.
 *
 * @ingroup mod
 */
size_t pwasm_mod_init(
  pwasm_mem_ctx_t * const mem_ctx,
  pwasm_mod_t * const mod,
  pwasm_buf_t src
);

/**
 * Finalize a module and free any memory associated with it.
 * @ingroup mod
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

/**
 * Initialize a new mod builder.
 *
 * Note: The pwasm_builder_* functions are used internally by
 * pwasm_mod_init() and pwasm_mod_init_unsafe(); you shouldn't need to
 * call them directly.
 */
_Bool pwasm_builder_init(
  pwasm_mem_ctx_t *,
  pwasm_builder_t *
);

/**
 * Finalize a mod builder and free any memory associated with it.
 *
 * Note: The pwasm_builder_* functions are used internally by
 * pwasm_mod_init() and pwasm_mod_init_unsafe(); you shouldn't need to
 * call them directly.
 */
void pwasm_builder_fini(pwasm_builder_t *);

/**
 * Populate a pwasm_mod_t structure with the parsed module data
 * contained in a pwasm_builder_t structure.
 *
 * Note: The pwasm_builder_* functions are used internally by
 * pwasm_mod_init() and pwasm_mod_init_unsafe(); you shouldn't need to
 * call them directly.
 */
_Bool pwasm_build(
  const pwasm_builder_t *,
  pwasm_mod_t *
);

/**
 * Module validation callbacks.
 * @ingroup mod
 */
typedef struct {
  /**
   * Called when a validation warning occurs.
   */
  void (*on_warning)(const char *, void *);

  /**
   * Called when a validation error occurs.
   */
  void (*on_error)(const char *, void *);
} pwasm_mod_check_cbs_t;

/**
 * Verify that a parsed module is valid.
 *
 * If a validation error occurs, and the `cbs` parameter and `on_error`
 * callback are both non-`NULL`, then the `on_error` callback will be
 * invoked an error message describing the validation error.
 *
 * @note This function is called by `pwasm_mod_init()`, so you only need
 * to call `pwasm_mod_check()` if you parsed the module with
 * `pwasm_mod_init_unsafe()`.
 *
 * @ingroup mod
 *
 * @param mod     Module
 * @param cbs     Module validation callbacks (optional, may be NULL).
 * @param cb_data Validation callback user data (optional, may be NULL).
 *
 * @return `true` if the module validates, and `false` otherwise.
 *
 * @see pwasm_mod_init()
 * @see pwasm_mod_init_unsafe()
 */
_Bool pwasm_mod_check(
  const pwasm_mod_t * const mod,
  const pwasm_mod_check_cbs_t * const cbs,
  void *cb_data
);

/**
 * @defgroup env Execution Environment
 */

/**
 * @defgroup env-low Execution Environment (Low-Level)
 */

/**
 * WebAssembly value.
 * @ingroup env
 */
typedef union {
  uint32_t i32;
  uint64_t i64;
  float    f32;
  double   f64;
} pwasm_val_t;

/**
 * Value stack.
 *
 * Stack of values used to pass parameters and results to functions in
 * an execution environment, and to store the internal state of the
 * stack machine inside the execution environment.
 *
 * @ingroup env
 */
typedef struct {
  pwasm_val_t * const ptr;
  const size_t len;
  size_t pos;
} pwasm_stack_t;

/**
 * Memory instance.
 *
 * Memory instance inside an execution environment.
 *
 * @ingroup env
 */
typedef struct {
  pwasm_buf_t buf;
  pwasm_limits_t limits;
} pwasm_env_mem_t;

/**
 * Global instance.
 */
typedef struct {
  pwasm_global_type_t type;
  pwasm_val_t val;
} pwasm_env_global_t;

/**
 * Used to peek inside a value stack.
 */
#define PWASM_PEEK(stack, ofs) ((stack)->ptr[(stack)->pos - 1 - (ofs)])

/**
 * @defgroup native Native Modules
 */

typedef struct pwasm_env_t pwasm_env_t;
typedef struct pwasm_native_t pwasm_native_t;

/**
 * Native instance.
 * @ingroup native
 * @deprecated Not used any more.  Will be removed.
 */
typedef struct {
  const uint32_t * const imports;
  const pwasm_native_t * const native;
} pwasm_native_instance_t;

/**
 * Prototype for a native function callback.
 * @ingroup native
 */
typedef _Bool (*pwasm_native_func_cb_t)(
  pwasm_env_t *env,
  const pwasm_native_t *mod
);

/**
 * Native function type.
 *
 * Used to specify the parameters and results of a native function.
 *
 * @ingroup native
 */
typedef struct {
  /** parameters */
  struct {
    const pwasm_value_type_t *ptr;
    const size_t len;
  } params;

  /** results */
  struct {
    const pwasm_value_type_t *ptr;
    const size_t len;
  } results;
} pwasm_native_type_t;

/**
 * Native function definition.
 * @ingroup native
 */
typedef struct {
  /** function name */
  const char * const name;

  /** function callback */
  const pwasm_native_func_cb_t func;

  /** function type */
  const pwasm_native_type_t type;
} pwasm_native_func_t;

/**
 * Native table definition.
 * @ingroup native
 */
typedef struct {
  /** table type (must be 0x70) */
  const pwasm_elem_type_t type; /* must be 0x70 */

  /** table elements */
  const uint32_t *vals;

  /** number of table elements */
  const size_t num_vals;
} pwasm_native_table_t;

/**
 * Native memory.
 * @ingroup native
 */
typedef struct {
  /** memory name */
  const char * const name;

  /** memory buffer (pointer and size) */
  pwasm_buf_t buf;

  /** memory limits */
  const pwasm_limits_t limits;
} pwasm_native_mem_t;

/**
 * Native global variable.
 * @ingroup native
 */
typedef struct {
  /** global variable name */
  const char * const name;

  /** global variable type */
  pwasm_global_type_t type;

  /** global variable value */
  pwasm_val_t val;
} pwasm_native_global_t;

/**
 * Native import.
 *
 * @note Not currently implemented.  May be removed.
 *
 * @ingroup native
 */
typedef struct {
  /** import type */
  pwasm_import_type_t type;

  /** import module name */
  const char *mod;

  /** import name */
  const char *name;
} pwasm_native_import_t;

/**
 * Native module.
 * @ingroup native
 */
struct pwasm_native_t {
  const size_t num_imports; //< Number of imports
  const pwasm_native_import_t * const imports; //< Imports (unused)

  const size_t num_funcs; //< Number of functions
  const pwasm_native_func_t * const funcs; //< Functions

  const size_t num_mems; //< Number of memories
  pwasm_native_mem_t * const mems; //< Memories

  const size_t num_globals; //< Number of globals
  const pwasm_native_global_t * const globals; //< Globals

  const size_t num_tables; //< Number of tables
  const pwasm_native_table_t * const tables; //< Tables
};

/**
 * Execution environment interface.
 *
 * Implement the callbacks in this structure to implement a different
 * execution environment.
 *
 * @ingroup env-low
 */
typedef struct {
  /**
   * Initialize execution environment.
   *
   * Callback to initialize execution environment internal data
   * (e.g. allocate memory).
   *
   * @param env Execution environment.
   *
   * @return `true` on success, or `false` on error.
   */
  _Bool (*init)(pwasm_env_t *);

  /**
   * Finalize execution environment.
   *
   * Callback to finalize execution environment internal data
   * (e.g. free memory).
   *
   * @param env Execution environment.
   */
  void (*fini)(pwasm_env_t *);

  /**
   * Add native module.
   *
   * Callback to add a native module to an execution environment with
   * the given name.
   *
   * @param env   Execution environment
   * @param name  Module name
   * @param null  Native module
   *
   * @return Module handle on success, or `0` on error.
   *
   * @note This callback implements `pwasm_add_native()` and
   * `pwasm_env_add_native().
   */
  uint32_t (*add_native)(
    pwasm_env_t *, // env
    const char *, // instance name
    const pwasm_native_t * // native module
  );

  /**
   * Add module.
   *
   * Callback to add a module to an execution environment with the given
   * name.
   *
   * @param env   Execution environment
   * @param name  Module name
   * @param null  module
   *
   * @return Module handle on success, or `0` on error.
   *
   * @note This callback implements `pwasm_add_mod()` and
   * `pwasm_env_add_mod().
   */
  uint32_t (*add_mod)(
    pwasm_env_t *, // env
    const char *, // instance name
    const pwasm_mod_t * // parsed module
  );

  /**
   * Get module by name.
   *
   * Callback to get a module from an execution environment with the
   * given name.
   *
   * @param env   Execution environment
   * @param name  Module name
   *
   * @return Module handle on success, or `0` on error.
   *
   * @note This callback implements `pwasm_find_mod()` and
   * `pwasm_env_find_mod().
   */
  uint32_t (*find_mod)(
    pwasm_env_t *, // env
    const pwasm_buf_t // name
  );

  /**
   * Get global variable by name.
   *
   * Callback to get a global variable from an execution environment
   * and module handle with the given name.
   *
   * @param env     Execution environment
   * @param mod_id  Module handle
   * @param name    Global variable name
   *
   * @return Global variable handle on success, or `0` on error.
   *
   * @note This callback implements `pwasm_find_global()` and
   * `pwasm_env_find_global().
   */
  uint32_t (*find_global)(
    pwasm_env_t *, // env
    const uint32_t, // module ID
    pwasm_buf_t // global name
  );

  /**
   * Get global variable value.
   *
   * Callback to get the value of a global variable from an execution
   * environment.
   *
   * @param[in]   env       Execution environment
   * @param[in]   global_id Global variable handle
   * @param[out]  ret_val   Return value
   *
   * @return `true` on success, or `false` on error.
   *
   * @note This callback implements `pwasm_get_global()` and
   * `pwasm_env_get_global().
   */
  _Bool (*get_global)(
    pwasm_env_t *, // env
    const uint32_t, // global ID
    pwasm_val_t * // return value
  );

  /**
   * Set global variable value.
   *
   * Callback to set the value of a global variable in an execution
   * environment.
   *
   * @param   env       Execution environment
   * @param   global_id Global variable handle
   * @param]  val       New value
   *
   * @return `true` on success, or `false` on error.
   *
   * @note This callback implements `pwasm_set_global()` and
   * `pwasm_env_set_global().
   */
  _Bool (*set_global)(
    pwasm_env_t *, // env
    const uint32_t, // global ID
    const pwasm_val_t // value
  );

  // get table handle by mod_id and name
  // (returns zero on error)
  uint32_t (*find_table)(
    pwasm_env_t *, // env
    const uint32_t, // module ID
    pwasm_buf_t // table name
  );

  // get value of table element by table_id and offset
  // (returns false on error)
  _Bool (*get_elem)(
    pwasm_env_t *, // env
    const uint32_t, // table ID (must be zero)
    const uint32_t, // element offset
    uint32_t * // return value
  );

  // find import handle by mod_id, import type, and import name
  // (returns zero on error)
  uint32_t (*find_import)(
    pwasm_env_t *, // env
    const uint32_t, // module handle,
    pwasm_import_type_t, // import type
    const pwasm_buf_t // import name
  );

  // get function handle by mod_id and name
  // (returns zero on error)
  uint32_t (*find_func)(
    pwasm_env_t *, // env
    const uint32_t, // module handle
    pwasm_buf_t // function name
  );

  // find memory handle by mod_id and name
  // (returns zero on error)
  uint32_t (*find_mem)(
    pwasm_env_t *, // env
    const uint32_t, // module handle
    pwasm_buf_t // memory name
  );

  // get environment memory data by memory handle
  // (returns NULL on error)
  pwasm_env_mem_t *(*get_mem)(
    pwasm_env_t *, // env
    const uint32_t // memory handle
  );

  // _Bool (*call_func)(pwasm_env_t *, uint32_t);

  // invoke function
  // parameters are taken from stack
  // (returns false on error)
  _Bool (*call)(
    pwasm_env_t *, // env
    const uint32_t // function ID
  );

  // load value from memory
  // (returns false on error)
  _Bool (*mem_load)(
    pwasm_env_t *, // env
    const uint32_t, // mem_id
    const pwasm_inst_t, // instruction (memory immediate and value mask)
    const uint32_t, // offset operand
    pwasm_val_t * // return value
  );

  // store value to memory
  // (returns false on error)
  _Bool (*mem_store)(
    pwasm_env_t *, // env
    const uint32_t, // mem_id
    const pwasm_inst_t, // instruction (memory immediate and value mask)
    const uint32_t, // offset operand
    const pwasm_val_t // value
  );

  // get memory size
  // (returns false on error)
  _Bool (*mem_size)(
    pwasm_env_t *, // env
    const uint32_t, // mem_id
    uint32_t * // return value
  );

  // grow memory
  // (returns false on error)
  _Bool (*mem_grow)(
    pwasm_env_t *, // env
    const uint32_t, // mem_id
    const uint32_t, // amount to grow
    uint32_t * // return value
  );
} pwasm_env_cbs_t;

/**
 * Execution environment.
 *
 * @ingroup env-low
 */
struct pwasm_env_t {
  pwasm_mem_ctx_t *mem_ctx;   //< memory context
  const pwasm_env_cbs_t *cbs; //< execution environment callbacks
  pwasm_stack_t *stack;       //< stack pointer
  void *env_data;             //< internal environment data
  void *user_data;            //< user data
};

/**
 * Create a new execution environment.
 *
 * Create a new execution environment and initialize it with the given
 * memory context, environment callbacks, stack, and user data.
 *
 * The `cbs` parameter specifies the type of execution environment; at
 * the only implementation currently is an interpreter environment.
 *
 * You can get the interpreter execution environment callbacks by
 * calling `pwasm_new_interpreter_get_cbs()`.
 *
 * The `stack` parameter is used to pass parameters when calling module
 * functions and to get the results of function calls.
 *
 * @ingroup env
 *
 * @param[out]  env       Pointer to destination execution environment.
 * @param[in]   mem_ctx   Memory context.
 * @param[in]   cbs       Execution environment callbacks.
 * @param[in]   stack     Value stack.
 * @param[in]   user_data User data.
 *
 * @return `true` on success, or `false` on error.
 *
 * @see pwasm_env_fini()
 */
_Bool pwasm_env_init(
  pwasm_env_t *env,
  pwasm_mem_ctx_t *mem_ctx,
  const pwasm_env_cbs_t *cbs,
  pwasm_stack_t *stack,
  void *data
);

/**
 * Finalize an execution environment and free any allocated memory.
 *
 * Finalize an execution environment and free all memory associated with
 * the context.
 *
 * @ingroup env
 *
 * @param env Execution environment.
 *
 * @note This function does not free any memory associated with parsed
 * modules; use `pwasm_mod_fini()` for that.
 *
 * @see pwasm_env_init()
 */
void pwasm_env_fini(pwasm_env_t *env);

/**
 * Get the user data for an execution environment.
 *
 * @ingroup env
 *
 * @param env Execution environment.
 *
 * @return Pointer to user data provided to `pwasm_env_init()`.
 *
 * @see pwasm_env_init()
 */
void *pwasm_env_get_data(const pwasm_env_t *env);

/**
 * Add a module to environment.
 *
 * Add parsed WebAssembly module to execution environment and return a
 * handle to the module.
 *
 * @ingroup env
 *
 * @param env   Execution environment
 * @param name  Module name
 * @param mod   Module
 *
 * @return Module handle, or `0` on error.
 *
 * @see pwasm_mod_init()
 * @see pwasm_env_add_native()
 */
uint32_t pwasm_env_add_mod(
  pwasm_env_t *env,
  const char * const name,
  const pwasm_mod_t *mod
);

/**
 * Add a native module to environment.
 *
 * Add parsed WebAssembly module to execution environment and return a
 * handle to the module.
 *
 * @ingroup env
 *
 * @param env   Execution environment
 * @param name  Module name
 * @param mod   Native module
 *
 * @return Module handle, or `0` on error.
 *
 * @see pwasm_env_add_mod()
 */
uint32_t pwasm_env_add_native(
  pwasm_env_t *env,
  const char * const name,
  const pwasm_native_t *mod
);

/**
 * Find module and return handle.
 *
 * @ingroup env-low
 *
 * @param env   Execution environment
 * @param name  Module name
 *
 * @return Module handle, or `0` on error.
 */
uint32_t pwasm_env_find_mod(
  pwasm_env_t *env,
  const pwasm_buf_t name
);

/**
 * Get function handle.
 *
 * Find function in given execution environment environment and module
 * handle, and then return a handle to the function.
 *
 * @ingroup env-low
 *
 * @param env     Execution environment
 * @param mod_id  Module handle
 * @param name    Function name.
 *
 * @return Function handle, or `0` on error.
 */
uint32_t pwasm_env_find_func(
  pwasm_env_t *env,
  const uint32_t mod_id,
  const pwasm_buf_t name
);

/**
 * Get memory handle.
 *
 * Find memory in given execution environment environment and module
 * handle, and then return a handle to the memory.
 *
 * @ingroup env-low
 *
 * @param env     Execution environment
 * @param mod_id  Module handle
 * @param name    Memory name.
 *
 * @return Memory handle, or `0` on error.
 */
uint32_t pwasm_env_find_mem(
  pwasm_env_t *env,
  const uint32_t mod_id,
  const pwasm_buf_t name
);

/**
 * Get global variable handle.
 *
 * Find global variable in given execution environment environment and
 * module handle, and then return a handle to the global variable.
 *
 * @ingroup env-low
 *
 * @param env     Execution environment
 * @param mod_id  Module handle
 * @param name    Global variable name.
 *
 * @return Global variable handle, or `0` on error.
 */
uint32_t pwasm_env_find_global(
  pwasm_env_t *env,
  const uint32_t mod_id,
  const pwasm_buf_t name
);

/**
 * Get table handle.
 *
 * Find table in given execution environment environment and
 * module handle, and then return a handle to the table.
 *
 * @ingroup env-low
 *
 * @param env     Execution environment
 * @param mod_id  Module handle
 * @param name    Table name.
 *
 * @return Table handle, or `0` on error.
 */
uint32_t pwasm_env_find_table(
  pwasm_env_t *,
  const uint32_t,
  const pwasm_buf_t
);

/**
 * Call function by handle.
 *
 * Calls the function handle with the parameters stord on the stack in
 * the given execution environment `env`.
 *
 * If `pwasm_env_call()` returns successfully, then the results of the
 * function call (if any) will be stored in the stack.
 *
 * @ingroup env-low
 *
 * @param env     Execution environment.
 * @param func_id Function handle.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @note This is a low-level execution environment function; see
 * `pwasm_call()` for the high-level equivalent.
 *
 * @see pwasm_call()
 * @see pwasm_env_find_func()
 */
_Bool pwasm_env_call(
  pwasm_env_t *,
  const uint32_t
);

/**
 * Load value from memory index at given memory address.
 *
 * @ingroup env-low
 *
 * @param[in]   env     Execution environment.
 * @param[in]   mem_id  Memory handle.
 * @param[in]   inst    Instruction (used to determine mask).
 * @param[in]   offset  Offset in memory handle.
 * @param[out]  ret_val Pointer to destination value.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @see pwasm_env_mem_store()
 * @see pwasm_env_find_mem()
 */
_Bool pwasm_env_mem_load(
  pwasm_env_t *env,
  const uint32_t mem_id,
  const pwasm_inst_t inst,
  const uint32_t offset,
  pwasm_val_t *ret_val
);

/**
 * Store value to memory handle at given address.
 *
 * @ingroup env-low
 *
 * @param[in] env     Execution environment.
 * @param[in] mem_id  Memory handle.
 * @param[in] inst    Instruction (used to determine mask).
 * @param[in] offset  Offset in memory handle.
 * @param[in] val     Value to store in memory.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @see pwasm_env_mem_load()
 * @see pwasm_env_find_mem()
 */
_Bool pwasm_env_mem_store(
  pwasm_env_t * env,
  const uint32_t mem_id,
  const pwasm_inst_t inst,
  const uint32_t offset,
  const pwasm_val_t val
);

/**
 * Get the size of a given memory handle.
 *
 * Get the size in pages of the given memory handle and and returns the
 * size in the output parameter `ret_val`.
 *
 * @ingroup env-low
 *
 * @param[in]   env     Execution environment.
 * @param[in]   mem_id  Memory handle.
 * @param[out]  ret_val Pointer to destination value.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @see pwasm_env_mem_grow()
 */
_Bool pwasm_env_mem_size(
  pwasm_env_t *env,
  const uint32_t mem_id,
  uint32_t *ret_val
);

/**
 * Grow the given memory handle.
 *
 * Grows the given memory handle and and returns the new size in the
 * output parameter `ret_val`.
 *
 * @ingroup env-low
 *
 * @param[in]   env     Execution environment.
 * @param[in]   mem_id  Memory handle.
 * @param[in]   grow    Number of pages to grow memory by.
 * @param[out]  ret_val Pointer to destination value.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @see pwasm_env_mem_size()
 */
_Bool pwasm_env_mem_grow(
  pwasm_env_t *env,
  const uint32_t mem_id,
  const uint32_t grow,
  uint32_t *ret_val
);

/**
 * Get the value of a global variable.
 *
 * @ingroup env-low
 *
 * @param[in]   env       Execution environment.
 * @param[in]   global_id Global variable handle.
 * @param[out]  ret_val   Pointer to destination value.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @note This is a low-level function; see `pwasm_get_global()` for the
 * high-level counterpart.
 *
 * @see pwasm_get_global()
 * @see pwasm_env_set_global()
 */
_Bool pwasm_env_get_global(
  pwasm_env_t *env,
  const uint32_t global_id,
  pwasm_val_t *val
);

/**
 * Set the value of a global variable.
 *
 * Set the value of a global variable, given an execution environment
 * and a handle for a global variable,
 *
 * @ingroup env-low
 *
 * @param env       Execution environment.
 * @param global_id Global variable handle.
 * @param val       New value of global variable.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @note This is a low-level function; see `pwasm_set_global()` for the
 * high-level counterpart.
 *
 * @see pwasm_env_get_global()
 * @see pwasm_set_global()
 */
_Bool pwasm_env_set_global(
  pwasm_env_t *env,
  const uint32_t global_id,
  const pwasm_val_t val
);

/**
 * Get handle to import.
 *
 * Find import by module handle, import type, and import name, and then
 * return a handle to the import.
 *
 * @ingroup env-low
 *
 * @param env Execution environment.
 * @param mod_id Module handle.
 * @param type Import type.
 * @param name Import name.
 *
 * @return Import handle, or `0` on error.
 *
 * @FIXME rename to `pwasm_env_link()`?
 */
uint32_t pwasm_env_find_import(
  pwasm_env_t *env,
  const uint32_t mod_id,
  const pwasm_import_type_t type,
  const pwasm_buf_t name
);

/**
 * Find module handle by module name.
 *
 * Find module in given execution environment by module name and then
 * return a handle to the module instance.
 *
 * @ingroup env
 *
 * @param env   Execution environment
 * @param mod   Module name
 *
 * @return Module handle, or `0` on error.
 *
 * @note Convenience wrapper around pwasm_env_find_mod().
 *
 * @see pwasm_env_find_mod()
 */
uint32_t pwasm_find_mod(
  pwasm_env_t *env,
  const char *mod
);

/**
 * Get function handle by module and function name.
 *
 * Find function in given execution environment by module name and
 * function name, and then return a handle to the function instance.
 *
 * @ingroup env
 *
 * @param env   Execution environment
 * @param mod   Module name
 * @param mem   Memory name
 *
 * @return Function handle, or `0` on error.
 *
 * @note Convenience wrapper around pwasm_env_find_func().
 *
 * @see pwasm_env_find_func()
 */
uint32_t pwasm_find_func(
  pwasm_env_t *env,
  const char *mod,
  const char *func
);

/**
 * Get memory instance by module name and memory name.
 *
 * Find memory instance in given execution environment by module name
 * and memory name, and then return a pointer to the memory instance.
 *
 * @ingroup env
 *
 * @param env   Execution environment
 * @param mod   Module name
 * @param mem   Memory name
 *
 * @return Pointer to the memory instance, or `NULL` on error.
 *
 * @note This is a convenience wrapper around pwasm_env_get_mem().
 *
 * @see pwasm_env_get_mem()
 */
pwasm_env_mem_t *pwasm_get_mem(
  pwasm_env_t *,
  const char *,
  const char *
);

/**
 * Get a global variable handle by module and global name.
 *
 * Find global variable in given execution environment by module name
 * and global name and then return a handle to the global instance.
 *
 * @ingroup env
 *
 * @param env   Execution environment.
 * @param mod   Module name.
 * @param name  Global variable name.
 *
 * @return Global variable handle, or `0` if an error occurred.
 *
 * @note This is a convenience wrapper around `pwasm_env_find_global()`.
 *
 * @see pwasm_env_find_global()
 */
uint32_t pwasm_find_global(
  pwasm_env_t *,
  const char *,
  const char *
);

/**
 * Get the value of a global variable.
 *
 * Find global variable in given execution environment by module name
 * and global name and then get the current value of the global
 * variable.
 *
 * @ingroup env
 *
 * @param[in]   env     Execution environment.
 * @param[in]   mod     Module name.
 * @param[in]   name    Global variable name.
 * @param[out]  ret_val Pointer to destination value.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @note This is a convenience wrapper around pwasm_env_get_global().
 *
 * @see pwasm_set_global()
 * @see pwasm_env_get_global()
 */
_Bool pwasm_get_global(
  pwasm_env_t *env,
  const char *mod,
  const char *name,
  pwasm_val_t *ret_val
);

/**
 * Set the value of a global variable.
 *
 * Find a global variable in the given execution environment by module
 * name and global variable name, and then set the value.
 *
 * @ingroup env
 *
 * @param env   Execution environment.
 * @param mod   Module name.
 * @param name  Global variable name.
 * @param val   New value of global variable.
 *
 * @return `true` on success, or `false` if an error occurred.
 *
 * @note This is a convenience wrapper around `pwasm_env_set_global()`.
 *
 * @see pwasm_env_set_global()
 * @see pwasm_get_global()
 *
 */
_Bool pwasm_set_global(
  pwasm_env_t *env,
  const char *mod,
  const char *func,
  const pwasm_val_t val
);

/**
 * Find and invoke function in an execution environment by module
 * and function name.
 *
 * @ingroup env
 *
 * @param env   Execution environment.
 * @param mod   Module name.
 * @param func  Function name.
 *
 * @return `true` on success, and `false` if an error occurred.
 *
 * @note Convenience wrapper around pwasm_env_call().
 *
 * @see pwasm_env_call()
 */
_Bool pwasm_call(
  pwasm_env_t * const env,  //< Environment
  const char * const mod,   //< Module name
  const char * const func   //< Function name
);

/**
 * @defgroup interp Interpreter Functions
 */

/**
 * Get callbacks for old interpreter environment.
 *
 * @ingroup interp
 *
 * @return Pointer to execution environment callbacks.
 *
 * @deprecated This interpreter is not fully functional and should not
 * be used.  Use `pwasm_new_interpreter_get_cbs()` instead.
 *
 * @see pwasm_env_init()
 * @see pwasm_new_interpreter_get_cbs()
 */
const pwasm_env_cbs_t *pwasm_old_interpreter_get_cbs(void);

/**
 * Get callbacks for new interpreter environment.
 *
 * @ingroup interp
 *
 * @return Pointer to execution environment callbacks.
 *
 * @see pwasm_env_init()
 */
const pwasm_env_cbs_t *pwasm_new_interpreter_get_cbs(void);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PWASM_H */
