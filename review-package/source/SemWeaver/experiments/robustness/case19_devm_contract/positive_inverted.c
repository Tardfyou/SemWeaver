#include "common.h"
void *setup_inverted(void *dev, void *object) {
  if (!devm_add_action_or_reset(dev, cleanup_primary, object)) {
    return object;
  } else {
    cleanup_primary(object);
    return 0;
  }
}
