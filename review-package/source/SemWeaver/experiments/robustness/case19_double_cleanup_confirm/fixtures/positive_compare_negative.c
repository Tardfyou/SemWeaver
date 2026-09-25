#include "common.h"
void *compare_negative(void *object) {
  if (register_or_reset(cleanup_primary, object) < 0) {
    cleanup_primary(object);
    return 0;
  }
  return object;
}
