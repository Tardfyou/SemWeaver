#include "common.h"
void *inverted(void *object) {
  if (!register_or_reset(cleanup_primary, object)) {
    return object;
  } else {
    cleanup_primary(object);
    return 0;
  }
}
