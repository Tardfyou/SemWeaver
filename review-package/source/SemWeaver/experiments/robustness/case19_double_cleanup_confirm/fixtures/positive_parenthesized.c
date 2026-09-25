#include "common.h"
void *parenthesized(void *object) {
  if ((register_or_reset(cleanup_primary, (void *)object))) {
    cleanup_primary((void *)object);
    return 0;
  }
  return object;
}
