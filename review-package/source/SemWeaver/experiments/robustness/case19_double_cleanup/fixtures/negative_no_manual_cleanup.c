#include "common.h"
void *automatic_only(void *object) {
  if (register_or_reset(cleanup_primary, object))
    return 0;
  return object;
}
