#include "common.h"
u64 renamed(u32 bits, u32 mantissa, u32 exponent) {
  u64 interval = ((1U << bits) | mantissa) << exponent;
  return interval;
}
