#include "common.h"
int commuted(struct trace3 *trace, unsigned i) {
  if (3 < trace->position[i])
    return -1;
  consume_index(trace->position[i] + 1);
  return 0;
}
