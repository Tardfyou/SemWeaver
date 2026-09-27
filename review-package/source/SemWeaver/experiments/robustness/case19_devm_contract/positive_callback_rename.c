#include "common.h"
void *setup_callback_renamed(void *device, void *resource) {
  if (devm_add_action_or_reset(device, cleanup_renamed, resource)) {
    cleanup_renamed(resource);
    return 0;
  }
  return resource;
}
