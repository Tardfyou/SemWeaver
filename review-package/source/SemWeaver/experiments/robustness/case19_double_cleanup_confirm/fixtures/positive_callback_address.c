#include "common.h"
void *callback_address(void *object) {
  if (register_or_reset(&cleanup_primary, object)) {
    cleanup_primary(object);
    return 0;
  }
  return object;
}
