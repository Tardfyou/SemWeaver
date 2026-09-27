#include "common.h"
static unsigned destination[5];
int larger_destination(struct trace3 *trace, unsigned i) {
  if (trace->position[i] > 3)
    return -1;
  destination[trace->position[i] + 1] = 1;
  return 0;
}
