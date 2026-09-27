#include "common.h"
void *renamed(void *resource) {
  if (register_or_reset(cleanup_alternate, resource)) {
    cleanup_alternate(resource);
    return 0;
  }
  return resource;
}
