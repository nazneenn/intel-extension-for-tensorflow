/* Copyright (c) 2023 Intel Corporation

Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include "itex/core/compiler/xla/service/hlo_domain_isolator.h"

#include <cstdint>
#include <utility>

#include "itex/core/compiler/xla/map_util.h"
#include "itex/core/compiler/xla/service/hlo_computation.h"
#include "itex/core/compiler/xla/service/hlo_domain_remover.h"
#include "itex/core/compiler/xla/service/hlo_graph_dumper.h"
#include "itex/core/compiler/xla/service/hlo_opcode.h"
#include "itex/core/compiler/xla/service/hlo_sharding_metadata.h"
#include "itex/core/compiler/xla/types.h"
#include "itex/core/utils/statusor.h"

namespace itex_xla {

namespace {

// Add domains which are used as users of a specific instruction.
StatusOr<int64_t> AddExitDomains(HloInstruction* instruction,
                                 HloDomainIsolator::DomainCreator* creator) {
  int64_t added_domains = 0;
  if (instruction->opcode() == HloOpcode::kDomain) {
    return added_domains;
  }
  // Make a const copy of instruction's users to loop through later, as the
  // users vector could be changed during the loop
  // (e.g. ReplaceUseWithDifferentShape).
  const std::vector<HloInstruction*> users(instruction->users());
  for (HloInstruction* user : users) {
    // Check whether a kDomain is necessary between user and instruction.
    HloInstruction* domain = (*creator)(user, instruction, instruction);
    if (domain != nullptr) {
      ITEX_VLOG(4) << "New domain: " << domain->ToString();
      // Call ReplaceUseWithDifferentShape even though the shapes are
      // expected to match to avoid an expensive shape check between the
      // original and the new instruction.
      TF_RETURN_IF_ERROR(
          instruction->ReplaceUseWithDifferentShape(user, domain));
      ++added_domains;
    }
  }
  return added_domains;
}

StatusOr<bool> RunInternal(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads,
    HloDomainIsolator::DomainCreator* creator) {
  int64_t added_domains = 0;
  for (HloComputation* computation : module->computations(execution_threads)) {
    // Walk in post order and place all the required kDomain instructions.
    for (HloInstruction* instruction :
         computation->MakeInstructionPostOrder()) {
      if (instruction->opcode() == HloOpcode::kDomain) {
        continue;
      }
      for (HloInstruction* operand : instruction->unique_operands()) {
        // When applying multiple domains, we could end up stacking more than
        // one in one edge, so here we want to build the effective
        // (kDomain-less) instruction->operand edge.
        HloInstruction* root = operand;
        while (root->opcode() == HloOpcode::kDomain) {
          root = root->mutable_operand(0);
        }
        // Check whether a kDomain is necessary between instruction and operand.
        HloInstruction* domain = (*creator)(instruction, root, operand);
        if (domain != nullptr) {
          ITEX_VLOG(4) << "New domain: " << domain->ToString();
          // Call ReplaceUseWithDifferentShape even though the shapes are
          // expected to match to avoid an expensive shape check between the
          // original and the new instruction.
          TF_RETURN_IF_ERROR(
              operand->ReplaceUseWithDifferentShape(instruction, domain));
          ++added_domains;
        }
      }
    }
  }
  ITEX_VLOG(3) << "Added " << added_domains << " kDomain instructions";
  return added_domains > 0;
}

}  // namespace

HloDomainIsolator::HloDomainIsolator(DomainCreatorFactory creator_factory)
    : creator_factory_(std::move(creator_factory)) {}

StatusOr<bool> HloDomainIsolator::UpdateDomains(HloInstruction* instruction) {
  TF_ASSIGN_OR_RETURN(const int64_t removed_domains,
                      HloDomainRemover::RemoveExitDomains(
                          instruction, ShardingMetadata::KindName()));
  DomainCreator creator = creator_factory_();
  TF_ASSIGN_OR_RETURN(const int64_t added_domains,
                      AddExitDomains(instruction, &creator));
  return removed_domains > 0 || added_domains > 0;
}

StatusOr<bool> HloDomainIsolator::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  DomainCreator creator = creator_factory_();
  return RunInternal(module, execution_threads, &creator);
}

}  // namespace itex_xla
