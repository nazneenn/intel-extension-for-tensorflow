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

#ifndef ITEX_CORE_COMPILER_XLA_SERVICE_GPU_ELEMENTAL_IR_EMITTER_H_
#define ITEX_CORE_COMPILER_XLA_SERVICE_GPU_ELEMENTAL_IR_EMITTER_H_

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "absl/types/span.h"
#include "itex/core/compiler/xla/service/elemental_ir_emitter.h"
#include "itex/core/compiler/xla/service/gpu/target_util.h"
#include "itex/core/compiler/xla/service/hlo_computation.h"
#include "itex/core/compiler/xla/service/hlo_instruction.h"
#include "itex/core/compiler/xla/service/hlo_module_config.h"
#include "itex/core/compiler/xla/service/llvm_ir/loop_emitter.h"
#include "itex/core/compiler/xla/statusor.h"
#include "itex/core/compiler/xla/types.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Value.h"
#include "protos/xla_data.pb.h"

namespace itex_xla {
namespace gpu {

class GpuElementalIrEmitter : public ElementalIrEmitter {
 public:
  // A NestedComputer computes an element of the output of the given computation
  // given a Span of its input elements.
  using NestedComputer = std::function<StatusOr<std::vector<llvm::Value*>>(
      const HloComputation&, absl::Span<llvm::Value* const>)>;

  GpuElementalIrEmitter(const HloModuleConfig& hlo_module_config,
                        llvm::Module* module, llvm::IRBuilder<>* b,
                        NestedComputer compute_nested);

 protected:
  llvm_ir::IrArray::Index GetSourceIndexOfBitcast(
      const llvm_ir::IrArray::Index& index, const HloInstruction* hlo) override;

  StatusOr<llvm::Value*> EmitFloatBinaryOp(const HloInstruction* op,
                                           llvm::Value* lhs_value,
                                           llvm::Value* rhs_value) override;

  StatusOr<llvm::Value*> EmitLog(PrimitiveType prim_type,
                                 llvm::Value* value) override;

  StatusOr<llvm::Value*> EmitLog1p(PrimitiveType prim_type,
                                   llvm::Value* value) override;

  StatusOr<llvm::Value*> EmitSin(PrimitiveType prim_type,
                                 llvm::Value* value) override;

  StatusOr<llvm::Value*> EmitCos(PrimitiveType prim_type,
                                 llvm::Value* value) override;

  StatusOr<llvm::Value*> EmitExp(PrimitiveType prim_type, llvm::Value* value,
                                 absl::string_view name) override;

  StatusOr<llvm::Value*> EmitExpm1(PrimitiveType prim_type,
                                   llvm::Value* value) override;

  StatusOr<llvm::Value*> EmitSqrt(PrimitiveType prim_type,
                                  llvm::Value* value) override;

  StatusOr<llvm::Value*> EmitRsqrt(PrimitiveType prim_type,
                                   llvm::Value* value) override;

  StatusOr<llvm::Value*> EmitPow(PrimitiveType prim_type, llvm::Value* lhs,
                                 llvm::Value* rhs,
                                 absl::string_view name) override;

  StatusOr<llvm::Value*> EmitAtan2(PrimitiveType prim_type, llvm::Value* lhs,
                                   llvm::Value* rhs,
                                   absl::string_view name) override;

  StatusOr<llvm::Value*> EmitTanh(PrimitiveType prim_type,
                                  llvm::Value* value) override;

  // StatusOr<llvm::Value*> EmitComplexAbs(PrimitiveType prim_type,
  //                                       llvm::Value* value) override;

  StatusOr<std::vector<llvm::Value*>> EmitThreadLocalCall(
      const HloComputation& callee, absl::Span<llvm::Value* const> parameters,
      absl::string_view, bool /*is_reducer*/) override {
    return compute_nested_(callee, parameters);
  }

  llvm::Value* EmitThreadId() override;

  bool fast_min_max() override {
    return hlo_module_config_.debug_options().xla_gpu_enable_fast_min_max();
  }

 private:
  // Emits IR for op, which must have opcode kPower.
  StatusOr<llvm::Value*> EmitPowerOp(const HloInstruction* op,
                                     llvm::Value* lhs_value,
                                     llvm::Value* rhs_value);

  // Emits IR to call an LLVM intrinsic of type [T] -> T.  Adjusts
  // callee_name according to T.  Returns the IR value that represents the
  // return value of the function.
  StatusOr<llvm::Value*> EmitLlvmIntrinsicMathCall(
      const std::string& callee_name, absl::Span<llvm::Value* const> operands,
      absl::Span<const PrimitiveType> input_types, PrimitiveType output_type);

  // Emits IR to call a device function of type [T] -> T.  Adjusts
  // callee_name according to T.  Returns the IR value that represents the
  // return value of the function.
  StatusOr<llvm::Value*> EmitDeviceMathCall(
      TargetDeviceFunctionID funcid, absl::Span<llvm::Value* const> operands,
      absl::Span<const PrimitiveType> input_types, PrimitiveType output_type,
      absl::string_view name = "");

  // Emits IR to call a function of type [T] -> T.  Does not munge callee_name.
  // Returns the IR value that represents the return value of the function.
  StatusOr<llvm::Value*> EmitMathCall(
      const std::string& callee_name, absl::Span<llvm::Value* const> operands,
      absl::Span<const PrimitiveType> input_types, PrimitiveType output_type,
      absl::string_view name = "");

  const HloModuleConfig& hlo_module_config_;

  NestedComputer compute_nested_;
};

}  // namespace gpu
}  // namespace itex_xla

#endif  // ITEX_CORE_COMPILER_XLA_SERVICE_GPU_ELEMENTAL_IR_EMITTER_H_
