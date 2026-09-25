#include "common.h"
static int arm_cleanup(cleanup_fn action, void *object) {
  return register_or_reset(action, object);
}
void *wrapped(void *object) {
  if (arm_cleanup(cleanup_primary, object)) {
    cleanup_primary(object);
    return 0;
  }
  return object;
}
