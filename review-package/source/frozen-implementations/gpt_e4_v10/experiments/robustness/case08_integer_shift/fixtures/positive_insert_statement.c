#include "common.h"
u64 inserted(u32 width, u32 value, u32 shift) {
  u32 unrelated = value ^ 7U;
  u64 result = ((1U << width) | value) << shift;
  return result + (unrelated & 0U);
}
