#include "common.h"
int validate(struct trace3 *trace, unsigned i) {
  if (trace->position[i] > TRACE_LIMIT)
    return -1;
  consume_index(trace->position[i] + 1);
  return 0;
}
