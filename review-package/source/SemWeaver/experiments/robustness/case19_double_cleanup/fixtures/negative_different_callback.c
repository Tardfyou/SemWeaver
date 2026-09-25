#include "common.h"
void *different_callback(void *object) {
  if (register_or_reset(cleanup_primary, object)) {
    cleanup_alternate(object);
    return 0;
  }
  return object;
}
