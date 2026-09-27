#include "common.h"
void *setup_with_observation(void *dev, void *object) {
  if (devm_add_action_or_reset(dev, cleanup_primary, object)) {
    observe(object);
    cleanup_primary(object);
    return 0;
  }
  return object;
}
