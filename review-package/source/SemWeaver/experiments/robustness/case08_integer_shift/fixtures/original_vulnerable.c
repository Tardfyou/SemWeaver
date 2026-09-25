#include "common.h"
u64 compute(u32 width, u32 value, u32 shift) {
  u64 result = ((1U << width) | value) << shift;
  consume_u64(result);
  return result;
}
