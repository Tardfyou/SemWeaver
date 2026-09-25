#include "common.h"
void setup(void *device, void *object) {
  if (__devm_add_action_or_reset(device, renamed_cleanup, object)) {
    renamed_cleanup(object);
    return;
  }
}
