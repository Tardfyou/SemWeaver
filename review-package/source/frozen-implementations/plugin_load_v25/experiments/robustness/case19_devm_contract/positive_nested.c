#include "common.h"
void *setup_nested(void *dev, void *object) {
  if (devm_add_action_or_reset(dev, cleanup_primary, object)) {
    {
      cleanup_primary(object);
    }
    return 0;
  }
  return object;
}
