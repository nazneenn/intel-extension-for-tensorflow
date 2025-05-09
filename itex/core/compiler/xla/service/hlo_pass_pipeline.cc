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

#include "itex/core/compiler/xla/service/hlo_pass_pipeline.h"

#include <functional>
#include <string>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "itex/core/compiler/xla/service/dump.h"
#include "itex/core/compiler/xla/service/hlo_graph_dumper.h"
#include "itex/core/compiler/xla/service/hlo_proto_util.h"
#include "itex/core/compiler/xla/status_macros.h"
#include "itex/core/compiler/xla/types.h"
#include "itex/core/compiler/xla/util.h"
#include "itex/core/utils/errors.h"
#include "itex/core/utils/logging.h"
#include "itex/core/utils/status.h"

namespace itex_xla {

namespace {

void RecordPassStartMetadata(HloModule& module, const std::string& pass_name,
                             const std::string& pipeline_name) {
  module.metadata()->RecordPassStart();
  // An HloPassMetadata was just created so Status should always be OK.
  ITEX_CHECK_OK(module.metadata()->set_current_pass_name(pass_name));
  ITEX_CHECK_OK(
      module.metadata()->set_current_pass_pipeline_name(pipeline_name));
}

void RecordPassStartMetadata(HloModuleGroup& module_group,
                             const std::string& pass_name,
                             const std::string& pipeline_name) {
  for (HloModule* module : module_group.modules()) {
    RecordPassStartMetadata(*module, pass_name, pipeline_name);
  }
}

Status AttemptRecordPassEndMetadata(HloModule& module,
                                    const std::string& pass_name,
                                    bool module_changed) {
  // Module id is set here instead of RecordPassStartMetadata because it may
  // change in the middle of the pass, and we want the final id.
  TF_RETURN_IF_ERROR(
      module.metadata()->set_current_pass_module_id(module.unique_id()));
  TF_RETURN_IF_ERROR(
      module.metadata()->set_current_pass_module_changed(module_changed));
  TF_RETURN_IF_ERROR(module.metadata()->RecordPassEnd());
  return Status::OK();
}

void RecordPassEndMetadata(HloModule& module, const std::string& pass_name,
                           bool module_changed) {
  Status status =
      AttemptRecordPassEndMetadata(module, pass_name, module_changed);
  if (!status.ok()) {
    ITEX_LOG(FATAL) << status;
  }
}

Status AttemptRecordPassEndMetadata(HloModuleGroup& module_group,
                                    const std::string& pass_name,
                                    bool module_changed) {
  for (HloModule* module : module_group.modules()) {
    for (HloModule* other_module : module_group.modules()) {
      TF_RETURN_IF_ERROR(
          module->metadata()->add_current_pass_module_group_module_id(
              other_module->unique_id()));
    }
    TF_RETURN_IF_ERROR(
        AttemptRecordPassEndMetadata(*module, pass_name, module_changed));
  }
  return Status::OK();
}

void RecordPassEndMetadata(HloModuleGroup& module_group,
                           const std::string& pass_name, bool module_changed) {
  Status status =
      AttemptRecordPassEndMetadata(module_group, pass_name, module_changed);
  if (!status.ok()) {
    ITEX_LOG(FATAL) << status;
  }
}

void SetInstructionMetadata(HloModule& module) {
  StatusOr<int64_t> pass_id = module.metadata()->current_pass_id();
  if (!pass_id.ok()) {
    ITEX_LOG(FATAL) << pass_id.status();
  }
  for (itex_xla::HloComputation* computation : module.computations()) {
    for (itex_xla::HloInstruction* instruction : computation->instructions()) {
      if (instruction->metadata().creation_pass_id() == 0) {
        instruction->set_creation_pass_id(*pass_id);
      }
      if (instruction->metadata().logical_creation_pass_id() == 0) {
        instruction->set_logical_creation_pass_id(*pass_id);
      }
    }
  }
}

void SetInstructionMetadata(HloModuleGroup& module_group) {
  for (HloModule* module : module_group.modules()) {
    SetInstructionMetadata(*module);
  }
}

}  // namespace

template <typename HloT>
Status HloPassPipeline::RunInvariantCheckers(
    HloT* hlo, absl::string_view after_pass_name,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  for (auto& invariant_checker : invariant_checkers_) {
    ITEX_VLOG(1) << "    Invariant checker " << invariant_checker->name();
    StatusOr<bool> changed_status =
        RunHelper(invariant_checker.get(), hlo, execution_threads);
    ITEX_VLOG(1) << "    Invariant checker done " << invariant_checker->name();
    if (!changed_status.ok()) {
      ITEX_VLOG(2) << "Failed invariant check:";
      ITEX_XLA_VLOG_LINES(2, hlo->ToString());
      return itex::errors::CreateWithUpdatedMessage(
          changed_status.status(),
          absl::StrCat(changed_status.status().error_message(),
                       "\n\nFailed after ", after_pass_name));
    }
    TF_RET_CHECK(!changed_status.value())
        << "invariant checkers must not change the graph";
  }
  return OkStatus();
}

template <typename HloT>
StatusOr<bool> HloPassPipeline::RunPassesInternal(
    HloT* hlo, const DebugOptions& debug_options,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  auto passes = GetEnabledPasses(debug_options);
  // Copy string by value since debug options could get clobbered in an hlo
  // module group pass.
  std::string dump_regex = debug_options.xla_dump_hlo_pass_re();
  static constexpr absl::string_view kPipelineStart = "pipeline-start";
  static constexpr absl::string_view kPipelineEnd = "pipeline-end";
  std::string pipeline_name = std::string(name());

  TF_RETURN_IF_ERROR(RunInvariantCheckers(hlo, kPipelineStart));

  RecordPassStartMetadata(*hlo, std::string(kPipelineStart), pipeline_name);
  SetInstructionMetadata(*hlo);
  MaybeDumpHloAndSaveFilenames(*hlo,
                               /*after_pass_name=*/kPipelineStart,
                               /*before_pass_name=*/passes.empty()
                                   ? kPipelineEnd
                                   : passes.front()->name());
  RecordPassEndMetadata(*hlo, std::string(kPipelineStart),
                        /*module_changed=*/false);

  bool changed = false;
  for (int i = 0; i < passes.size(); i++) {
    HloPassInterface* pass = passes[i];
    XLA_SCOPED_LOGGING_TIMER(absl::StrCat("HLO pass: ", pass->name()));
    std::string pass_name = std::string(pass->name());
    ITEX_VLOG(1) << "  HLO pass " << pass_name;
    ITEX_VLOG(2) << "  Module hash " << absl::HashOf(*hlo);
    if (!pass->IsPassPipeline()) {
      compilation_stats_->StartPass(pass_name);
    }
    RecordPassStartMetadata(*hlo, pass_name, pipeline_name);
    // Embed RunHelper into lambda to enable recording of error statuses
    auto run_helper_lambda =
        [this, pass_name](
            HloPassInterface* pass, HloT* hlo,
            const absl::flat_hash_set<absl::string_view>& execution_threads) {
          auto status_or = RunHelper(pass, hlo, execution_threads);
          if (!status_or.ok()) {
            compilation_stats_->RecordPassError(
                pass_name, itex::error_name(status_or.status().code()));
          }
          return status_or;
        };
    TF_ASSIGN_OR_RETURN(bool pass_changed,
                        run_helper_lambda(pass, hlo, execution_threads));
    SetInstructionMetadata(*hlo);
    if (!dump_regex.empty() && (pass_changed || dump_regex != ".*")) {
      MaybeDumpHloAndSaveFilenames(*hlo,
                                   /*after_pass_name=*/pass_name,
                                   /*before_pass_name=*/i + 1 >= passes.size()
                                       ? kPipelineEnd
                                       : passes[i + 1]->name());
    }
    RecordPassEndMetadata(*hlo, pass_name, pass_changed);
    changed |= pass_changed;
    if (pass_changed) {
      ITEX_VLOG(3) << "  Pass caused changes " << pass->name();
      // Embed RunInvariantCheckers into lambda to enable recording of errors
      auto run_invariant_checkers_lambda = [this](HloT* hlo,
                                                  absl::string_view pass_name) {
        auto status = RunInvariantCheckers(hlo, pass_name);
        if (!status.ok()) {
          compilation_stats_->RecordPassError(pass_name,
                                              itex::error_name(status.code()));
        }
        return status;
      };
      TF_RETURN_IF_ERROR(run_invariant_checkers_lambda(hlo, pass_name));
    }
    if (!pass->IsPassPipeline()) {
      compilation_stats_->EndPass(pass_name);
    }
  }
  return changed;
}

std::vector<HloPassInterface*> HloPassPipeline::GetEnabledPasses(
    const DebugOptions& debug_options) {
  if (debug_options.xla_disable_all_hlo_passes()) {
    ITEX_VLOG(1) << "*All* passes disabled by --xla_disable_all_hlo_passes.";
    return {};
  }

  absl::flat_hash_set<std::string> disabled_pass_names(
      debug_options.xla_disable_hlo_passes().begin(),
      debug_options.xla_disable_hlo_passes().end());

  absl::flat_hash_set<std::string> enabled_pass_names(
      debug_options.xla_enable_hlo_passes_only().begin(),
      debug_options.xla_enable_hlo_passes_only().end());

  if (!disabled_pass_names.empty()) {
    ITEX_VLOG(1) << "Passes disabled by --xla_disable_hlo_passes: "
                 << absl::StrJoin(disabled_pass_names, ", ");
  }

  if (!enabled_pass_names.empty()) {
    ITEX_VLOG(1) << "Passes enabled by --xla_enable_hlo_passes_only: "
                 << absl::StrJoin(enabled_pass_names, ", ");
  }

  ITEX_CHECK(disabled_pass_names.empty() || enabled_pass_names.empty());

  std::vector<HloPassInterface*> enabled_passes;
  if (!enabled_pass_names.empty()) {
    for (auto& pass : passes_) {
      if (enabled_pass_names.contains(pass->name())) {
        enabled_passes.push_back(pass.get());
      }
    }
  } else {
    for (auto& pass : passes_) {
      if (!disabled_pass_names.contains(pass->name())) {
        enabled_passes.push_back(pass.get());
      }
    }
  }
  return enabled_passes;
}

void HloPassPipeline::MaybeDumpHloAndSaveFilenames(
    HloModule& module, absl::string_view after_pass_name,
    absl::string_view before_pass_name) {
  for (const std::string& filename : DumpHloModuleBetweenPassesIfEnabled(
           name(), before_pass_name, after_pass_name, module)) {
    Status status = module.metadata()->add_current_pass_dump_filename(filename);
    if (!status.ok()) {
      ITEX_LOG(FATAL) << status;
    }
  }
}

void HloPassPipeline::MaybeDumpHloAndSaveFilenames(
    HloModuleGroup& module_group, absl::string_view after_pass_name,
    absl::string_view before_pass_name) {
  for (HloModule* module : module_group.modules()) {
    MaybeDumpHloAndSaveFilenames(*module, after_pass_name, before_pass_name);
  }
}

StatusOr<bool> HloPassPipeline::Run(
    HloModule* module,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  run_called_ = true;

  ITEX_VLOG(1) << "Running HLO pass pipeline on module " << module->name()
               << ": " << name();

  return RunPassesInternal(module, module->config().debug_options(),
                           execution_threads);
}

StatusOr<bool> HloPassPipeline::RunOnModuleGroup(
    HloModuleGroup* module_group,
    const absl::flat_hash_set<absl::string_view>& execution_threads) {
  run_called_ = true;

  ITEX_VLOG(1) << "Running HLO pass pipeline on module group "
               << module_group->name() << ": " << name();

  if (module_group->modules().empty()) {
    ITEX_VLOG(1) << "Module group is empty. Nothing to do.";
    return false;
  }

  return RunPassesInternal(module_group,
                           module_group->module(0).config().debug_options(),
                           execution_threads);
}

}  // namespace itex_xla
