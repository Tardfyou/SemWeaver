#include "common.h"
void *setup_no_extra_cleanup(void *dev, void *object) {
  if (devm_add_action_or_reset(dev, cleanup_primary, object))
    return 0;
  return object;
}
