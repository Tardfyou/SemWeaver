#include "common.h"
void *setup_renamed(void *device, void *resource) {
  if (devm_add_action_or_reset(device, cleanup_primary, resource)) {
    cleanup_primary(resource);
    return 0;
  }
  return resource;
}
