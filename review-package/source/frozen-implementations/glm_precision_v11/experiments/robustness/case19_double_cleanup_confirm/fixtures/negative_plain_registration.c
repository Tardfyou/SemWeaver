#include "common.h"
void *plain_registration(void *object) {
  if (register_plain(cleanup_primary, object)) {
    cleanup_primary(object);
    return 0;
  }
  return object;
}
