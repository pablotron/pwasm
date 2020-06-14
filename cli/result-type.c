#include <stdbool.h>
#include "result-type.h"

bool result_type_is_valid(
  const result_type_t type
) {
  return (
#define RESULT_TYPE(a, b) (type == RESULT_TYPE_ ## a) ||
RESULT_TYPES
#undef RESULT_TYPE
    false // sentinel
  );
}

const char *
result_type_get_name(
  const result_type_t type
) {
  switch (type) {
#define RESULT_TYPE(a, b) case RESULT_TYPE_ ## a: return b;
RESULT_TYPES
#undef RESULT_TYPE
  default: return "unknown";
  }
}
