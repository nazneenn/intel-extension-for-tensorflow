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

#include "itex/core/compiler/xla/service/gpu/instruction_fusion.h"

#include "absl/container/flat_hash_set.h"
#include "itex/core/compiler/xla/service/fusion_node_indexing_evaluation.h"
#include "itex/core/compiler/xla/service/gpu/gpu_fusible.h"
#include "itex/core/compiler/xla/service/gpu/ir_emission_utils.h"
#include "itex/core/compiler/xla/service/hlo_opcode.h"
#include "itex/core/compiler/xla/service/hlo_query.h"
#include "itex/core/compiler/xla/service/llvm_ir/fused_ir_emitter.h"
#include "itex/core/compiler/xla/service/pattern_matcher.h"
#include "itex/core/compiler/xla/shape_util.h"
#include "protos/xla_data.pb.h"

namespace itex_xla {
namespace gpu {

namespace {
bool ElementIsF32OrF16(const Shape& shape) {
  PrimitiveType type = shape.element_type();
  return type == F32 || type == F16;
}
}  // namespace

/*static*/ bool GpuInstructionFusion::IsExpensive(
    const HloInstruction& instruction) {
  // We say that some floating-point math ops are cheap on the GPU. Unlike other
  // intrinsics that can be expanded into many instructions, Div and Rsqrt are
  // lowered into single hardware instructions.
  switch (instruction.opcode()) {
    case HloOpcode::kDivide:
    case HloOpcode::kRsqrt:
    case HloOpcode::kExp:
      if (ElementIsF32OrF16(instruction.shape())) {
        return false;
      }
      break;
    default:
      break;
  }
  return InstructionFusion::IsExpensive(instruction);
}

FusionDecision GpuInstructionFusion::ShouldFuseInexpensiveChecks(
    HloInstruction* consumer, int64_t operand_index) {
  HloInstruction* producer = consumer->mutable_operand(operand_index);

  // Output fusions are not currently supported on GPUs.
  if (producer->opcode() == HloOpcode::kFusion) {
    return "the producer is a fusion";
  }
  // Cost condition: not fuse (simple, expensive producers) and (consumers who
  // reuse operand elements).
  if (producer->opcode() != HloOpcode::kFusion && is_expensive(*producer) &&
      ReusesOperandElements(consumer, operand_index)) {
    return "the producer is expensive, and the consumer reuses inputs";
  }

  if (NoFusionPossible fusible =
          !IsProducerConsumerFusible(*producer, *consumer)) {
    return !fusible;
  }
  if (NoFusionPossible fusible =
          !InstructionFusion::ShouldFuse(consumer, operand_index)) {
    return !fusible;
  }
  return {};
}

FusionDecision GpuInstructionFusion::ShouldFuse(HloInstruction* consumer,
                                                int64_t operand_index) {
  if (NoFusionPossible fusible =
          !ShouldFuseInexpensiveChecks(consumer, operand_index)) {
    return !fusible;
  }

  auto producer = consumer->operand(operand_index);

  // The following checks are potentially expensive.
  if (NoFusionPossible too_large =
          !FusionFitsInBudget(*consumer, *producer,
                              /*is_consumer_producer_fusion=*/true)) {
    return !too_large;
  }

  if (consumer->opcode() != HloOpcode::kFusion) {
    return {};
  }

  // Also check that our emitter can handle the fusion node. We currently can
  // have exponential time/memory requirements for emitting certain fusion
  // kernels, in which case we don't want to fuse.
  // TODO(b/119692968): Remove this once we have fixed our fusion emitter.
  if (fusion_node_evaluations_.find(consumer) ==
      fusion_node_evaluations_.end()) {
    // We have no cached results for this fusion node yet. This can happen when
    // we run the InstructionFusion pass more than once. We can only cache the
    // results within one run.
    fusion_node_evaluations_.emplace(consumer,
                                     FusionNodeIndexingEvaluation(consumer));
  }
  if (fusion_node_evaluations_.at(consumer).CodeDuplicationTooHigh(producer)) {
    return "the fusion would result in an overly large code duplication";
  }
  return {};
}

HloInstruction::FusionKind GpuInstructionFusion::ChooseKind(
    const HloInstruction* producer, const HloInstruction* consumer) {
  return ChooseFusionKind(*producer, *consumer);
}

HloInstruction* GpuInstructionFusion::FuseInstruction(
    HloInstruction* fusion_instruction, HloInstruction* producer) {
  auto evaluation = fusion_node_evaluations_.find(fusion_instruction);
  if (evaluation == fusion_node_evaluations_.end()) {
    evaluation = fusion_node_evaluations_
                     .emplace(fusion_instruction,
                              FusionNodeIndexingEvaluation(fusion_instruction))
                     .first;
  }
  auto indexing_users = evaluation->second.RemoveFusionOperand(producer);
  HloInstruction* new_producer =
      InstructionFusion::FuseInstruction(fusion_instruction, producer);
  evaluation->second.UpdateEvaluationCache(new_producer, indexing_users);
  return new_producer;
}

}  // namespace gpu
}  // namespace itex_xla
