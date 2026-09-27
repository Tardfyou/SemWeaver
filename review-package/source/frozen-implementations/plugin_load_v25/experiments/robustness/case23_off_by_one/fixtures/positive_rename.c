#include "common.h"
int renamed(struct trace3 *sample, unsigned slot) {
  if (sample->position[slot] > 3)
    return -1;
  consume_index(sample->position[slot] + 1);
  return 0;
}
