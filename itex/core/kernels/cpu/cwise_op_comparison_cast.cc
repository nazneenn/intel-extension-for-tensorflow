/* Copyright (c) 2021-2022 Intel Corporation

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

#include "itex/core/kernels/common/cwise_ops_common.h"

namespace itex {

REGISTER2(BinaryOp, CPU, "_ITEXEqualWithCast", functor::equal_to_with_cast,
          float, Eigen::bfloat16);

REGISTER2(BinaryOp, CPU, "_ITEXGreaterEqualWithCast",
          functor::greater_equal_with_cast, float, Eigen::bfloat16);

REGISTER2(BinaryOp, CPU, "_ITEXGreaterWithCast", functor::greater_with_cast,
          float, Eigen::bfloat16);

REGISTER2(BinaryOp, CPU, "_ITEXLessEqualWithCast",
          functor::less_equal_with_cast, float, Eigen::bfloat16);

REGISTER2(BinaryOp, CPU, "_ITEXLessWithCast", functor::less_with_cast, float,
          Eigen::bfloat16);

REGISTER2(BinaryOp, CPU, "_ITEXNotEqualWithCast",
          functor::not_equal_to_with_cast, float, Eigen::bfloat16);

}  // namespace itex
