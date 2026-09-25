#include "common.h"
void *goto_cleanup(void *object) {
  if (register_or_reset(cleanup_primary, object))
    goto failed;
  return object;
failed:
  cleanup_primary(object);
  return 0;
}
