/* Copyright (c) 2023 Intel Corporation

Copyright 2020 The TensorFlow Authors. All Rights Reserved.

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

#include "itex/core/compiler/xla/service/logistic_expander.h"

#include "absl/types/optional.h"
#include "absl/types/span.h"
#include "itex/core/compiler/xla/literal.h"
#include "itex/core/compiler/xla/literal_util.h"
#include "itex/core/compiler/xla/service/dfs_hlo_visitor_with_default.h"
#include "itex/core/compiler/xla/service/hlo_computation.h"
#include "itex/core/compiler/xla/service/hlo_creation_utils.h"
#include "itex/core/compiler/xla/service/hlo_instruction.h"
#include "itex/core/compiler/xla/service/hlo_opcode.h"
#include "itex/core/compiler/xla/shape_util.h"
#include "itex/core/compiler/xla/status_macros.h"
#include "itex/core/compiler/xla/types.h"
#include "itex/core/compiler/xla/util.h"
#include "itex/core/utils/errors.h"
#include "itex/core/utils/logging.h"
#include "itex/core/utils/status.h"
#include "protos/xla_data.pb.h"

namespace itex_xla {

namespace {

HloInstruction* ExpandLogisticWithTanh(HloInstruction* logistic) {
  HloInstruction* operand = logistic->mutable_operand(0);
  const Shape operand_shape = operand->shape();
  HloInstruction* half_constant = MakeScalarLike(operand, 0.5f);
  HloInstruction* tanh_instr =
      MakeUnaryHlo(HloOpcode::kTanh,
                   MakeBinaryHlo(HloOpcode::kMultiply, half_constant, operand)
                       .ValueOrDie())
          .ValueOrDie();
  return MakeBinaryHlo(
             HloOpcode::kAdd, half_constant,
             MakeBinaryHlo(HloOpcode::kMultiply, half_constant, tanh_instr)
                 .ValueOrDie())
      .ValueOrDie();
}

HloInstruction* ExpandLogisticWithExp(HloInstruction* logistic) {
  HloInstruction* operand = logistic->mutable_operand(0);
  const Shape operand_shape = operand->shape();
  // Computing 1.0 / (1.0 - exp(-x))
  HloInstruction* one_constant = MakeScalarLike(operand, 1.0f);
  HloInstruction* exp_instr =
      MakeUnaryHlo(HloOpcode::kExp,
                   MakeUnaryHlo(HloOpcode::kNegate, operand).ValueOrDie())
          .ValueOrDie();
  HloInstruction* denominator =
      MakeBinaryHlo(HloOpcode::kAdd, one_constant, exp_instr).ValueOrDie();
  return MakeBinaryHlo(HloOpcode::kDivide, one_constant, denominator)
      .ValueOrDie();
}

}  // namespace

bool LogisticExpander::InstructionMatchesPattern(HloInstruction* instruction) {
  return instruction->opcode() == HloOpcode::kLogistic;
}

StatusOr<HloInstruction*> LogisticExpander::ExpandInstruction(
    HloInstruction* instruction) {
  switch (expansion_type_) {
    case LogisticExpansionType::kTanh:
      return ExpandLogisticWithTanh(instruction);
    case LogisticExpansionType::kExp:
      return ExpandLogisticWithExp(instruction);
  }
}

}  // namespace itex_xla
