/* Copyright (c) 2023 Intel Corporation

Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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

#ifndef ITEX_CORE_COMPILER_XLA_SERVICE_GPU_GPU_LAYOUT_ASSIGNMENT_H_
#define ITEX_CORE_COMPILER_XLA_SERVICE_GPU_GPU_LAYOUT_ASSIGNMENT_H_

#include "itex/core/compiler/xla/service/computation_layout.h"
#include "itex/core/compiler/xla/service/hlo_instructions.h"
#include "itex/core/compiler/xla/service/layout_assignment.h"
#include "itex/core/utils/status.h"

namespace itex_xla {
namespace gpu {

// GPU-specific layout assignment pass which preassigns layouts to satisfy
// layout constraints for operands and results of library calls.
class GpuLayoutAssignment : public LayoutAssignment {
 public:
  explicit GpuLayoutAssignment(
      ComputationLayout* entry_computation_layout,
      ChannelLayoutConstraints* channel_constraints = nullptr)
      : LayoutAssignment(entry_computation_layout, channel_constraints) {}
  ~GpuLayoutAssignment() override {}

 protected:
  Status AddBackendConstraints(LayoutConstraints* constraints) override;

 private:
  Status AddBackendConstraintsToDnnConvCustomCall(
      HloCustomCallInstruction* instr, LayoutConstraints* constraints);
};

}  // namespace gpu
}  // namespace itex_xla

#endif  // ITEX_CORE_COMPILER_XLA_SERVICE_GPU_GPU_LAYOUT_ASSIGNMENT_H_
