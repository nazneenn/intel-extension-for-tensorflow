/* Copyright (c) 2023 Intel Corporation

Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef ITEX_CORE_COMPILER_XLA_OVERFLOW_UTIL_H_
#define ITEX_CORE_COMPILER_XLA_OVERFLOW_UTIL_H_

#include <limits>
#include <type_traits>

#include "absl/types/optional.h"
#include "itex/core/utils/logging.h"
#include "protos/xla.pb.h"

namespace itex_xla {

// Multiply two nonnegative int64_t's, returning negative for overflow
inline int64_t MultiplyWithoutOverflow(const int64_t x, const int64_t y) {
  // Multiply in uint64_t rather than int64_t since signed overflow is
  // undefined. Negative values will wrap around to large unsigned values in the
  // casts (see section 4.7 [conv.integral] of the C++14 standard).
  const uint64_t ux = x;
  const uint64_t uy = y;
  const uint64_t uxy = ux * uy;

  // Check if we overflow uint64_t, using a cheap check if both inputs are small
  if (ABSL_PREDICT_FALSE((ux | uy) >> 32 != 0)) {
    // Ensure nonnegativity.  Note that negative numbers will appear "large"
    // to the unsigned comparisons above.
    ITEX_CHECK(x >= 0 && y >= 0);

    // Otherwise, detect overflow using a division
    if (ux != 0 && uxy / ux != uy) return -1;
  }

  // Cast back to signed.  Any negative value will signal an error.
  return static_cast<int64_t>(uxy);
}

// Computes x + y and returns nullopt if it overflows.
//
// x and y must be signed integers.
template <typename T>
inline absl::optional<T> OverflowSafeAdd(T x, T y) {
  static_assert(std::is_signed<T>::value,
                "Only implemented for signed numbers T.");
  static_assert(std::is_integral<T>::value, "Only implemented for integers T.");
  // "Signed integer overflow occurs on integer addition iff the operands have
  // the same sign and the sum has a sign opposite to that of the operands."
  // Hacker's Delight 2nd ed, p 28.
  using U = typename std::make_unsigned<T>::type;
  const U ux = x;
  const U uy = y;
  const U usum = ux + uy;
  const T sum = usum;
  if (x >= 0 == y >= 0 && sum >= 0 != x >= 0) {
    return absl::nullopt;
  }
  return sum;
}

inline bool FitsInIntegralType(int64_t x, PrimitiveType ty) {
  switch (ty) {
    case S8:
      return std::numeric_limits<int8_t>::min() <= x &&
             std::numeric_limits<int8_t>::max() >= x;
    case S16:
      return std::numeric_limits<int16_t>::min() <= x &&
             std::numeric_limits<int16_t>::max() >= x;
    case S32:
      return std::numeric_limits<int32_t>::min() <= x &&
             std::numeric_limits<int32_t>::max() >= x;
    case S64:
      return true;
    case U8:
      return 0 <= x && std::numeric_limits<uint8_t>::max() >= x;
    case U16:
      return 0 <= x && std::numeric_limits<uint16_t>::max() >= x;
    case U32:
      return 0 <= x && std::numeric_limits<uint32_t>::max() >= x;
    case U64:
      return 0 <= x;
    default:
      ITEX_LOG(FATAL) << "Invalid primitive type " << PrimitiveType_Name(ty);
  }
}

}  // namespace itex_xla

#endif  // ITEX_CORE_COMPILER_XLA_OVERFLOW_UTIL_H_
