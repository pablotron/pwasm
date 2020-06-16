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
 * @defgroup type Types
 */

/**
 * Module section types.
 *
 * Macro used to define the `pwasm_section_type_t` enumeration and the
 * internal section type names.
 *
 * @ingroup type
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
 * Value types.
 *
 * Macro used to define the `pwasm_value_type_t` enumeration and the
 * internal value type names.
 *
 * @ingroup type
 */
#define PWASM_VALUE_TYPE_DEFS \
  PWASM_VALUE_TYPE(0x7F, I32, "i32") \
  PWASM_VALUE_TYPE(0x7E, I64, "i64") \
  PWASM_VALUE_TYPE(0x7D, F32, "f32") \
  PWASM_VALUE_TYPE(0x7C, F64, "f64") \
  PWASM_VALUE_TYPE(0x7B, V128, "v128") \
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
const char *pwasm_value_type_get_name(const pwasm_value_type_t type);

/**
 * Import/export types.
 *
 * Macro used to define the `pwasm_import_type_t` enumeration and the
 * names for import types and export types.
 *
 * @ingroup type
 */
#define PWASM_IMPORT_TYPES \
  PWASM_IMPORT_TYPE(FUNC, "func", func) \
  PWASM_IMPORT_TYPE(TABLE, "table", table) \
  PWASM_IMPORT_TYPE(MEM, "memory", mem) \
  PWASM_IMPORT_TYPE(GLOBAL, "global", global) \
  PWASM_IMPORT_TYPE(LAST, "unknown import type", invalid)

/**
 * Import/export types.
 *
 * The values in this enumeration are used to specify the type of module
 * imports and exports.
 *
 * @ingroup type
 */
typedef enum {
#define PWASM_IMPORT_TYPE(a, b, c) PWASM_IMPORT_TYPE_##a,
PWASM_IMPORT_TYPES
#undef PWASM_IMPORT_TYPE
} pwasm_import_type_t;

/**
 * Get the name of the given import type.
 */
const char *pwasm_import_type_get_name(const pwasm_import_type_t);

/**
 * Immediate types.
 *
 * Macro used to define the `pwasm_imm_t` enumeration and the
 * internal immediate type names.
 *
 * @ingroup type
 */
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
  PWASM_IMM(V128_CONST, "v128_const") \
  PWASM_IMM(LANE_INDEX, "lane_index") \
  PWASM_IMM(LAST, "invalid")

/**
 * Immediate types.
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
const char *pwasm_imm_get_name(const pwasm_imm_t type);

/**
 * Opcode Set.
 *
 * The values in this enumeration are used to specify the set that a
 * given opcode belongs to.
 *
 * @ingroup type
 */
typedef enum {
  PWASM_OPS_MAIN, /**< main */
  PWASM_OPS_TRUNC_SAT, /**< trunc_sat */
  PWASM_OPS_SIMD, /**< simd */
  PWASM_OPS_LAST, /**< sentinel */
} pwasm_ops_t;

/**
 * Opcodes
 *
 * Macro used to define the `pwasm_op_t` enumeration and the internal
 * opcode names.
 *
 * @ingroup type
 */
typedef enum {
  PWASM_OP_UNREACHABLE, /**< unreachable */
  PWASM_OP_NOP, /**< nop */
  PWASM_OP_BLOCK, /**< block */
  PWASM_OP_LOOP, /**< loop */
  PWASM_OP_IF, /**< if */
  PWASM_OP_ELSE, /**< else */
  PWASM_OP_END, /**< end */
  PWASM_OP_BR, /**< br */
  PWASM_OP_BR_IF, /**< br_if */
  PWASM_OP_BR_TABLE, /**< br_table */
  PWASM_OP_RETURN, /**< return */
  PWASM_OP_CALL, /**< call */
  PWASM_OP_CALL_INDIRECT, /**< call_indirect */
  PWASM_OP_DROP, /**< drop */
  PWASM_OP_SELECT, /**< select */
  PWASM_OP_LOCAL_GET, /**< local.get */
  PWASM_OP_LOCAL_SET, /**< local.set */
  PWASM_OP_LOCAL_TEE, /**< local.tee */
  PWASM_OP_GLOBAL_GET, /**< global.get */
  PWASM_OP_GLOBAL_SET, /**< global.set */
  PWASM_OP_I32_LOAD, /**< i32.load */
  PWASM_OP_I64_LOAD, /**< i64.load */
  PWASM_OP_F32_LOAD, /**< f32.load */
  PWASM_OP_F64_LOAD, /**< f64.load */
  PWASM_OP_I32_LOAD8_S, /**< i32.load8_s */
  PWASM_OP_I32_LOAD8_U, /**< i32.load8_u */
  PWASM_OP_I32_LOAD16_S, /**< i32.load16_s */
  PWASM_OP_I32_LOAD16_U, /**< i32.load16_u */
  PWASM_OP_I64_LOAD8_S, /**< i64.load8_s */
  PWASM_OP_I64_LOAD8_U, /**< i64.load8_u */
  PWASM_OP_I64_LOAD16_S, /**< i64.load16_s */
  PWASM_OP_I64_LOAD16_U, /**< i64.load16_u */
  PWASM_OP_I64_LOAD32_S, /**< i64.load32_s */
  PWASM_OP_I64_LOAD32_U, /**< i64.load32_u */
  PWASM_OP_I32_STORE, /**< i32.store */
  PWASM_OP_I64_STORE, /**< i64.store */
  PWASM_OP_F32_STORE, /**< f32.store */
  PWASM_OP_F64_STORE, /**< f64.store */
  PWASM_OP_I32_STORE8, /**< i32.store8 */
  PWASM_OP_I32_STORE16, /**< i32.store16 */
  PWASM_OP_I64_STORE8, /**< i64.store8 */
  PWASM_OP_I64_STORE16, /**< i64.store16 */
  PWASM_OP_I64_STORE32, /**< i64.store32 */
  PWASM_OP_MEMORY_SIZE, /**< memory.size */
  PWASM_OP_MEMORY_GROW, /**< memory.grow */
  PWASM_OP_I32_CONST, /**< i32.const */
  PWASM_OP_I64_CONST, /**< i64.const */
  PWASM_OP_F32_CONST, /**< f32.const */
  PWASM_OP_F64_CONST, /**< f64.const */
  PWASM_OP_I32_EQZ, /**< i32.eqz */
  PWASM_OP_I32_EQ, /**< i32.eq */
  PWASM_OP_I32_NE, /**< i32.ne */
  PWASM_OP_I32_LT_S, /**< i32.lt_s */
  PWASM_OP_I32_LT_U, /**< i32.lt_u */
  PWASM_OP_I32_GT_S, /**< i32.gt_s */
  PWASM_OP_I32_GT_U, /**< i32.gt_u */
  PWASM_OP_I32_LE_S, /**< i32.le_s */
  PWASM_OP_I32_LE_U, /**< i32.le_u */
  PWASM_OP_I32_GE_S, /**< i32.ge_s */
  PWASM_OP_I32_GE_U, /**< i32.ge_u */
  PWASM_OP_I64_EQZ, /**< i64.eqz */
  PWASM_OP_I64_EQ, /**< i64.eq */
  PWASM_OP_I64_NE, /**< i64.ne */
  PWASM_OP_I64_LT_S, /**< i64.lt_s */
  PWASM_OP_I64_LT_U, /**< i64.lt_u */
  PWASM_OP_I64_GT_S, /**< i64.gt_s */
  PWASM_OP_I64_GT_U, /**< i64.gt_u */
  PWASM_OP_I64_LE_S, /**< i64.le_s */
  PWASM_OP_I64_LE_U, /**< i64.le_u */
  PWASM_OP_I64_GE_S, /**< i64.ge_s */
  PWASM_OP_I64_GE_U, /**< i64.ge_u */
  PWASM_OP_F32_EQ, /**< f32.eq */
  PWASM_OP_F32_NE, /**< f32.ne */
  PWASM_OP_F32_LT, /**< f32.lt */
  PWASM_OP_F32_GT, /**< f32.gt */
  PWASM_OP_F32_LE, /**< f32.le */
  PWASM_OP_F32_GE, /**< f32.ge */
  PWASM_OP_F64_EQ, /**< f64.eq */
  PWASM_OP_F64_NE, /**< f64.ne */
  PWASM_OP_F64_LT, /**< f64.lt */
  PWASM_OP_F64_GT, /**< f64.gt */
  PWASM_OP_F64_LE, /**< f64.le */
  PWASM_OP_F64_GE, /**< f64.ge */
  PWASM_OP_I32_CLZ, /**< i32.clz */
  PWASM_OP_I32_CTZ, /**< i32.ctz */
  PWASM_OP_I32_POPCNT, /**< i32.popcnt */
  PWASM_OP_I32_ADD, /**< i32.add */
  PWASM_OP_I32_SUB, /**< i32.sub */
  PWASM_OP_I32_MUL, /**< i32.mul */
  PWASM_OP_I32_DIV_S, /**< i32.div_s */
  PWASM_OP_I32_DIV_U, /**< i32.div_u */
  PWASM_OP_I32_REM_S, /**< i32.rem_s */
  PWASM_OP_I32_REM_U, /**< i32.rem_u */
  PWASM_OP_I32_AND, /**< i32.and */
  PWASM_OP_I32_OR, /**< i32.or */
  PWASM_OP_I32_XOR, /**< i32.xor */
  PWASM_OP_I32_SHL, /**< i32.shl */
  PWASM_OP_I32_SHR_S, /**< i32.shr_s */
  PWASM_OP_I32_SHR_U, /**< i32.shr_u */
  PWASM_OP_I32_ROTL, /**< i32.rotl */
  PWASM_OP_I32_ROTR, /**< i32.rotr */
  PWASM_OP_I64_CLZ, /**< i64.clz */
  PWASM_OP_I64_CTZ, /**< i64.ctz */
  PWASM_OP_I64_POPCNT, /**< i64.popcnt */
  PWASM_OP_I64_ADD, /**< i64.add */
  PWASM_OP_I64_SUB, /**< i64.sub */
  PWASM_OP_I64_MUL, /**< i64.mul */
  PWASM_OP_I64_DIV_S, /**< i64.div_s */
  PWASM_OP_I64_DIV_U, /**< i64.div_u */
  PWASM_OP_I64_REM_S, /**< i64.rem_s */
  PWASM_OP_I64_REM_U, /**< i64.rem_u */
  PWASM_OP_I64_AND, /**< i64.and */
  PWASM_OP_I64_OR, /**< i64.or */
  PWASM_OP_I64_XOR, /**< i64.xor */
  PWASM_OP_I64_SHL, /**< i64.shl */
  PWASM_OP_I64_SHR_S, /**< i64.shr_s */
  PWASM_OP_I64_SHR_U, /**< i64.shr_u */
  PWASM_OP_I64_ROTL, /**< i64.rotl */
  PWASM_OP_I64_ROTR, /**< i64.rotr */
  PWASM_OP_F32_ABS, /**< f32.abs */
  PWASM_OP_F32_NEG, /**< f32.neg */
  PWASM_OP_F32_CEIL, /**< f32.ceil */
  PWASM_OP_F32_FLOOR, /**< f32.floor */
  PWASM_OP_F32_TRUNC, /**< f32.trunc */
  PWASM_OP_F32_NEAREST, /**< f32.nearest */
  PWASM_OP_F32_SQRT, /**< f32.sqrt */
  PWASM_OP_F32_ADD, /**< f32.add */
  PWASM_OP_F32_SUB, /**< f32.sub */
  PWASM_OP_F32_MUL, /**< f32.mul */
  PWASM_OP_F32_DIV, /**< f32.div */
  PWASM_OP_F32_MIN, /**< f32.min */
  PWASM_OP_F32_MAX, /**< f32.max */
  PWASM_OP_F32_COPYSIGN, /**< f32.copysign */
  PWASM_OP_F64_ABS, /**< f64.abs */
  PWASM_OP_F64_NEG, /**< f64.neg */
  PWASM_OP_F64_CEIL, /**< f64.ceil */
  PWASM_OP_F64_FLOOR, /**< f64.floor */
  PWASM_OP_F64_TRUNC, /**< f64.trunc */
  PWASM_OP_F64_NEAREST, /**< f64.nearest */
  PWASM_OP_F64_SQRT, /**< f64.sqrt */
  PWASM_OP_F64_ADD, /**< f64.add */
  PWASM_OP_F64_SUB, /**< f64.sub */
  PWASM_OP_F64_MUL, /**< f64.mul */
  PWASM_OP_F64_DIV, /**< f64.div */
  PWASM_OP_F64_MIN, /**< f64.min */
  PWASM_OP_F64_MAX, /**< f64.max */
  PWASM_OP_F64_COPYSIGN, /**< f64.copysign */
  PWASM_OP_I32_WRAP_I64, /**< i32.wrap_i64 */
  PWASM_OP_I32_TRUNC_F32_S, /**< i32.trunc_f32_s */
  PWASM_OP_I32_TRUNC_F32_U, /**< i32.trunc_f32_u */
  PWASM_OP_I32_TRUNC_F64_S, /**< i32.trunc_f64_s */
  PWASM_OP_I32_TRUNC_F64_U, /**< i32.trunc_f64_u */
  PWASM_OP_I64_EXTEND_I32_S, /**< i64.extend_i32_s */
  PWASM_OP_I64_EXTEND_I32_U, /**< i64.extend_i32_u */
  PWASM_OP_I64_TRUNC_F32_S, /**< i64.trunc_f32_s */
  PWASM_OP_I64_TRUNC_F32_U, /**< i64.trunc_f32_u */
  PWASM_OP_I64_TRUNC_F64_S, /**< i64.trunc_f64_s */
  PWASM_OP_I64_TRUNC_F64_U, /**< i64.trunc_f64_u */
  PWASM_OP_F32_CONVERT_I32_S, /**< f32.convert_i32_s */
  PWASM_OP_F32_CONVERT_I32_U, /**< f32.convert_i32_u */
  PWASM_OP_F32_CONVERT_I64_S, /**< f32.convert_i64_s */
  PWASM_OP_F32_CONVERT_I64_U, /**< f32.convert_i64_u */
  PWASM_OP_F32_DEMOTE_F64, /**< f32.demote_f64 */
  PWASM_OP_F64_CONVERT_I32_S, /**< f64.convert_i32_s */
  PWASM_OP_F64_CONVERT_I32_U, /**< f64.convert_i32_u */
  PWASM_OP_F64_CONVERT_I64_S, /**< f64.convert_i64_s */
  PWASM_OP_F64_CONVERT_I64_U, /**< f64.convert_i64_u */
  PWASM_OP_F64_PROMOTE_F32, /**< f64.promote_f32 */
  PWASM_OP_I32_REINTERPRET_F32, /**< i32.reinterpret_f32 */
  PWASM_OP_I64_REINTERPRET_F64, /**< i64.reinterpret_f64 */
  PWASM_OP_F32_REINTERPRET_I32, /**< f32.reinterpret_i32 */
  PWASM_OP_F64_REINTERPRET_I64, /**< f64.reinterpret_i64 */
  PWASM_OP_I32_EXTEND8_S, /**< i32.extend8_s */
  PWASM_OP_I32_EXTEND16_S, /**< i32.extend16_s */
  PWASM_OP_I64_EXTEND8_S, /**< i64.extend8_s */
  PWASM_OP_I64_EXTEND16_S, /**< i64.extend16_s */
  PWASM_OP_I64_EXTEND32_S, /**< i64.extend32_s */
  PWASM_OP_I32_TRUNC_SAT_F32_S, /**< i32.trunc_sat_f32_s */
  PWASM_OP_I32_TRUNC_SAT_F32_U, /**< i32.trunc_sat_f32_u */
  PWASM_OP_I32_TRUNC_SAT_F64_S, /**< i32.trunc_sat_f64_s */
  PWASM_OP_I32_TRUNC_SAT_F64_U, /**< i32.trunc_sat_f64_u */
  PWASM_OP_I64_TRUNC_SAT_F32_S, /**< i64.trunc_sat_f32_s */
  PWASM_OP_I64_TRUNC_SAT_F32_U, /**< i64.trunc_sat_f32_u */
  PWASM_OP_I64_TRUNC_SAT_F64_S, /**< i64.trunc_sat_f64_s */
  PWASM_OP_I64_TRUNC_SAT_F64_U, /**< i64.trunc_sat_f64_u */
  PWASM_OP_V128_LOAD, /**< v128.load */
  PWASM_OP_I16X8_LOAD8X8_S, /**< i16x8.load8x8_s */
  PWASM_OP_I16X8_LOAD8X8_U, /**< i16x8.load8x8_u */
  PWASM_OP_I32X4_LOAD16X4_S, /**< i32x4.load16x4_s */
  PWASM_OP_I32X4_LOAD16X4_U, /**< i32x4.load16x4_u */
  PWASM_OP_I64X2_LOAD32X2_S, /**< i64x2.load32x2_s */
  PWASM_OP_I64X2_LOAD32X2_U, /**< i64x2.load32x2_u */
  PWASM_OP_V8X16_LOAD_SPLAT, /**< v8x16.load_splat */
  PWASM_OP_V16X8_LOAD_SPLAT, /**< v16x8.load_splat */
  PWASM_OP_V32X4_LOAD_SPLAT, /**< v32x4.load_splat */
  PWASM_OP_V64X2_LOAD_SPLAT, /**< v64x2.load_splat */
  PWASM_OP_V128_STORE, /**< v128.store */
  PWASM_OP_V128_CONST, /**< v128.const */
  PWASM_OP_V8X16_SHUFFLE, /**< v8x16.shuffle */
  PWASM_OP_V8X16_SWIZZLE, /**< v8x16.swizzle */
  PWASM_OP_I8X16_SPLAT, /**< i8x16.splat */
  PWASM_OP_I16X8_SPLAT, /**< i16x8.splat */
  PWASM_OP_I32X4_SPLAT, /**< i32x4.splat */
  PWASM_OP_I64X2_SPLAT, /**< i64x2.splat */
  PWASM_OP_F32X4_SPLAT, /**< f32x4.splat */
  PWASM_OP_F64X2_SPLAT, /**< f64x2.splat */
  PWASM_OP_I8X16_EXTRACT_LANE_S, /**< i8x16.extract_lane_s */
  PWASM_OP_I8X16_EXTRACT_LANE_U, /**< i8x16.extract_lane_u */
  PWASM_OP_I8X16_REPLACE_LANE, /**< i8x16.replace_lane */
  PWASM_OP_I16X8_EXTRACT_LANE_S, /**< i16x8.extract_lane_s */
  PWASM_OP_I16X8_EXTRACT_LANE_U, /**< i16x8.extract_lane_u */
  PWASM_OP_I16X8_REPLACE_LANE, /**< i16x8.replace_lane */
  PWASM_OP_I32X4_EXTRACT_LANE, /**< i32x4.extract_lane */
  PWASM_OP_I32X4_REPLACE_LANE, /**< i32x4.replace_lane */
  PWASM_OP_I64X2_EXTRACT_LANE, /**< i64x2.extract_lane */
  PWASM_OP_I64X2_REPLACE_LANE, /**< i64x2.replace_lane */
  PWASM_OP_F32X4_EXTRACT_LANE, /**< f32x4.extract_lane */
  PWASM_OP_F32X4_REPLACE_LANE, /**< f32x4.replace_lane */
  PWASM_OP_F64X2_EXTRACT_LANE, /**< f64x2.extract_lane */
  PWASM_OP_F64X2_REPLACE_LANE, /**< f64x2.replace_lane */
  PWASM_OP_I8X16_EQ, /**< i8x16.eq */
  PWASM_OP_I8X16_NE, /**< i8x16.ne */
  PWASM_OP_I8X16_LT_S, /**< i8x16.lt_s */
  PWASM_OP_I8X16_LT_U, /**< i8x16.lt_u */
  PWASM_OP_I8X16_GT_S, /**< i8x16.gt_s */
  PWASM_OP_I8X16_GT_U, /**< i8x16.gt_u */
  PWASM_OP_I8X16_LE_S, /**< i8x16.le_s */
  PWASM_OP_I8X16_LE_U, /**< i8x16.le_u */
  PWASM_OP_I8X16_GE_S, /**< i8x16.ge_s */
  PWASM_OP_I8X16_GE_U, /**< i8x16.ge_u */
  PWASM_OP_I16X8_EQ, /**< i16x8.eq */
  PWASM_OP_I16X8_NE, /**< i16x8.ne */
  PWASM_OP_I16X8_LT_S, /**< i16x8.lt_s */
  PWASM_OP_I16X8_LT_U, /**< i16x8.lt_u */
  PWASM_OP_I16X8_GT_S, /**< i16x8.gt_s */
  PWASM_OP_I16X8_GT_U, /**< i16x8.gt_u */
  PWASM_OP_I16X8_LE_S, /**< i16x8.le_s */
  PWASM_OP_I16X8_LE_U, /**< i16x8.le_u */
  PWASM_OP_I16X8_GE_S, /**< i16x8.ge_s */
  PWASM_OP_I16X8_GE_U, /**< i16x8.ge_u */
  PWASM_OP_I32X4_EQ, /**< i32x4.eq */
  PWASM_OP_I32X4_NE, /**< i32x4.ne */
  PWASM_OP_I32X4_LT_S, /**< i32x4.lt_s */
  PWASM_OP_I32X4_LT_U, /**< i32x4.lt_u */
  PWASM_OP_I32X4_GT_S, /**< i32x4.gt_s */
  PWASM_OP_I32X4_GT_U, /**< i32x4.gt_u */
  PWASM_OP_I32X4_LE_S, /**< i32x4.le_s */
  PWASM_OP_I32X4_LE_U, /**< i32x4.le_u */
  PWASM_OP_I32X4_GE_S, /**< i32x4.ge_s */
  PWASM_OP_I32X4_GE_U, /**< i32x4.ge_u */
  PWASM_OP_F32X4_EQ, /**< f32x4.eq */
  PWASM_OP_F32X4_NE, /**< f32x4.ne */
  PWASM_OP_F32X4_LT, /**< f32x4.lt */
  PWASM_OP_F32X4_GT, /**< f32x4.gt */
  PWASM_OP_F32X4_LE, /**< f32x4.le */
  PWASM_OP_F32X4_GE, /**< f32x4.ge */
  PWASM_OP_F64X2_EQ, /**< f64x2.eq */
  PWASM_OP_F64X2_NE, /**< f64x2.ne */
  PWASM_OP_F64X2_LT, /**< f64x2.lt */
  PWASM_OP_F64X2_GT, /**< f64x2.gt */
  PWASM_OP_F64X2_LE, /**< f64x2.le */
  PWASM_OP_F64X2_GE, /**< f64x2.ge */
  PWASM_OP_V128_NOT, /**< v128.not */
  PWASM_OP_V128_AND, /**< v128.and */
  PWASM_OP_V128_ANDNOT, /**< v128.andnot */
  PWASM_OP_V128_OR, /**< v128.or */
  PWASM_OP_V128_XOR, /**< v128.xor */
  PWASM_OP_V128_BITSELECT, /**< v128.bitselect */
  PWASM_OP_I8X16_ABS, /**< i8x16.abs */
  PWASM_OP_I8X16_NEG, /**< i8x16.neg */
  PWASM_OP_I8X16_ANY_TRUE, /**< i8x16.any_true */
  PWASM_OP_I8X16_ALL_TRUE, /**< i8x16.all_true */
  PWASM_OP_I8X16_NARROW_I16X8_S, /**< i8x16.narrow_i16x8_s */
  PWASM_OP_I8X16_NARROW_I16X8_U, /**< i8x16.narrow_i16x8_u */
  PWASM_OP_I8X16_SHL, /**< i8x16.shl */
  PWASM_OP_I8X16_SHR_S, /**< i8x16.shr_s */
  PWASM_OP_I8X16_SHR_U, /**< i8x16.shr_u */
  PWASM_OP_I8X16_ADD, /**< i8x16.add */
  PWASM_OP_I8X16_ADD_SATURATE_S, /**< i8x16.add_saturate_s */
  PWASM_OP_I8X16_ADD_SATURATE_U, /**< i8x16.add_saturate_u */
  PWASM_OP_I8X16_SUB, /**< i8x16.sub */
  PWASM_OP_I8X16_SUB_SATURATE_S, /**< i8x16.sub_saturate_s */
  PWASM_OP_I8X16_SUB_SATURATE_U, /**< i8x16.sub_saturate_u */
  PWASM_OP_I8X16_MIN_S, /**< i8x16.min_s */
  PWASM_OP_I8X16_MIN_U, /**< i8x16.min_u */
  PWASM_OP_I8X16_MAX_S, /**< i8x16.max_s */
  PWASM_OP_I8X16_MAX_U, /**< i8x16.max_u */
  PWASM_OP_I8X16_AVGR_U, /**< i8x16.avgr_u */
  PWASM_OP_I16X8_ABS, /**< i16x8.abs */
  PWASM_OP_I16X8_NEG, /**< i16x8.neg */
  PWASM_OP_I16X8_ANY_TRUE, /**< i16x8.any_true */
  PWASM_OP_I16X8_ALL_TRUE, /**< i16x8.all_true */
  PWASM_OP_I16X8_NARROW_I32X4_S, /**< i16x8.narrow_i32x4_s */
  PWASM_OP_I16X8_NARROW_I32X4_U, /**< i16x8.narrow_i32x4_u */
  PWASM_OP_I16X8_WIDEN_LOW_I8X16_S, /**< i16x8.widen_low_i8x16_s */
  PWASM_OP_I16X8_WIDEN_HIGH_I8X16_S, /**< i16x8.widen_high_i8x16_s */
  PWASM_OP_I16X8_WIDEN_LOW_I8X16_U, /**< i16x8.widen_low_i8x16_u */
  PWASM_OP_I16X8_WIDEN_HIGH_I8X16_U, /**< i16x8.widen_high_i8x16_u */
  PWASM_OP_I16X8_SHL, /**< i16x8.shl */
  PWASM_OP_I16X8_SHR_S, /**< i16x8.shr_s */
  PWASM_OP_I16X8_SHR_U, /**< i16x8.shr_u */
  PWASM_OP_I16X8_ADD, /**< i16x8.add */
  PWASM_OP_I16X8_ADD_SATURATE_S, /**< i16x8.add_saturate_s */
  PWASM_OP_I16X8_ADD_SATURATE_U, /**< i16x8.add_saturate_u */
  PWASM_OP_I16X8_SUB, /**< i16x8.sub */
  PWASM_OP_I16X8_SUB_SATURATE_S, /**< i16x8.sub_saturate_s */
  PWASM_OP_I16X8_SUB_SATURATE_U, /**< i16x8.sub_saturate_u */
  PWASM_OP_I16X8_MUL, /**< i16x8.mul */
  PWASM_OP_I16X8_MIN_S, /**< i16x8.min_s */
  PWASM_OP_I16X8_MIN_U, /**< i16x8.min_u */
  PWASM_OP_I16X8_MAX_S, /**< i16x8.max_s */
  PWASM_OP_I16X8_MAX_U, /**< i16x8.max_u */
  PWASM_OP_I16X8_AVGR_U, /**< i16x8.avgr_u */
  PWASM_OP_I32X4_ABS, /**< i32x4.abs */
  PWASM_OP_I32X4_NEG, /**< i32x4.neg */
  PWASM_OP_I32X4_ANY_TRUE, /**< i32x4.any_true */
  PWASM_OP_I32X4_ALL_TRUE, /**< i32x4.all_true */
  PWASM_OP_I32X4_WIDEN_LOW_I16X8_S, /**< i32x4.widen_low_i16x8_s */
  PWASM_OP_I32X4_WIDEN_HIGH_I16X8_S, /**< i32x4.widen_high_i16x8_s */
  PWASM_OP_I32X4_WIDEN_LOW_I16X8_U, /**< i32x4.widen_low_i16x8_u */
  PWASM_OP_I32X4_WIDEN_HIGH_I16X8_U, /**< i32x4.widen_high_i16x8_u */
  PWASM_OP_I32X4_SHL, /**< i32x4.shl */
  PWASM_OP_I32X4_SHR_S, /**< i32x4.shr_s */
  PWASM_OP_I32X4_SHR_U, /**< i32x4.shr_u */
  PWASM_OP_I32X4_ADD, /**< i32x4.add */
  PWASM_OP_I32X4_SUB, /**< i32x4.sub */
  PWASM_OP_I32X4_MUL, /**< i32x4.mul */
  PWASM_OP_I32X4_MIN_S, /**< i32x4.min_s */
  PWASM_OP_I32X4_MIN_U, /**< i32x4.min_u */
  PWASM_OP_I32X4_MAX_S, /**< i32x4.max_s */
  PWASM_OP_I32X4_MAX_U, /**< i32x4.max_u */
  PWASM_OP_I64X2_NEG, /**< i64x2.neg */
  PWASM_OP_I64X2_SHL, /**< i64x2.shl */
  PWASM_OP_I64X2_SHR_S, /**< i64x2.shr_s */
  PWASM_OP_I64X2_SHR_U, /**< i64x2.shr_u */
  PWASM_OP_I64X2_ADD, /**< i64x2.add */
  PWASM_OP_I64X2_SUB, /**< i64x2.sub */
  PWASM_OP_I64X2_MUL, /**< i64x2.mul */
  PWASM_OP_F32X4_ABS, /**< f32x4.abs */
  PWASM_OP_F32X4_NEG, /**< f32x4.neg */
  PWASM_OP_F32X4_SQRT, /**< f32x4.sqrt */
  PWASM_OP_F32X4_ADD, /**< f32x4.add */
  PWASM_OP_F32X4_SUB, /**< f32x4.sub */
  PWASM_OP_F32X4_MUL, /**< f32x4.mul */
  PWASM_OP_F32X4_DIV, /**< f32x4.div */
  PWASM_OP_F32X4_MIN, /**< f32x4.min */
  PWASM_OP_F32X4_MAX, /**< f32x4.max */
  PWASM_OP_F64X2_ABS, /**< f64x2.abs */
  PWASM_OP_F64X2_NEG, /**< f64x2.neg */
  PWASM_OP_F64X2_SQRT, /**< f64x2.sqrt */
  PWASM_OP_F64X2_ADD, /**< f64x2.add */
  PWASM_OP_F64X2_SUB, /**< f64x2.sub */
  PWASM_OP_F64X2_MUL, /**< f64x2.mul */
  PWASM_OP_F64X2_DIV, /**< f64x2.div */
  PWASM_OP_F64X2_MIN, /**< f64x2.min */
  PWASM_OP_F64X2_MAX, /**< f64x2.max */
  PWASM_OP_I32X4_TRUNC_SAT_F32X4_S, /**< i32x4.trunc_sat_f32x4_s */
  PWASM_OP_I32X4_TRUNC_SAT_F32X4_U, /**< i32x4.trunc_sat_f32x4_u */
  PWASM_OP_F32X4_CONVERT_I32X4_S, /**< f32x4.convert_i32x4_s */
  PWASM_OP_F32X4_CONVERT_I32X4_U, /**< f32x4.convert_i32x4_u */
  PWASM_OP_LAST, /**< sentinel */
} pwasm_op_t;

/**
 * Structure containing opcode properties.
 *
 * @ingroup type
 */
typedef struct {
  const pwasm_ops_t set;
  const char * const name;
  const uint8_t bytes[4];
  const size_t num_bytes;
  const pwasm_imm_t imm;
  const size_t num_lanes;
  const size_t mem_size;
} pwasm_op_data_t;

/**
 * Get opcode data.
 *
 * @ingroup type
 *
 * @param op Opcode
 *
 * @return Constant pointer to opcode data, or `NULL` if the opcode is
 * invalid.
 */
const pwasm_op_data_t *pwasm_op_get_data(pwasm_op_t);

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
 * @param op Opcode
 *
 * @return Immediate type of opcode.
 */
pwasm_imm_t pwasm_op_get_imm(const pwasm_op_t);

/**
 * @defgroup util Utilities
 */

/**
 * Buffer structure a memory pointer and a length, in bytes.
 *
 * @ingroup util
 */
typedef struct {
  const uint8_t *ptr; ///< Pointer to backing memory for this buffer.
  size_t len;         ///< Size of this buffer, in bytes.
} pwasm_buf_t;

/**
 * Structure used to represent a subset of a larger memory buffer or
 * group of values.
 *
 * @ingroup util
 */
typedef struct {
  size_t ofs; ///< Offset of first element.
  size_t len; ///< Total length of slice.
} pwasm_slice_t;

/**
 * 128-bit SIMD vector value.
 * @ingroup type
 */
typedef union {
  uint8_t i8[16];
  uint16_t i16[8];
  uint32_t i32[4];
  uint64_t i64[2];
  float f32[4];
  double f64[2];
} pwasm_v128_t;

/**
 * Representation of WebAssembly limits.
 *
 * Limits always have a lower bound, but not necessarily an upper bound.
 * If specified, the upper bound must be greater than or equal to the
 * lower bound.
 *
 * The units for the values in a limits structure vary depending on use.
 * For example, when associated with a memory instance, the limit
 * represent memory pages, and when associated with a table instance,
 * the limits represent number of elements.
 *
 * @ingroup mod
 */
typedef struct {
  uint32_t min;   ///< Lower bound.
  uint32_t max;   ///< Upper bound.
  _Bool has_max;  ///< Does this structure have an upper bound?
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
  pwasm_elem_type_t elem_type;  ///< Table type.  Must be `0x70`.
  pwasm_limits_t limits;        ///< Limits for this table.
} pwasm_table_t;

/**
 * Memory immediate.
 * @ingroup mem
 */
typedef struct {
  uint32_t align; ///< alignment immediate
  uint32_t offset; ///< offset immediate
} pwasm_mem_imm_t;

/**
 * Decoded instruction.
 */
typedef struct {
  /** Instruction opcode */
  pwasm_op_t op;

  union {
    /**
     * Data for `block`, `loop`, and `if` instructions.
     */
    struct {
      /// block result type
      int32_t block_type;

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
     * Index immediate for `br`, `br_if`, `call`, `call_indirect`,
     * `local.get`, `local.set`, `local.tee`, `global.get`, and
     * `global.set` instructions.
     *
     * @note Eventually `call_indirect` will need it's own
     * immediate to handle the table index.
     */
    uint32_t v_index;

    /**
     * Memory immediate for `*.load` and `*.store` instructions.
     */
    pwasm_mem_imm_t v_mem;

    /**
     * Immediate value for `i32.const` instructions.
     */
    uint32_t v_i32;

    /**
     * Immediate value for `i64.const` instructions.
     */
    uint64_t v_i64;

    /**
     * Immediate value for `f32.const` instructions.
     */
    float v_f32;

    /**
     * Immediate for `f64.const` instructions.
     */
    double v_f64;

    /**
     * Immediate for `v128.const` and `v8x16` instructions.
     */
    pwasm_v128_t v_v128;
  };
} pwasm_inst_t;

/**
 * @defgroup mem Memory Context
 */

/**
 * Memory context callbacks.
 * @ingroup mem
 */
typedef struct {
  /**
   * Callback that is invoked to allocate, resize, or free memory.
   */
  void *(*on_realloc)(void *, size_t, void *);

  /**
   * Callback that is invoked when an error occurs.
   */
  void (*on_error)(const char *, void *);
} pwasm_mem_cbs_t;

/**
 * Memory context.
 * @ingroup mem
 */
typedef struct {
  const pwasm_mem_cbs_t *cbs; ///< Memory context callbacks
  void *cb_data; ///< Callback data
} pwasm_mem_ctx_t;

/**
 * Create a new memory context initialized with default values.
 * @ingroup mem
 */
pwasm_mem_ctx_t pwasm_mem_ctx_init_defaults(void *);

/**
 * Allocate, resize, or free memory.
 *
 * Allocate, resize, or free memory from the given memory context.
 *
 * This function behaves similar to `realloc()`:
 *
 * 1. If `ptr` is `NULL` and `size` is not zero, then this function is
 *    equivalent to `malloc()`.
 * 2. If `ptr` is non-`NULL` and `size` is not zero, then this function
 *    is equivalent to `realloc()`.
 * 3. If `ptr` is non-`NULL` and `size` is zero, then this function is
 *    equivalent to `free()`.
 *
 * @note `pwasm_realloc()` returns `NULL` if cases #1 or #2 fail.  It
 * always returns `NULL` in case #3, so you should check for error by
 * checking for a `NULL` return value AND a non-zero `size`, like in the
 * following example.
 *
 * @code{.c}
 * // resize memory, check for error
 * void *new_ptr = pwasm_realloc(mem_ctx, old_ptr, new_size);
 * if (!new_ptr && new_size > 0) {
 *   // handle error here
 * }
 * @endcode
 *
 * @ingroup mem
 *
 * @param mem_ctx Memory context
 * @param ptr     Memory pointer, or NULL
 * @param size    Number of bytes.
 *
 * @return Memory pointer or `NULL` on error.  Also returns `NULL` if
 * `size` is sero.
 */
void *pwasm_realloc(
  pwasm_mem_ctx_t *mem_ctx,
  void *ptr,
  const size_t size
);

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
  pwasm_vec_t *vec,
  const size_t stride
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
_Bool pwasm_vec_fini(pwasm_vec_t *vec);

/**
 * Get the number of entries in this vector.
 *
 * @ingroup vec
 *
 * @param vec Vector
 *
 * @return Number of entries.
 */
size_t pwasm_vec_get_size(const pwasm_vec_t *vec);

/**
 * Get a pointer to the data (rows) for this vector.
 *
 * @ingroup vec
 *
 * @param vec Vector
 *
 * @return Pointer to internal elements.
 */
const void *pwasm_vec_get_data(const pwasm_vec_t *vec);

/**
 * Append new entries to this vector.
 *
 * @ingroup vec
 *
 * @param[in] vec         Destination vector.
 * @param[in] num_entries Number of entries to append.
 * @param[in] entries     Pointer to array of new entries.
 * @param[out] ret_ofs    Optional pointer to output variable to write
 * the offset of the first entry in the new set of entries.
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
 * @ingroup vec
 * @param vec Vector
 */
void pwasm_vec_clear(pwasm_vec_t *vec);

/**
 * @defgroup mod Modules
 */

/**
 * Module section header.
 * @ingroup mod
 */
typedef struct {
  pwasm_section_type_t type; ///< section type
  uint32_t len; ///< section length, in bytes
} pwasm_header_t;

/**
 * Module custom section.
 * @ingroup mod
 */
typedef struct {
  pwasm_slice_t name; ///< custom section name
  pwasm_slice_t data; ///< custom section data
} pwasm_custom_section_t;

/**
 * Function type.
 *
 * This structure contains the value types and count of parameters and
 * results for a function type.
 *
 * @ingroup mod
 */
typedef struct {
  pwasm_slice_t params; ///< function parameters
  pwasm_slice_t results; ///< function results
} pwasm_type_t;

/**
 * Global variable type.
 *
 * This structure contains the value type and mutable flag properties of
 * a global variable.
 *
 * @ingroup mod
 */
typedef struct {
  pwasm_value_type_t type; ///< Value type of global variable.
  _Bool mutable; ///< Is this global variable mutable?
} pwasm_global_type_t;

/**
 * Module import.
 * @ingroup mod
 */
typedef struct {
  pwasm_slice_t module; ///< import module name
  pwasm_slice_t name; ///< import name
  pwasm_import_type_t type; ///< import type

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
 * Local variable properties (count and type).
 * @ingroup mod
 */
typedef struct {
  uint32_t num; ///< Number of local variables of this value type.
  pwasm_value_type_t type; ///< Value type of local variable.
} pwasm_local_t;

/**
 * Module function.
 * @ingroup mod
 */
typedef struct {
  /** offset of function prototype in function_types */
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
  pwasm_slice_t name; ///< Export name
  pwasm_import_type_t type; ///< Export type
  uint32_t id; ///< Export index
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

/**
 * Module parsing callbacks.
 * @ingroup mod
 */
typedef struct {
  /**
   * Called when module parser encounters unsigned 32-bit integers.
   *
   * Callback should append u32s to internal u32s list and return a
   * slice indicating the offset and length in the internal u32s list.
   */
  pwasm_slice_t (*on_u32s)(const uint32_t *, const size_t, void *);

  /**
   * Called when module parser encounters arbitrary byte data.
   *
   * Callback should append bytes to internal byte buffer and return a
   * slice indicating the offset and length wthin the internal byte
   * buffer.
   */
  pwasm_slice_t (*on_bytes)(const uint8_t *, const size_t, void *);

  /**
   * Called when module parser encounters instructions.
   *
   * Callback should append the instructions to an internal instructions
   * vector and return a slice indicating the offset and length within
   * the internal buffer.
   */
  pwasm_slice_t (*on_insts)(const pwasm_inst_t *, const size_t, void *);

  /**
   * Called when module parser encounters a section header.
   */
  void (*on_section)(const pwasm_header_t *, void *);

  /**
   * Called when module parser encounters a custom section.
   *
   * @note The custom section name and custom section data are
   * represented as slices into the internal byte buffer.
   */
  void (*on_custom_section)(const pwasm_custom_section_t *, void *);

  /**
   * Called when module parser encounters types.
   *
   * @note The parameters and results are represented as slices into the
   * internal u32s buffer.
   */
  void (*on_types)(const pwasm_type_t *, const size_t, void *);

  /**
   * Called when module parser encounters imports.
   */
  void (*on_imports)(const pwasm_import_t *, const size_t, void *);

  /**
   * Called when module parser encounters functions.
   *
   * @note Functions are u32s representing an offset into the list of
   * types.
   */
  void (*on_funcs)(const uint32_t *, const size_t, void *);

  /**
   * Called when module parser encounters tables.
   */
  void (*on_tables)(const pwasm_table_t *, const size_t, void *);

  /**
   * Called when module parser encounters memory declarations.
   */
  void (*on_mems)(const pwasm_limits_t *, const size_t, void *);

  /**
   * Called when module parser encounters global variables.
   *
   * @note Global variable initialization expressions are represented as
   * slices of instructions within the `insts` buffer.
   */
  void (*on_globals)(const pwasm_global_t *, const size_t, void *);

  /**
   * Called when module parser encounters exports.
   */
  void (*on_exports)(const pwasm_export_t *, const size_t, void *);

  /**
   * Called when module parser encounters table elements.
   *
   * @note the `funcs` field is represented as a slice into the `u32s`
   * vector, and the `expr` field is represented as a slice into the
   * `insts` vector.
   */
  void (*on_elems)(const pwasm_elem_t *, const size_t, void *);

  /**
   * Called when module parser encounters a start function.
   */
  void (*on_start)(const uint32_t, void *);

  /**
   * Called when module parser encounters local variables.
   *
   * Callback should append locals to internal `locals` buffer and
   * return a slice indicating the offset and lenght within the `locals`
   * buffer.
   */
  pwasm_slice_t (*on_locals)(const pwasm_local_t *, const size_t, void *);

  /**
   * Called when module parser encounters label targets for the
   * `br_table` instruction.
   *
   * Callback should append labels to internal `labels` buffer and
   * return a slice indicating the offset and lenght within the `labels`
   * vector.
   */
  pwasm_slice_t (*on_labels)(const uint32_t *, const size_t, void *);

  /**
   * Called when module parser encounters function bodies.
   *
   * @note The locals in this function are represented as a slice into
   * the `locals` internal vector, and the expression is represented as
   * a slice into the internal `insts` vector.
   */
  void (*on_codes)(const pwasm_func_t *, const size_t, void *);

  /**
   * Called when module parser encounters a data segment.
   *
   * @note The bytes in this function  are represented as a slice into
   * the `bytes` internal vector, and the offset expression is
   * represented as a slice into the internal `insts` vector.
   */
  void (*on_segments)(const pwasm_segment_t *, const size_t, void *);

  // TODO (https://webassembly.github.io/spec/core/appendix/custom.html)
  // void (*on_module_name)(const pwasm_buf_t, void *);
  // void (*on_func_names)(const pwasm_func_name_t *, const size_t, void *);
  // void (*on_local_names)(const pwasm_local_name_t *, const size_t, void *);

  /**
   * Called when a parsing error is encountered.
   */
  void (*on_error)(const char *, void *);
} pwasm_mod_parse_cbs_t;

/**
 * Parse module in buffer.
 *
 * Parse the bytes in the given source buffer `src` with the given
 * module parser callbacks `cbs`.
 *
 * @ingroup mod
 *
 * @param src   Source buffer
 * @param cbs   Module parser callbacks
 * @param data  User callback data.
 *
 * @return The number of bytes consumed, or `0` on error.
 *
 * @note You shouldn't need to call this function directly; it is called
 * by `pwasm_mod_init()`.
 *
 */
size_t pwasm_mod_parse(
  const pwasm_buf_t src,
  const pwasm_mod_parse_cbs_t *cbs,
  void *data
);

/**
 * Parsed module.
 * @ingroup mod
 */
typedef struct {
  /** memory context */
  pwasm_mem_ctx_t * const mem_ctx;

  /**
   * single block of contiguous memory that serves as the backing store
   * for all of the items below.
   */
  pwasm_buf_t mem;

  /** array of u32s, referenced via slices from other sections below */
  const uint32_t * const u32s;
  const size_t num_u32s; /**< number of u32s */

  const uint32_t * const sections; ///< section IDs
  const size_t num_sections; ///< section ID count

  const pwasm_custom_section_t * const custom_sections; ///< custom sections
  const size_t num_custom_sections; ///< custom section count

  const pwasm_type_t * const types; ///< function types
  const size_t num_types; ///< function type count

  const pwasm_import_t * const imports; ///< imports
  const size_t num_imports; ///< import count

  /** Number of imports by import type */
  size_t num_import_types[PWASM_IMPORT_TYPE_LAST];

  /**
   * Maximum index by import type.
   *
   * In other words, the maximum number of imports of a given import
   * type plus the number of internal items of that same import type.
   */
  size_t max_indices[PWASM_IMPORT_TYPE_LAST];

  const pwasm_inst_t * const insts; ///< instructions
  const size_t num_insts; ///< instruction count

  const pwasm_global_t * const globals; ///< global variables
  const size_t num_globals; ///< global variable count

  const uint32_t * const funcs; ///< functions
  const size_t num_funcs; ///< function count

  const pwasm_table_t * const tables; ///< tables
  const size_t num_tables; ///< table count

  const pwasm_limits_t * const mems; ///< memories
  const size_t num_mems; ///< memory count

  const pwasm_export_t * const exports; ///< exports
  const size_t num_exports; ///< export count

  const pwasm_local_t * const locals; ///< locals
  const size_t num_locals; ///< local count

  const pwasm_func_t * const codes; ///< function bodies
  const size_t num_codes; ///< function body count

  const pwasm_elem_t * const elems; ///< table elements
  const size_t num_elems; ///< table element count

  const pwasm_segment_t * const segments; ///< data segments
  const size_t num_segments; ///< data segment count

  const uint8_t * const bytes; ///< bytes
  const size_t num_bytes; ///< byte count

  const _Bool has_start; ///< does this module have a start function?
  const uint32_t start; ///< start function index
} pwasm_mod_t;

/**
 * Parse a module from source `src` into the module `mod`.
 *
 * This function calls `pwasm_mod_init_unsafe()` to parse the given
 * source buffer `src` into a module `mod`, and then calls
 * `pwasm_mod_check()` to validate the parsed module.
 *
 * @ingroup mod
 *
 * @param[in]  mem_ctx  Memory context
 * @param[out] mod      Module
 * @param[in]  src      Source buffer
 *
 * @return Number of bytes consumed, or `0` on error.
 *
 * @see pwasm_mod_init_unsafe()
 * @see pwasm_mod_check()
 * @see pwasm_mod_fini()
 */
size_t pwasm_mod_init(
  pwasm_mem_ctx_t * const mem_ctx,
  pwasm_mod_t * const mod,
  pwasm_buf_t src
);

/**
 * Finalize a module and free any memory associated with it.
 *
 * @ingroup mod
 *
 * @param mod Module
 *
 * @see pwasm_mod_init()
 */
void pwasm_mod_fini(pwasm_mod_t *mod);

/**
 * Get the number of parameters for the given block type.
 *
 * @ingroup type
 *
 * @param[in]   mod         Module
 * @param[in]   block_type  Block type
 * @param[out]  ret_size    Number of parameters
 *
 * @return `true` on success or `false` on error.
 *
 * @see pwasm_block_type_params_get_nth()
 */
_Bool pwasm_block_type_params_get_size(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  size_t * const ret_size
);

/**
 * Get the type of the Nth parameter of the given block type.
 *
 * @ingroup type
 *
 * @param[in]   mod         Module
 * @param[in]   block_type  Block type
 * @param[in]   pos         Offset
 * @param[out]  ret_type    Return type
 *
 * @return `true` on success or `false` on error.
 *
 * @see pwasm_block_type_params_get_size()
 */
_Bool pwasm_block_type_params_get_nth(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  const size_t pos,
  uint32_t * const ret_type
);

/**
 * Get the number of results for the given block type.
 *
 * @ingroup type
 *
 * @param[in]   mod         Module
 * @param[in]   block_type  Block type
 * @param[out]  ret_size    Number of results
 *
 * @return `true` on success or `false` on error.
 *
 * @see pwasm_block_type_results_get_nth()
 */
_Bool pwasm_block_type_results_get_size(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  size_t * const ret_size
);

/**
 * Get the type of the Nth result of the given block type.
 *
 * @ingroup type
 *
 * @param[in]   mod         Module
 * @param[in]   block_type  Block type
 * @param[in]   pos         Offset
 * @param[out]  ret_type    Return type
 *
 * @return `true` on success or `false` on error.
 *
 * @see pwasm_block_type_results_get_size()
 */
_Bool pwasm_block_type_results_get_nth(
  const pwasm_mod_t * const mod,
  const int32_t block_type,
  const size_t pos,
  uint32_t * const ret_type
);

/**
 * Module builder state.
 * @ingroup mod
 * @see pwasm_builder_init()
 */
typedef struct {
  /** memory context */
  pwasm_mem_ctx_t * const mem_ctx;

  /** number of imports by import type */
  size_t num_import_types[PWASM_IMPORT_TYPE_LAST];

  /** number of exports by export type */
  size_t num_export_types[PWASM_IMPORT_TYPE_LAST];

  /**
   * Vector of bytes.
   *
   * Contains custom section names, custom section data, import module
   * names, import names, export names, and memory segment data.
   */
  pwasm_vec_t bytes;

  /**
   * Vector of u32s.
   *
   * Contains type parameters, type results, `br_table` labels, function
   * IDs, and table elements.
   */
  pwasm_vec_t u32s;

  /**
   * Section ID vector.
   * @note This should probably be section headers.
   */
  pwasm_vec_t sections;

  pwasm_vec_t custom_sections; ///< custom sections
  pwasm_vec_t types; ///< function types
  pwasm_vec_t imports; ///< imports

  /**
   * Instruction vector.
   *
   * Contains function bodies, element offset init exprs, global
   * init exprs, and data segment offset init exprs.
   */
  pwasm_vec_t insts;

  pwasm_vec_t tables; ///< Tables
  pwasm_vec_t mems; ///< Memories
  pwasm_vec_t funcs; ///< pwasm_func_ts

  pwasm_vec_t globals; ///< globals
  pwasm_vec_t exports; ///< exports
  pwasm_vec_t locals; ///< locals
  pwasm_vec_t codes; ///< function bodies
  pwasm_vec_t elems; ///< table elements
  pwasm_vec_t segments; ///< memory data segments

  _Bool has_start; ///< does this module have a start function?
  uint32_t start; ///< start function ID
} pwasm_builder_t;

/**
 * Create module builder.
 *
 * @ingroup mod
 *
 * @param[in]   mem_ctx Memory context
 * @param[out]  builder Builder
 *
 * @return `true` on success or `false` on error.
 *
 * @note The `pwasm_builder_*` functions are used internally by
 * `pwasm_mod_init_unsafe()`; you shouldn't need to call them directly.
 */
_Bool pwasm_builder_init(
  pwasm_mem_ctx_t *mem_ctx,
  pwasm_builder_t *builder
);

/**
 * Finalize a module builder.
 *
 * Finalize a module builder and free any memory associated with it.
 *
 * @ingroup mod
 *
 * @param builder Module builder
 *
 * @note The `pwasm_builder_*` functions are used internally by
 * `pwasm_mod_init_unsafe()`; you shouldn't need to call them directly.
 */
void pwasm_builder_fini(pwasm_builder_t *builder);

/**
 * Create module from module builder.
 *
 * Populate a `pwasm_mod_t` with the parsed module data contained in a
 * `pwasm_builder_t`.
 *
 * @ingroup mod
 *
 * @param[in] builder Module builder
 * @param[out] mod    Destination module
 *
 * @return `true` on success or `false` on error.
 *
 * @note The `pwasm_builder_*` functions are used internally by
 * `pwasm_mod_init_unsafe()`; you shouldn't need to call them directly.
 */
_Bool pwasm_build(
  const pwasm_builder_t *builder,
  pwasm_mod_t *mod
);

/**
 * Module validation callbacks.
 * @ingroup mod
 */
typedef struct {
  /**
   * Called by `pwasm_mod_check()` when a validation warning occurs.
   *
   * A validation warning is a non-fatal error.
   */
  void (*on_warning)(const char *, void *);

  /**
   * Called by `pwasm_mod_check()` when a validation error occurs.
   *
   * A validation error means that the module is invalid and cannot be
   * used.
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
  uint32_t i32; ///< 32-bit integer value (sign-agnostic)
  uint64_t i64; ///< 64-bit integer value (sign-agnostic)
  float    f32; ///< 32-bit floating-point value
  double   f64; ///< 64-bit floating-point value
  pwasm_v128_t v128; ///< 128-bit SIMD vector
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
  pwasm_val_t * const ptr; ///< pointer to backing value array
  const size_t len; ///< size of backing value array
  size_t pos; ///< current stack depth
} pwasm_stack_t;

/**
 * Memory instance.
 *
 * Memory instance inside an execution environment.
 *
 * @ingroup env
 */
typedef struct {
  /** Backing memory */
  pwasm_buf_t buf;

  /** Instance memory limits (minimum and maximum size, in pages) */
  pwasm_limits_t limits;
} pwasm_env_mem_t;

/**
 * Global variable instance.
 * @ingroup env
 */
typedef struct {
  /** global variable type (e.g., value type and mutability) */
  pwasm_global_type_t type;

  /** value of global variable */
  pwasm_val_t val;
} pwasm_env_global_t;

/**
 * Peek inside a value stack.
 * @ingroup util
 */
#define PWASM_PEEK(stack, ofs) ((stack)->ptr[(stack)->pos - 1 - (ofs)])

/** forward declaration */
typedef struct pwasm_env_t pwasm_env_t;

/** forward declaration */
typedef struct pwasm_native_t pwasm_native_t;

/**
 * @defgroup native Native Modules
 */

/**
 * Native instance.
 * @ingroup native
 * @deprecated Not used any more.  Will be removed.
 */
typedef struct {
  const uint32_t * const imports; ///< imports
  const pwasm_native_t * const native; ///< native
} pwasm_native_instance_t;

/**
 * Prototype for a native function callback.
 *
 * This is the prototype for a native module function.  A native module
 * function should accept an execution environment `env` and a native
 * module `mod` as parameters, and return true to indicate successful
 * execution, or `false to indicate an execution error.
 *
 * @ingroup native
 */
typedef _Bool (*pwasm_native_func_cb_t)(
  /** Execution environment */
  pwasm_env_t *env,

  /** Native module */
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
    /** Array of parameter value types */
    const pwasm_value_type_t *ptr;

    /** Number of parameter value types */
    const size_t len;
  } params;

  /** results */
  struct {
    /** Array of result value types */
    const pwasm_value_type_t *ptr;

    /** Number of result value types */
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
  const size_t num_imports; ///< Number of imports
  const pwasm_native_import_t * const imports; ///< Imports (unused)

  const size_t num_funcs; ///< Number of functions
  const pwasm_native_func_t * const funcs; ///< Functions

  const size_t num_mems; ///< Number of memories
  pwasm_native_mem_t * const mems; ///< Memories

  const size_t num_globals; ///< Number of globals
  const pwasm_native_global_t * const globals; ///< Globals

  const size_t num_tables; ///< Number of tables
  const pwasm_native_table_t * const tables; ///< Tables
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
    pwasm_env_t *env,
    const pwasm_buf_t name
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

  /**
   * Get table handle.
   *
   * Callback to get the handle of a table instance in an execution
   * environment.
   *
   * @param env     Execution environment
   * @param mod_id  Module handle
   * @param name    Table name buffer
   *
   * @return Table handle, or `0` on error.
   *
   * @note This callback implements `pwasm_env_find_table()`.
   */
  uint32_t (*find_table)(
    pwasm_env_t *env, // env
    const uint32_t mod_id, // module ID
    pwasm_buf_t name // table name
  );

  /**
   * Get value of table element.
   *
   * Callback to get the value of a table element in an execution
   * environment.
   *
   * @param[in]   env       Execution environment
   * @param[in]   table_id  Table handle
   * @param[in]   elem_ofs  Element offset
   * @param[out]  ret_val   Return value
   *
   * @return `true` on success or `false` on error.
   *
   * @note This callback implements `pwasm_env_get_elem()`.
   *
   * @note FIXME: Is this the table handle or table ID?
   */
  _Bool (*get_elem)(
    pwasm_env_t *env, // env
    const uint32_t table_id, // table ID (must be zero)
    const uint32_t elem_ofs, // element offset
    uint32_t *ret_val // return value
  );

  // find import handle by mod_id, import type, and import name
  // (returns zero on error)
  /**
   * Get import handle.
   *
   * Find an import handle in an execution environment `env` given a
   * module handle `mod_id`, an import type `type`, and an import name
   * `name`.
   *
   * @param env     Execution environment
   * @param mod_id  Module handle
   * @param type    Import type
   * @param name    Import name
   *
   * @return Import handle, or `0` on error.
   */
  uint32_t (*find_import)(
    pwasm_env_t *env, // env
    const uint32_t mod_id, // module handle,
    pwasm_import_type_t type, // import type
    const pwasm_buf_t name // import name
  );

  /**
   * Get function instance handle.
   *
   * Return a handle to a function instance within the execution
   * environment `env`, given a module handle `mod_id` and a function
   * name `name`.
   *
   * @param env     Execution environment
   * @param mod_id  Module handle
   * @param name    Function name
   *
   * @return Function handle, or `0` on error.
   */
  uint32_t (*find_func)(
    pwasm_env_t *env, // env
    const uint32_t mod_id, // module handle
    pwasm_buf_t name // function name
  );

  /**
   * Get memory instance handle.
   *
   * Return a handle to a memory instance within the execution
   * environment `env`, given a module handle `mod_id` and a memory name
   * `name`.
   *
   * @param env     Execution environment
   * @param mod_id  Module handle
   * @param name    Memory name
   *
   * @result Memory handle, or `0` on error.
   */
  uint32_t (*find_mem)(
    pwasm_env_t *env, // env
    const uint32_t mod_id, // module handle
    pwasm_buf_t name // memory name
  );

  /**
   * Get a pointer to a memory instance.
   *
   * Return a pointer to a memory instance with in an execution
   * environment `env` given a memory instance handle `mem_id`.
   *
   * @param env Execution environment
   * @param mem_id Execution environment
   *
   * @return Pointer to memory instance or `NULL` on error.
   */
  pwasm_env_mem_t *(*get_mem)(
    pwasm_env_t *, // env
    const uint32_t // memory handle
  );

  // _Bool (*call_func)(pwasm_env_t *, uint32_t);

  /**
   * Invoke function.
   *
   * Call a function `func_id` within the execution environment `env`.
   * The function parameters are stored on the stack.  Upon successful
   * exit, the results of the function call will be stored on the top of
   * the stack.
   *
   * @param env     Execution environment
   * @param func_id Function handle
   *
   * @return `true` if the function was successfully executed, or
   * `false` on error.
   */
  _Bool (*call)(
    pwasm_env_t *env, // env
    const uint32_t func_id // function ID
  );

  // load value from memory
  // (returns false on error)

  /**
   * Load value from memory.
   *
   * @param[in]   env     Execution environment
   * @param[in]   mem_id  Memory handle (FIXME, is this right?)
   * @param[in]   inst    Instruction (for memory immediate and value mask)
   * @param[in]   ofs     Offset operand
   * @param[out]  ret_val Return value
   *
   * @return `true` on success or `false` on error.
   */
  _Bool (*mem_load)(
    pwasm_env_t *env, // env
    const uint32_t mem_id, // mem_id
    const pwasm_inst_t inst, // instruction (memory immediate and value mask)
    const uint32_t ofs, // offset operand
    pwasm_val_t *ret_val // return value
  );

  /**
   * Store value to memory.
   *
   * @param env     Execution environment
   * @param mem_id  Memory handle (FIXME, is this right?)
   * @param inst    Instruction (for memory immediate and value mask)
   * @param ofs     Offset operand
   * @param val     Value
   *
   * @return `true` on success or `false` on error.
   */
  _Bool (*mem_store)(
    pwasm_env_t *env, // env
    const uint32_t mem_id, // mem_id
    const pwasm_inst_t inst, // instruction (memory immediate and value mask)
    const uint32_t ofs, // offset operand
    const pwasm_val_t val // value
  );

  /**
   * Get memory size.
   *
   * @param[in]  env      Execution environment
   * @param[in]  mem_id   Memory handle
   * @param[out] ret_size Return size, in pages
   *
   * @return `true` on success or `false` on error.
   */
  _Bool (*mem_size)(
    pwasm_env_t *env, // env
    const uint32_t mem_id, // mem_id
    uint32_t *ret_size // return value
  );

  /**
   * Grow memory.
   *
   * @param[in]   env       Execution environment
   * @param[in]   mem_id    Memory handle
   * @param[in]   grow      Amount to grow
   * @param[out]  ret_size  Return size
   *
   * @return `true` on success or `false` on error.
   */
  _Bool (*mem_grow)(
    pwasm_env_t *, // env
    const uint32_t, // mem_id
    const uint32_t, // amount to grow
    uint32_t * // return value
  );

  /**
   * Compile function.
   *
   * @param[in]   env       Execution environment
   * @param[in]   mod       Module
   * @param[in]   func_ofs  Function offset
   *
   * @return pointer to compiled function on success, or `NULL` on
   * error.
   */
  void *(*compile)(
    pwasm_env_t *, // env
    const pwasm_mod_t *, // mod
    const size_t func_ofs // function offset
  );
} pwasm_env_cbs_t;

/**
 * Execution environment.
 *
 * @ingroup env-low
 */
struct pwasm_env_t {
  pwasm_mem_ctx_t *mem_ctx;   ///< memory context
  const pwasm_env_cbs_t *cbs; ///< execution environment callbacks
  pwasm_stack_t *stack;       ///< stack pointer
  void *env_data;             ///< internal environment data
  void *user_data;            ///< user data
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
 * @param[out]  env     Pointer to destination execution environment.
 * @param[in]   mem_ctx Memory context.
 * @param[in]   cbs     Execution environment callbacks.
 * @param[in]   stack   Value stack.
 * @param[in]   data    User data.
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
  pwasm_env_t *env,
  const uint32_t mod_id,
  const pwasm_buf_t name
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
  pwasm_env_t *env,
  const uint32_t func_id
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
  pwasm_env_t *env,
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
  pwasm_val_t *ret_val
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
 * @param env Execution environment
 * @param mod Module name
 *
 * @return Module handle, or `0` on error.
 *
 * @note Convenience wrapper around `pwasm_env_find_mod()`.
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
 * @param name  Function name
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
  const char *name
);

/**
 * Get memory instance by module name and memory name.
 *
 * Find memory instance in given execution environment by module name
 * and memory name, and then return a pointer to the memory instance.
 *
 * @ingroup env
 *
 * @param env Execution environment
 * @param mod Module name
 * @param mem Memory name
 *
 * @return Pointer to the memory instance, or `NULL` on error.
 *
 * @note This is a convenience wrapper around pwasm_env_get_mem().
 *
 * @see pwasm_env_get_mem()
 */
pwasm_env_mem_t *pwasm_get_mem(
  pwasm_env_t *env,
  const char *mod,
  const char *mem
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
  pwasm_env_t *env,
  const char *mod,
  const char *name
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
  const char *name,
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
  pwasm_env_t * const env,
  const char * const mod,
  const char * const func
);

/**
 * @defgroup interp Interpreter Functions
 */

/**
 * Get callbacks for interpreter environment.
 *
 * @ingroup interp
 *
 * @return Pointer to execution environment callbacks.
 *
 * @see pwasm_env_init()
 */
const pwasm_env_cbs_t *pwasm_new_interpreter_get_cbs(void);

/**
 * @defgroup aot-jit AOT JIT Functions
 */

/**
 * JIT compiler function.
 * @ingroup type
 *
 * @param env       Execution environment.
 * @param mod       Module.
 * @param func_ofs  Function offset in module.
 *
 * @return Function pointer, or `NULL` on error.
 */
typedef void *(pwasm_compile_func_t)(
  pwasm_env_t *env,
  const pwasm_mod_t *mod,
  const size_t func_ofs
);

/*
 * Get AOT JIT environment callbacks.
 *
 * Populate environment variable callbacks for an ahead-of-time (AOT),
 * just in time (JIT) environment.
 *
 * @ingroup aot-jit
 *
 * @param[out]  cbs     Pointer to execution environment callbacks.
 * @param[in]   compile Pointer to compile function.
 *
 * @see pwasm_env_init()
 */
void
pwasm_aot_jit_get_cbs(
  pwasm_env_cbs_t * const cbs,
  pwasm_compile_func_t compile
);

#ifdef __cplusplus
};
#endif /* __cplusplus */

#endif /* PWASM_H */
