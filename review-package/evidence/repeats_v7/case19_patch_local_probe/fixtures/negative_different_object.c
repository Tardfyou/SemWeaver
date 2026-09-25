#include "common.h"
void setup(void *device, void *object, void *other) {
  if (__devm_add_action_or_reset(device, scmi_debugfs_common_cleanup, object)) {
    scmi_debugfs_common_cleanup(other);
    return;
  }
}
