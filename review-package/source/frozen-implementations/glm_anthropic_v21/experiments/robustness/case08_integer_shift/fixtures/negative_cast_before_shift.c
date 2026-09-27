#include "common.h"
u64 fixed_cast(u32 width, u32 value, u32 shift) {
  return (u64)((1U << width) | value) << shift;
}
