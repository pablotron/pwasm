#ifndef CLI_RESULT_TYPE_H
#define CLI_RESULT_TYPE_H

// Note: result types have been replaced by block types in the WASM
// spec, but we are still using result types to specify our test
// results, so keep these around for now.

#define RESULT_TYPES \
  RESULT_TYPE(I32, "i32") \
  RESULT_TYPE(I64, "i64") \
  RESULT_TYPE(F32, "f32") \
  RESULT_TYPE(F64, "f64") \
  RESULT_TYPE(VOID, "void")

typedef enum {
#define RESULT_TYPE(a, b) RESULT_TYPE_ ## a,
RESULT_TYPES
#undef RESULT_TYPE
  RESULT_TYPE_LAST, // sentinel
} result_type_t;

/**
 * Is this value a valid block result type?
 *
 * From section 5.3.2 of the WebAssembly documentation.
 */
_Bool result_type_is_valid(const result_type_t type);

/**
 * Get name of result type.
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
const char *result_type_get_name(const result_type_t type);

#endif /* CLI_RESULT_TYPE_H */
