#include "common.h"
int parenthesized(struct trace3 *trace, unsigned i) {
  if (((trace->position[i])) > (3))
    return -1;
  consume_index(trace->position[i] + 1);
  return 0;
}
