#include "common.h"
void *setup_other_object(void *dev, void *object, void *other) {
  if (devm_add_action_or_reset(dev, cleanup_primary, object)) {
    cleanup_primary(other);
    return 0;
  }
  return object;
}
