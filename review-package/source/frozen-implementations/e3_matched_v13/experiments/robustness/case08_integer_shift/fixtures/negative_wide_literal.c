#include "common.h"
u64 fixed_literal(u32 width, u32 value, u32 shift) {
  return ((1ULL << width) | (u64)value) << shift;
}
