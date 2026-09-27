#include "common.h"
void *setup_other_callback(void *dev, void *object) {
  if (devm_add_action_or_reset(dev, cleanup_primary, object)) {
    cleanup_other(object);
    return 0;
  }
  return object;
}
