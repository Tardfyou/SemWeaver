#include "common.h"
u64 composed(u32 width, u32 value, u32 shift, u64 base) {
  return base + (((1U << width) | value) << shift);
}
