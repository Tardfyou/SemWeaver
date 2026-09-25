#include "common.h"
void *inserted(void *object) {
  if (register_or_reset(cleanup_primary, object)) {
    observe(object);
    cleanup_primary(object);
    return 0;
  }
  return object;
}
