#include "common.h"
void *saved_status(void *object) {
  int error = register_or_reset(cleanup_primary, object);
  if (error) {
    cleanup_primary(object);
    return 0;
  }
  return object;
}
