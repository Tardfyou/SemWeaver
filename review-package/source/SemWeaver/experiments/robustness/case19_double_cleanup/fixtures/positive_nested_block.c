#include "common.h"
void *nested(void *object) {
  if (register_or_reset(cleanup_primary, object)) {
    {
      cleanup_primary(object);
    }
    return 0;
  }
  return object;
}
