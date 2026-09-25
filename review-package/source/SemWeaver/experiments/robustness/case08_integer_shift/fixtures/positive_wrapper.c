#include "common.h"
static u32 narrow_compute(u32 width, u32 value, u32 shift) {
  return ((1U << width) | value) << shift;
}
u64 wrapped(u32 width, u32 value, u32 shift) {
  u64 result = narrow_compute(width, value, shift);
  return result;
}
