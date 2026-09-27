#include "common.h"
int composed(struct trace3 *trace, unsigned i, int disabled) {
  if (trace->position[i] > 3 || disabled)
    return -1;
  consume_index(trace->position[i] + 1);
  return 0;
}
