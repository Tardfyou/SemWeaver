#include "common.h"
u64 fixed_intermediate(u32 width, u32 value, u32 shift) {
  u64 wide = (1ULL << width) | value;
  return wide << shift;
}
