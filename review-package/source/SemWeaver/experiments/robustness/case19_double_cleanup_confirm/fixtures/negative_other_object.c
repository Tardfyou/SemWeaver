#include "common.h"
void *other_object(void *object, void *other) {
  if (register_or_reset(cleanup_primary, object)) {
    cleanup_primary(other);
    return 0;
  }
  return object;
}
