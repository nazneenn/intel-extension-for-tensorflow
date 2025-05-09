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

#include "itex/core/compiler/xla/service/gpu/gpu_executable.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "absl/cleanup/cleanup.h"
#include "absl/container/flat_hash_map.h"
#include "absl/memory/memory.h"
#include "absl/synchronization/mutex.h"
#include "itex/core/compiler/xla/map_util.h"
#include "itex/core/compiler/xla/service/gpu/buffer_allocations.h"
#include "itex/core/compiler/xla/service/gpu/gpu_constants.h"
#include "itex/core/compiler/xla/service/gpu/stream_executor_util.h"
#include "itex/core/compiler/xla/service/hlo_instruction.h"
#include "itex/core/compiler/xla/service/hlo_parser.h"
#include "itex/core/compiler/xla/service/llvm_ir/buffer_assignment_util.h"
#include "itex/core/compiler/xla/service/logical_buffer.h"
#include "itex/core/compiler/xla/service/shaped_buffer.h"
#include "itex/core/compiler/xla/service/xla_debug_info_manager.h"
#include "itex/core/compiler/xla/shape_tree.h"
#include "itex/core/compiler/xla/shape_util.h"
#include "itex/core/compiler/xla/status_macros.h"
#include "itex/core/compiler/xla/stream_executor/sycl/sycl_stream.h"
#include "itex/core/compiler/xla/util.h"
#include "itex/core/utils/env.h"
#include "itex/core/utils/errors.h"
#include "itex/core/utils/gtl/map_util.h"
#include "itex/core/utils/logging.h"
namespace itex_xla {
namespace gpu {

bool IsBefExecutableEnabled(const HloModuleConfig& config) {
  (void)config;
  return false;
}

bool IsBefThunkEnabled(const HloModuleConfig& config) {
  (void)config;
  return false;
}

namespace {

// using ::itex::profiler::ScopedAnnotation;

bool NeedsAsyncCommsStream(Thunk& thunk) {
  switch (thunk.kind()) {
    case Thunk::Kind::kNcclAllReduceStart:
    case Thunk::Kind::kNcclAllReduceDone:
      return true;
    default:
      return false;
  }
}

static std::string ModuleUniqueName(absl::string_view module_name,
                                    const HloModule* module) {
  std::string unique_id;
  if (module != nullptr) {
    unique_id = absl::StrCat("module.", module->unique_id(), ".");
  }
  return absl::StrCat(unique_id, module_name);
}

}  // namespace

void GpuExecutable::BefBufferDeleter::operator()(uint8_t* ptr) const {
  ITEX_LOG(FATAL) << "OwnedBefBuffer only supported with XLA_ENABLE_XLIR";
}

StatusOr<std::unique_ptr<GpuExecutable>> GpuExecutable::Create(Params params) {
  auto thunks_or_bef = std::move(params.thunks_or_bef);
  std::unique_ptr<GpuExecutable> result(new GpuExecutable(std::move(params)));

  if (absl::holds_alternative<OwnedThunkSchedule>(thunks_or_bef)) {
    result->thunks_ = std::move(absl::get<OwnedThunkSchedule>(thunks_or_bef));
    return result;
  }

  return InternalError("No thunk or bef provided");
}

// Implementation note: HLO profiling is always enabled for GPU executables,
// since we can use timers around thunks.
GpuExecutable::GpuExecutable(GpuExecutable::Params params)
    : Executable(std::move(params.debug_module)),
      text_(std::move(params.asm_text)),
      binary_(std::move(params.binary)),
      entry_func_attrs_(params.entry_func_attrs),
      module_name_(params.module_name),
      output_shape_(params.output_shape),
      allocations_(std::move(params.allocations)),
      debug_buffer_assignment_(std::move(params.debug_buffer_assignment)),
      verbose_buffer_assignment_string_dumper_(
          params.verbose_buffer_assignment_string_dumper),
      constants_(std::move(params.constants)),
      output_info_(std::move(params.output_info)) {
  XlaDebugInfoManager::Get()->RegisterModule(
      ModuleUniqueName(module_name_, shared_module().get()), shared_module(),
      debug_buffer_assignment_);
}

GpuExecutable::GpuExecutable(
    std::shared_ptr<HloModule> hlo_module,
    itex_xla::EntryFunctionAttributes entry_func_attrs,
    absl::string_view module_name, Shape xla_output_shape,
    std::vector<BufferAllocation> allocations,
    absl::flat_hash_map<ShapeIndex, OutputInfo> output_info)
    : Executable(std::move(hlo_module)),
      entry_func_attrs_(entry_func_attrs),
      module_name_(module_name),
      output_shape_(xla_output_shape),
      allocations_(std::move(allocations)),
      output_info_(std::move(output_info)) {
  XlaDebugInfoManager::Get()->RegisterModule(
      ModuleUniqueName(module_name_, shared_module().get()), shared_module(),
      debug_buffer_assignment_);
}

GpuExecutable::~GpuExecutable() {
  XlaDebugInfoManager::Get()->UnregisterModule(
      ModuleUniqueName(module_name_, shared_module().get()), shared_module(),
      debug_buffer_assignment_);

  {
    // We could have issued host->device mem copies in ResolveConstantGlobals.
    // Wait for those to finish so that we can safely deallocate the backing HLO
    // module.
    //
    // We need for the host->device memcpies to finish they are concurrently
    // reading memory (itex_xla::Literal's) owned by the HLO module.
    absl::MutexLock lock(&module_handle_mutex_);
    for (const auto& pair : module_globals_) {
      ITEX_CHECK(pair.first->SynchronizeAllActivity());
    }
  }
}

Status GpuExecutable::CheckCompatibilityWithServiceExecutableRunOptions(
    const ServiceExecutableRunOptions* run_options) {
  return Status::OK();
}

namespace {

Status MaybeSyncAndProfile(const ServiceExecutableRunOptions* run_options,
                           uint64_t start_micros, se::Stream* stream_to_sync);

Status ExecuteThunks(const std::string& module_name,
                     const ThunkSchedule& thunk_schedule,
                     const ServiceExecutableRunOptions* run_options,
                     const BufferAllocations& buffer_allocations,
                     bool block_host_until_done) {
  XlaDebugInfoManager::Get()->OnModuleStart(module_name);
  auto cleanup = absl::MakeCleanup(
      [&]() { XlaDebugInfoManager::Get()->OnModuleStop(module_name); });

  se::Stream* main_stream = run_options->stream();
  se::StreamExecutor* executor = main_stream->parent();

  StatusOr<StreamPool::Ptr> async_comms_stream =
      run_options->BorrowStream(executor->device_ordinal());

  // TODO(ITEX): how to enable multi-stream?
  // Stream 0 indicates `main_stream` and substreams start from stream 1.
  // std::vector<StreamPool::Ptr> sub_streams;
  // sub_streams.reserve(thunk_schedule.StreamCount() - 1);
  // while (sub_streams.size() + 1 < thunk_schedule.StreamCount()) {
  //   sub_streams.emplace_back();
  //   TF_ASSIGN_OR_RETURN(sub_streams.back(),
  //                       run_options->BorrowStream(executor->device_ordinal()));
  //   // Require substreams to wait for the main stream, otherwise substreams
  //   may
  //   // execute before the program is scheduled to start on the main stream.
  //   sub_streams.back()->ThenWaitFor(main_stream);
  // }

  uint64_t start_micros = itex::Env::Default()->NowMicros();

  // itex::profiler::TraceMe hlo_module_activity(
  //     [&] { return absl::StrCat(module_name, ":XLA GPU module"); },
  //     itex::profiler::TraceMeLevel::kInfo);

  absl::flat_hash_map<const Thunk*, std::unique_ptr<se::Event>>
      thunk_to_finish_event;
  for (const std::unique_ptr<Thunk>& thunk : thunk_schedule.TotalOrder()) {
    // // Annotate execution of this op if tracing was enabled when we started
    // // running this module.  If tracing is enabled *while* we're running the
    // // module, we won't get any data, but that's probably an OK trade-off.
    // ScopedAnnotation annotation([&] { return thunk->profile_annotation(); });

    int32_t stream_no = thunk_schedule.StreamNumberForThunk(thunk.get());
    // se::Stream* stream =
    //     (stream_no == 0 ? main_stream : sub_streams[stream_no - 1].get());
    se::Stream* stream = main_stream;
    for (const Thunk* dependency : thunk_schedule.DependsOn(thunk.get())) {
      stream->ThenWaitFor(FindOrDie(thunk_to_finish_event, dependency).get());
    }

    ITEX_VLOG(2) << "Executing the thunk for " << thunk->profile_annotation()
                 << " on stream " << stream_no;

    TF_RET_CHECK(async_comms_stream.ok() || !NeedsAsyncCommsStream(*thunk))
        << "`run_options` must have a stream borrower for async thunks.";

    Thunk::ExecuteParams thunk_params{
        *run_options, buffer_allocations, stream,
        async_comms_stream.ok() ? async_comms_stream->get() : nullptr};
    // Thunk::ExecuteParams thunk_params{*run_options, buffer_allocations,
    // stream,
    //                                  nullptr};
    TF_RETURN_IF_ERROR(thunk->ExecuteOnStream(thunk_params));
    if (thunk_schedule.Depended(thunk.get())) {
      // auto finish_event =
      // absl::make_unique<SP_Event_st>(main_stream->parent());
      // finish_event->Init();
      se::Event finish_event(stream->parent());
      finish_event.Init();
      stream->ThenRecordEvent(&finish_event);
      thunk_to_finish_event[thunk.get()] =
          std::unique_ptr<se::Event>(&finish_event);
    }
  }

  // main_stream->ThenWaitFor(&sub_streams);
  return MaybeSyncAndProfile(run_options, start_micros,
                             block_host_until_done ? main_stream : nullptr);
}

Status MaybeSyncAndProfile(const ServiceExecutableRunOptions* run_options,
                           uint64_t start_micros,
                           se::Stream* stream_to_sync = nullptr) {
  // Make sure kernels are completed before deallocating temporary buffers or
  // the profiler state.
  // TODO(b/30100571): we could potentially postpone deallocating the temp
  // buffers until a different computation is executed.
  if (stream_to_sync) {
    se::StreamExecutor* executor = stream_to_sync->parent();
    Status block_status = stream_to_sync->BlockHostUntilDone();
    if (!block_status.ok()) {
      return InternalError(
          "Failed to complete all kernels launched on stream %p: %s",
          stream_to_sync, block_status.error_message());
    }
  }

  // FinishExecution() blocks until main_stream has completed if profiling is
  // enabled; we therefore do not need to defer profile collection onto a
  // stream.
  uint64_t end_micros = itex::Env::Default()->NowMicros();

  // if (run_options->run_options().execution_profile()) {
  //   ExecutionProfile* profile =
  //   run_options->run_options().execution_profile(); const double nanoseconds
  //   = (end_micros - start_micros) * 1000.0;
  //   profile->set_compute_time_ns(std::max(nanoseconds, 1.0));
  // }

  return Status::OK();
}

}  // namespace

StatusOr<const GpuExecutable::BufferAllocToDeviceMemoryMap*>
GpuExecutable::ResolveConstantGlobals(se::Stream* stream) {
  se::StreamExecutor* executor = stream->parent();

  absl::MutexLock lock(&module_handle_mutex_);
  auto it = module_globals_.find(executor);
  if (it != module_globals_.end()) {
    return &it->second;
  }

  se::MultiModuleLoaderSpec module_spec;
  if (!binary().empty()) {
    module_spec.AddCudaCubinInMemory(binary());
  }
  module_spec.AddCudaPtxInMemory(text().c_str());

  absl::flat_hash_map<int64_t, se::DeviceMemoryBase> globals;
  se::ModuleHandle module_handle;
  if (module_spec.has_cuda_cubin_in_memory()) {
    TF_RETURN_IF_ERROR(executor->LoadModule(module_spec, &module_handle));
  }

  for (const ConstantInfo& info : constants_) {
    StatusOr<se::DeviceMemoryBase> global_status;
    if (static_cast<bool>(module_handle)) {
      global_status =
          executor->GetUntypedSymbol(info.symbol_name, module_handle);
    }

    se::DeviceMemoryBase global;
    if (static_cast<bool>(module_handle) && global_status.ok()) {
      // The constant was defined in the PTX and has been allocated by the CUDA
      // driver.
      global = *global_status;
      ITEX_VLOG(3) << "Resolved global " << info.symbol_name << " to "
                   << global.opaque();

      if (!info.content.empty()) {
        // This means the constant did not have an initializer in the PTX and
        // therefore must be initialized by XLA here.
        stream->ThenMemcpy(&global, info.content.data(), info.content.size());
      }
    } else {
      // The constant was not defined in the PTX and therefore must be both
      // allocated and initialized by XLA here.
      if (info.content.empty()) {
        void* opaque = nullptr;
        size_t bytes = 0;
        global = se::DeviceMemoryBase(opaque, bytes);
      } else {
        TF_ASSIGN_OR_RETURN(
            auto shared, executor->CreateOrShareConstant(stream, info.content));
        global = *shared;
        ITEX_VLOG(3) << "Allocated (or shared) global " << info.symbol_name
                     << " at " << global.opaque();

        // XLA will continue to own this global at least until this executable
        // is destroyed (longer if another, longer-lived executable shares the
        // same constant).
        shared_constants_.push_back(std::move(shared));
      }
    }

    if (info.allocation_index != -1) {
      InsertOrDie(&globals, info.allocation_index, global);
    }
  }

  module_handles_.emplace(executor,
                          se::ScopedModuleHandle(executor, module_handle));
  return &module_globals_.emplace(executor, std::move(globals)).first->second;
}

StatusOr<se::DeviceMemoryBase> GpuExecutable::BufferForAllocation(
    VariantArguments arguments,
    const GpuExecutable::BufferAllocToDeviceMemoryMap* globals,
    const BufferAllocation& allocation,
    se::DeviceMemoryAllocator* const memory_allocator, int device_ordinal,
    int64_t arg_idx) {
  if (allocation.is_thread_local()) {
    return se::DeviceMemoryBase{};
  } else if (allocation.is_entry_computation_parameter()) {
    int64_t param_no = allocation.parameter_number();
    se::DeviceMemoryBase registered_buffer = [&] {
      if (auto unowned_shapedbuffers =
              absl::get_if<absl::Span<const ShapedBuffer* const>>(&arguments)) {
        return (*unowned_shapedbuffers)[param_no]->buffers().element(
            allocation.param_shape_index());
      } else {
        return absl::get<absl::Span<ExecutionInput>>(arguments)[param_no]
            .Buffer(allocation.param_shape_index())
            .AsDeviceMemoryBase();
      }
    }();
    if (registered_buffer.is_null() && registered_buffer.size() > 0) {
      return FailedPrecondition(
          "Cannot run XLA computation because pointer to (sub-)buffer at "
          "index %s of parameter %d was null.  All pointers to "
          "(sub-)buffers must not be null, unless the (sub-)buffer has "
          "zero elements.",
          allocation.param_shape_index().ToString(), param_no);
    }
    return registered_buffer;
  } else if (allocation.is_constant()) {
    auto it = globals->find(arg_idx);
    if (it == globals->end()) {
      return se::DeviceMemoryBase();
    }
    return it->second;
  } else {
    // Allocate each allocation that might escape, or is the temp buffer.
    ITEX_CHECK(allocation.maybe_live_out() ||
               allocation.IsPreallocatedTempBuffer());
    const int64_t buffer_size = allocation.size();
    se::DeviceMemoryBase buffer_address;
    if (buffer_size > 0) {
      StatusOr<se::OwningDeviceMemory> buffer =
          memory_allocator->Allocate(device_ordinal, buffer_size);
      if (!buffer.ok()) {
        return ResourceExhausted("%s\n%s\n", buffer.status().error_message(),
                                 verbose_buffer_assignment_string_dumper_());
      }
      buffer_address = buffer->Release();
    }
    return buffer_address;
  }
}

static Status CheckAlignment(const BufferAllocation& allocation,
                             se::DeviceMemoryBase buffer, int arg_idx) {
  const int64_t expected_alignment = [&] {
    if (allocation.is_entry_computation_parameter()) {
      return kEntryParameterAlignBytes;
    } else if (allocation.is_constant()) {
      return kConstantBufferAlignBytes;
    } else {
      return kXlaAllocatedBufferAlignBytes;
    }
  }();
  if (!buffer.is_null() &&
      reinterpret_cast<uintptr_t>(buffer.opaque()) % expected_alignment != 0) {
    return InternalError(
        "Address of buffer %d must be a multiple of %x, but "
        "was %p",
        arg_idx, expected_alignment, buffer.opaque());
  }
  return Status::OK();
}

StatusOr<BufferAllocations> GpuExecutable::GenerateBufferAllocations(
    VariantArguments arguments,
    const GpuExecutable::BufferAllocToDeviceMemoryMap* globals,
    se::DeviceMemoryAllocator* const memory_allocator, int device_ordinal) {
  // itex::profiler::TraceMe hlo_module_activity(
  //     [&] { return std::string("Build buffer allocations"); },
  //     itex::profiler::TraceMeLevel::kInfo);

  const int64_t num_buffers = allocations_.size();
  std::vector<se::DeviceMemoryBase> buffers;
  buffers.reserve(num_buffers);
  for (int64_t i = 0; i < num_buffers; ++i) {
    const BufferAllocation& allocation = allocations_[i];
    TF_ASSIGN_OR_RETURN(
        se::DeviceMemoryBase buffer,
        BufferForAllocation(arguments, globals, allocation, memory_allocator,
                            device_ordinal, i));
    buffers.push_back(buffer);
    TF_RETURN_IF_ERROR(CheckAlignment(allocation, buffer, i));
  }
  return {{buffers, device_ordinal, memory_allocator}};
}

StatusOr<ExecutionOutput> GpuExecutable::ExecuteAsyncOnStream(
    const ServiceExecutableRunOptions* run_options,
    std::vector<ExecutionInput> arguments,
    HloExecutionProfile* hlo_execution_profile) {
  return ExecuteAsyncOnStreamImpl(run_options, absl::MakeSpan(arguments));
}
/*
StatusOr<ScopedShapedBuffer> GpuExecutable::ExecuteAsyncOnStream(
    const ServiceExecutableRunOptions* run_options,
    absl::Span<const ShapedBuffer* const> arguments,
    HloExecutionProfile* hlo_execution_profile) {
  TF_ASSIGN_OR_RETURN(ExecutionOutput out,
                      ExecuteAsyncOnStreamImpl(run_options, arguments));
  return out.ConsumeResult();
}
*/
StatusOr<ExecutionOutput> GpuExecutable::ExecuteAsyncOnStreamImpl(
    const ServiceExecutableRunOptions* run_options,
    VariantArguments arguments) {
  XLA_SCOPED_LOGGING_TIMER(absl::StrCat(
      "GpuExecutable::ExecuteAsyncOnStreamImpl(", module_name_, ")"));
  se::DeviceMemoryAllocator* const memory_allocator = run_options->allocator();
  // Force synchronous execution if the allocator requires it.
  const bool block_host_until_done =
      !memory_allocator->AllowsAsynchronousDeallocation();

  se::StreamExecutor* executor = run_options->stream()->parent();

  // // Lock the GPU with a shared lock so that we don't interfere with
  // autotuning
  // // that may be running during JIT compilation while allowing multiple XLA
  // // computations to use the same GPU simultaneously.
  // absl::ReaderMutexLock gpu_lock(&GetGpuMutex(executor));

  const GpuExecutable::BufferAllocToDeviceMemoryMap* globals;
  {
    // itex::profiler::TraceMe hlo_module_activity(
    //     [&] { return std::string("Resolve constant globals"); },
    //     itex::profiler::TraceMeLevel::kInfo);

    TF_ASSIGN_OR_RETURN(globals, ResolveConstantGlobals(run_options->stream()));
  }

  auto device_ordinal = executor->device_ordinal();
  ExecutionOutput result(output_shape_, memory_allocator, device_ordinal);

  TF_ASSIGN_OR_RETURN(
      BufferAllocations buffer_allocations,
      GenerateBufferAllocations(arguments, globals, memory_allocator,
                                device_ordinal));
  ITEX_VLOG(2) << buffer_allocations.ToString();
  std::set<se::DeviceMemoryBase> buffers_in_result;

  const bool is_entire_tuple_contents_aliased = [&] {
    for (auto& p : result.MutableResult()->buffers().leaves()) {
      if (!output_info_.contains(p.first)) {
        continue;
      }
      const OutputInfo& output_info = output_info_.at(p.first);
      if (!output_info.alias_config.has_value()) {
        return false;
      }
    }
    return true;
  }();

  for (auto& p : result.MutableResult()->buffers()) {
    const ShapeIndex& index = p.first;
    if (!output_info_.contains(index)) {
      continue;
    }
    const OutputInfo& output_info = output_info_.at(index);
    const BufferAllocation* allocation =
        &allocations_[output_info.allocation_index];
    se::DeviceMemoryBase& result_buffer = p.second;

    ITEX_VLOG(4) << "Looking at: allocation " << output_info.allocation_index
                 << " @ index: " << index.ToString();

    if (output_info.alias_config) {
      MaybeOwningDeviceMemory* maybe_owning_memory =
          [&]() -> itex_xla::MaybeOwningDeviceMemory* {
        // ScopedBuffer is never an owned buffer.
        if (auto* unowned_shapedbuffers =
                absl::get_if<absl::Span<const ShapedBuffer* const>>(
                    &arguments)) {
          return nullptr;
        } else {
          auto unowned_execution_input =
              absl::get<absl::Span<ExecutionInput>>(arguments);
          ExecutionInput& input =
              unowned_execution_input[allocation->parameter_number()];
          return input.MutableBuffer(allocation->param_shape_index());
        }
      }();
      if (output_info.alias_config->must_alias() && maybe_owning_memory &&
          !maybe_owning_memory->HasOwnership()) {
        return InvalidArgument(
            "An input was configured to be must-alias at "
            "compile time but not donated at runtime: allocation %d",
            output_info.allocation_index);
      }
      if (maybe_owning_memory && maybe_owning_memory->HasOwnership()) {
        absl::optional<se::OwningDeviceMemory> owning =
            maybe_owning_memory->Release();
        // If the caller passes the ownership of the device memory, reuse it
        // as the output buffer. It is up to the caller whether or not to
        // donate a buffer; the aliasing information describes which buffers
        // may alias, not buffers that must alias.
        se::DeviceMemoryBase argument_buffer = owning->Release();
        *maybe_owning_memory = argument_buffer;
        result_buffer = argument_buffer;
        // The caller is giving us the
        // input buffer, but in case of error from the execute call, we should
        // not be releasing it as it contains valid data (for example, it is a
        // parameter which the user wants us to alias, in a gradient update
        // computation). So we store the index into the result in the aliased
        // vector, which will be fed to the ExecutionOutput, which will use
        // the indices to drop the addresses from its own ScopedShapedBuffer
        // result, if the ExecutionOutput is not committed.
        result.AddAliasedIndex(index);
      } else if (!output_info.passthrough &&
                 !ShapeUtil::GetSubshape(output_shape_, index).IsTuple()) {
        // The guard is above is not to insert copy-protection when aliasing
        // pass-through params, as we do not need to write into the output
        // buffer.
        ITEX_VLOG(3) << "Using copy-protection: aliasing is specified, but the "
                        "buffer is not donated; allocating a fresh buffer";
        int64_t allocation_size =
            ShapeUtil::ByteSizeOf(ShapeUtil::GetSubshape(output_shape_, index));
        StatusOr<se::OwningDeviceMemory> allocated_buffer =
            memory_allocator->Allocate(device_ordinal, allocation_size);
        if (!allocated_buffer.ok()) {
          return ResourceExhausted("%s\n%s\n",
                                   allocated_buffer.status().error_message(),
                                   verbose_buffer_assignment_string_dumper_());
        }
        result_buffer = allocated_buffer->Release();
        se::DeviceMemoryBase& aliased_buffer =
            buffer_allocations.GetMutableDeviceAddress(
                output_info.allocation_index);
        ITEX_CHECK_EQ(aliased_buffer.size(), result_buffer.size());
        se::Stream* stream = run_options->stream();
        stream->ThenMemcpy(&result_buffer, aliased_buffer,
                           aliased_buffer.size());
        aliased_buffer = result_buffer;
      }
    }

    if (result_buffer.is_null()) {
      // The source instruction should have a non-parameter buffer
      // assigned.
      result_buffer =
          buffer_allocations.GetDeviceAddress(output_info.allocation_index);

      // If the entire tuple contents is aliased, the copy insertion will *not*
      // materialize a new tuple, so we mark it as aliased as well.
      if (is_entire_tuple_contents_aliased) {
        result.AddAliasedIndex(index);
      }
    }
    buffers_in_result.insert(result_buffer);
  }

  TF_RETURN_IF_ERROR(ExecuteThunksOrBef(run_options, buffer_allocations,
                                        block_host_until_done));

  // Free all temporary allocations.
  TF_RETURN_IF_ERROR(
      buffer_allocations.TearDown(buffers_in_result, allocations_));

  // Free allocations for arguments.
  if (auto args = absl::get_if<absl::Span<ExecutionInput>>(&arguments)) {
    MarkToBeReleasedArguments(*args, result);
  }
  return std::move(result);
}

Status GpuExecutable::ExecuteThunksOrBef(
    const ServiceExecutableRunOptions* run_options,
    const BufferAllocations& buffer_allocations, bool block_host_until_done) {
  TF_RETURN_IF_ERROR(
      CheckCompatibilityWithServiceExecutableRunOptions(run_options));

  if (thunks_) {
    se::StreamExecutor* executor = run_options->stream()->parent();
    for (const std::unique_ptr<Thunk>& thunk : thunks_->TotalOrder()) {
      TF_RETURN_IF_ERROR(thunk->Initialize(*this, executor));
    }
    return ExecuteThunks(module_name_, *thunks_, run_options,
                         buffer_allocations, block_host_until_done);
  }

  return FailedPrecondition("Expected thunk or bef is not supplied.");
}

int64_t GpuExecutable::SizeOfGeneratedCodeInBytes() const {
  // Non-empty PTX but empty cubin: compilation must have failed, return
  // "unknown".
  if (binary().empty() && !text_.empty()) {
    return -1;
  }
  int64_t size = binary().size();
  for (BufferAllocation::Index i = 0; i < allocations_.size(); ++i) {
    const BufferAllocation& allocation = allocations_[i];
    if (allocation.is_constant()) {
      size += allocation.size();
    }
  }
  return size;
}

Status GpuExecutable::SetUpMlirAllocation(
    mlir::func::FuncOp func, llvm::ArrayRef<int64_t> buffer_sizes,
    std::vector<BufferAllocation>* allocations,
    absl::flat_hash_map<ShapeIndex, GpuExecutable::OutputInfo>* output_info,
    Shape* output_shape, int buffer_param_offset) {
  for (int i = 0; i < buffer_sizes.size(); i++) {
    allocations->emplace_back(i, buffer_sizes[i], 0);
  }

  for (int i = 0; i < func.getNumArguments(); i++) {
    if (i < buffer_param_offset) {
      continue;
    }
    const int buffer_index = i - buffer_param_offset;

    if (auto param_attr = func.getArgAttr(i, "lmhlo.params")) {
      itex_xla::ShapeIndex shape_index;
      if (auto shape_index_attr =
              func.getArgAttrOfType<mlir::DenseIntElementsAttr>(
                  i, "lmhlo.param_shape_index")) {
        for (const llvm::APInt& element : shape_index_attr) {
          shape_index.push_back(element.getSExtValue());
        }
      }
      allocations->at(buffer_index)
          .set_entry_computation_parameter(
              param_attr.cast<mlir::IntegerAttr>().getInt(), shape_index,
              static_cast<bool>(func.getArgAttr(i, "lmhlo.output_index")));
    }
    // TODO(timshen): this information is redundant. This is here only for
    // smooth migration to LMHLO. Remove it.
    if (func.getArgAttr(i, "lmhlo.constant_name")) {
      allocations->at(buffer_index).set_constant(true);
    }
    if (auto output_index_attr = func.getArgAttr(i, "lmhlo.output_index")) {
      allocations->at(buffer_index).set_maybe_live_out(true);

      // Reconstruct a shape index from output_index.
      ShapeIndex shape_index;
      for (const llvm::APInt& element :
           output_index_attr.cast<mlir::DenseIntElementsAttr>()) {
        shape_index.push_back(element.getSExtValue());
      }
      auto& o = (*output_info)[shape_index];
      o.allocation_index = buffer_index;
      if (auto param_attr = func.getArgAttr(i, "lmhlo.params")) {
        HloInputOutputAliasConfig::AliasKind kind =
            HloInputOutputAliasConfig::kMayAlias;
        if (func.getArgAttr(i, "lmhlo.must_alias")) {
          kind = HloInputOutputAliasConfig::kMustAlias;
        }
        o.alias_config.emplace(param_attr.cast<mlir::IntegerAttr>().getInt(),
                               ShapeIndex{}, kind);
      }
      if (func.getArgument(i).use_empty()) {
        o.passthrough = true;
      }
    }
  }
  // Expects result_xla_shape as a XLA shape in string form.
  //
  // The attribute is necessary, because GpuExecutable/ExecutionOutput supports
  // tuples / tree-like shapes, while the LMHLO argument list loses the tree
  // form.
  //
  // The string format is necessary since MLIR doesn't support XLA shape with
  // dynamic_dimension.
  //
  // TODO(timshen): now this field is mandatory. Make it optional for
  // non-GpuExecutable outputs.
  TF_ASSIGN_OR_RETURN(
      *output_shape,
      ParseShape(func->getAttrOfType<mlir::StringAttr>("result_xla_shape")
                     .getValue()
                     .str()));

  return Status::OK();
}

StatusOr<std::unique_ptr<Executable>> GpuExecutable::LoadFromBef(
    std::shared_ptr<HloModule> hlo_module, absl::string_view bef,
    itex_xla::EntryFunctionAttributes entry_func_attrs) {
  return FailedPrecondition("Not built with XLA_ENABLE_XLIR");
}

StatusOr<absl::flat_hash_map<ShapeIndex, GpuExecutable::OutputInfo>>
GetOutputInfo(const HloModule& hlo_module, const BufferAssignment& assignment) {
  const HloInstruction* root =
      hlo_module.entry_computation()->root_instruction();

  InstructionValueSet root_value_set =
      assignment.dataflow_analysis().GetInstructionValueSet(root);

  if (root_value_set.IsAmbiguous()) {
    return Unimplemented("Points-to set of root instruction is ambiguous");
  }

  using OutputInfoMap =
      absl::flat_hash_map<ShapeIndex, GpuExecutable::OutputInfo>;
  OutputInfoMap output;
  TF_RETURN_IF_ERROR(ShapeUtil::ForEachSubshapeWithStatus(
      root->shape(),
      [&](const Shape& /*sub_shape*/, const ShapeIndex& index) -> Status {
        const auto& sources = root_value_set.element(index);
        // The points-to set is unambiguous so the set should be a
        // singleton. That is, we know exactly which instruction
        // produced the array at this element.
        ITEX_CHECK_EQ(1, sources.values().size());
        HloInstruction* src_hlo = sources.values()[0]->instruction();

        GpuExecutable::OutputInfo& info = output[index];
        info.passthrough = src_hlo->opcode() == HloOpcode::kParameter;
        TF_ASSIGN_OR_RETURN(
            const BufferAllocation::Slice slice,
            assignment.GetUniqueSlice(src_hlo, sources.values()[0]->index()));
        ITEX_CHECK_EQ(slice.offset(), 0)
            << "Parameter should get its own slice";
        info.allocation_index = slice.index();

        output[index].alias_config =
            hlo_module.input_output_alias_config().GetAliasedParameter(index);

        return Status::OK();
      }));
  return output;
}

}  // namespace gpu
}  // namespace itex_xla
