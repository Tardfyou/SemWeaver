#include "common.h"
void *no_cleanup(void *object) {
  if (register_or_reset(cleanup_primary, object))
    return 0;
  return object;
}
