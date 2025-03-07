/* Copyright (c) 2023 Intel Corporation

Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "itex/core/compiler/xla/service/gpu/gpu_scatter_expander.h"

#include "absl/algorithm/container.h"
#include "itex/core/compiler/xla/service/hlo_computation.h"
#include "itex/core/compiler/xla/service/hlo_instruction.h"
#include "itex/core/compiler/xla/service/hlo_module.h"
#include "itex/core/compiler/xla/statusor.h"

namespace itex_xla {

bool GpuScatterExpander::InstructionMatchesPattern(HloInstruction* inst) {
  // TODO(b/129698548): Scattering elements larger than 64 bits is not
  // supported by XLA:GPU.
  // TODO(b/227486631): Variadic scatter is not yet supported by GPU.
  return inst->opcode() == HloOpcode::kScatter &&
         (inst->shape().IsTuple() ||
          primitive_util::BitWidth(inst->shape().element_type()) > 64);
}

}  // namespace itex_xla
