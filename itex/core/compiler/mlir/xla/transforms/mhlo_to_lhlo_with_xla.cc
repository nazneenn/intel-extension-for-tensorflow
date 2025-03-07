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

#include "itex/core/compiler/mlir/xla/transforms/mhlo_to_lhlo_with_xla.h"

#include <climits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/cleanup/cleanup.h"
#include "absl/types/optional.h"
#include "itex/core/compiler/mlir/tensorflow/utils/error_util.h"
#include "itex/core/compiler/mlir/xla/attribute_importer.h"
#include "itex/core/compiler/mlir/xla/hlo_function_importer.h"
#include "itex/core/compiler/mlir/xla/hlo_utils.h"
#include "itex/core/compiler/mlir/xla/mlir_hlo_to_hlo.h"
#include "itex/core/compiler/mlir/xla/type_to_shape.h"
#include "itex/core/compiler/xla/debug_options_flags.h"
#include "itex/core/compiler/xla/service/buffer_assignment.h"
#include "itex/core/compiler/xla/service/gpu/ir_emission_utils.h"
#include "itex/core/compiler/xla/service/gpu/mkl.h"
#include "itex/core/compiler/xla/service/hlo_casting_utils.h"
#include "itex/core/compiler/xla/service/hlo_computation.h"
#include "itex/core/compiler/xla/service/hlo_instruction.h"
#include "itex/core/compiler/xla/service/hlo_instructions.h"
#include "itex/core/compiler/xla/service/hlo_module.h"
#include "itex/core/compiler/xla/service/hlo_parser.h"
#include "itex/core/compiler/xla/service/llvm_ir/buffer_assignment_util.h"
#include "itex/core/compiler/xla/shape_util.h"
#include "itex/core/compiler/xla/statusor.h"
#include "itex/core/compiler/xla/util.h"
#include "itex/core/compiler/xla/window_util.h"
#include "lhlo/IR/lhlo_ops.h"
#include "lhlo_gpu/IR/lhlo_gpu_ops.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mhlo/IR/hlo_ops.h"
#include "mlir/Dialect/Arith/IR/Arith.h"                  // from @llvm-project
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"                 // from @llvm-project
#include "mlir/Dialect/MemRef/IR/MemRef.h"                // from @llvm-project
#include "mlir/IR/AffineExpr.h"                           // from @llvm-project
#include "mlir/IR/AffineMap.h"                            // from @llvm-project
#include "mlir/IR/Attributes.h"                           // from @llvm-project
#include "mlir/IR/Builders.h"                             // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"                    // from @llvm-project
#include "mlir/IR/BuiltinOps.h"                           // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"                         // from @llvm-project
#include "mlir/IR/Dialect.h"                              // from @llvm-project
#include "mlir/IR/Location.h"                             // from @llvm-project
#include "mlir/IR/MLIRContext.h"                          // from @llvm-project
#include "mlir/IR/OpDefinition.h"                         // from @llvm-project
#include "mlir/IR/Operation.h"                            // from @llvm-project
#include "mlir/IR/PatternMatch.h"                         // from @llvm-project
#include "mlir/IR/SymbolTable.h"                          // from @llvm-project
#include "mlir/IR/Verifier.h"                             // from @llvm-project
#include "mlir/Pass/Pass.h"                               // from @llvm-project
#include "mlir/Pass/PassOptions.h"                        // from @llvm-project
#include "mlir/Tools/mlir-translate/Translation.h"        // from @llvm-project
#include "protos/backend_configs.pb.h"
#include "protos/xla_data.pb.h"

using itex_xla::BufferAllocation;
using itex_xla::BufferAssignment;
using itex_xla::HloComputation;
using itex_xla::HloCustomCallInstruction;
using itex_xla::HloInfeedInstruction;
using itex_xla::HloInstruction;
using itex_xla::HloModule;
using itex_xla::HloModuleProto;
using itex_xla::HloOutfeedInstruction;
using itex_xla::HloProto;
using itex_xla::Shape;
using itex_xla::Status;
using itex_xla::StatusOr;

namespace mlir {
/*
namespace {

absl::string_view StringRefToView(llvm::StringRef ref) {
  return {ref.data(), ref.size()};
}

StatusOr<std::unique_ptr<HloModule>> HloModuleFromProto(
    const HloProto& hlo_proto) {
  const HloModuleProto& module_proto = hlo_proto.hlo_module();
  TF_ASSIGN_OR_RETURN(const itex_xla::HloModuleConfig module_config,
                      HloModule::CreateModuleConfigFromProto(
                          module_proto, itex_xla::GetDebugOptionsFromFlags()));
  return HloModule::CreateFromProto(module_proto, module_config);
}

}  // namespace

// Convert the MLIR `module` from HLO dialect to LHLO dialect using XLA for the
// given platform.
Status OptimizeAndConvertHloToLmhlo(std::unique_ptr<HloModule> hlo_module,
                                    ModuleOp module, StringRef platform_name,
                                    bool optimize_xla_hlo) {
  auto platform = itex_xla::se::MultiPlatformManager::PlatformWithName(
      StringRefToView(platform_name));
  if (!platform.ok()) {
    std::string error_msg;
    llvm::raw_string_ostream os(error_msg);
    os << "failed to get platform: " << platform.status().ToString()
       << " (available Platform: ";
    std::vector<std::string> available_platforms;
    (void)itex_xla::se::MultiPlatformManager::PlatformsWithFilter(
        [&](const stream_executor::Platform* p) {
          available_platforms.push_back(p->Name());
          return false;
        });
    llvm::interleaveComma(available_platforms, os);
    os << ")";
    return itex_xla::InvalidArgument("%s", os.str().c_str());
  }

  itex_xla::BackendOptions backend_options;
  backend_options.set_platform(platform.ValueOrDie());
  auto backend_or_err = itex_xla::Backend::CreateBackend(backend_options);
  TF_RETURN_WITH_CONTEXT_IF_ERROR(backend_or_err.status(),
                                  "failed to create XLA Backend ");
  auto backend = std::move(backend_or_err.ValueOrDie());

  StatusOr<std::unique_ptr<HloModule>> optimized_hlo_module;

  if (optimize_xla_hlo) {
    // Run all HLO passes to produce an optimized module.
    optimized_hlo_module = backend->compiler()->RunHloPasses(
        std::move(hlo_module), backend->default_stream_executor(),
        backend->memory_allocator());
    TF_RETURN_WITH_CONTEXT_IF_ERROR(optimized_hlo_module.status(),
                                    "running XLA pass pipeline");
  } else {
    optimized_hlo_module = std::move(hlo_module);
  }

  StatusOr<std::unique_ptr<BufferAssignment>> assignment =
      backend->compiler()->AssignBuffers(optimized_hlo_module->get());
  TF_RETURN_WITH_CONTEXT_IF_ERROR(assignment.status(),
                                  "running XLA buffer assigment");

  // Clear the module before populating it back with the result of the
  // conversion.
  module.getBody()->clear();
  OpBuilder builder(module);

  TF_RETURN_WITH_CONTEXT_IF_ERROR(
      HloToLhloModule(**assignment, **optimized_hlo_module, module),
      "converting HLO to LHLO");

  return Status::OK();
}
*/
// namespace {
// // This pass takes an MLIR HLO module, converts it to XLA to perform the HLO
// // optimization pipeline for the required platform, and then converts it back
// to
// // MLIR LHLO.
// class XlaHloToLhloPass
//     : public PassWrapper<XlaHloToLhloPass, OperationPass<ModuleOp>> {
//   void getDependentDialects(DialectRegistry& registry) const override {
//     registry
//         .insert<arith::ArithDialect,
//         bufferization::BufferizationDialect,
//                 func::FuncDialect, memref::MemRefDialect, mhlo::MhloDialect,
//                 lmhlo::LmhloDialect, lmhlo_gpu::LmhloGpuDialect>();
//   }

//  public:
//   MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(XlaHloToLhloPass)

//   XlaHloToLhloPass() = default;
//   XlaHloToLhloPass(const XlaHloToLhloPass&) {}
//   StringRef getArgument() const final { return "xla-hlo-to-lhlo-with-xla"; }
//   StringRef getDescription() const final {
//     return "Emit LHLO from HLO using the existing XLA implementation";
//   }

//  private:
//   void runOnOperation() final {
//     ModuleOp module = getOperation();

//     auto status = [&module, this]() -> Status {
//       SymbolTable symbol_table(module);
//       if (!symbol_table.lookup("main")) {
//         return itex_xla::InvalidArgument(
//             "conversion to HLO module failed: missing main()");
//       }
//       HloProto hlo_proto;
//       TF_RETURN_WITH_CONTEXT_IF_ERROR(
//           ConvertMlirHloToHlo(module, &hlo_proto,
//                               /*use_tuple_args=*/false,
//                               /*return_tuple=*/false,
//                               /*shape_determination_fns=*/{}),
//           "conversion to XLA HLO proto failed");

//       auto statusOrHloModule = HloModuleFromProto(hlo_proto);
//       TF_RETURN_WITH_CONTEXT_IF_ERROR(statusOrHloModule.status(),
//                                       "parsing HLO proto to HLO module
//                                       failed");
//       std::unique_ptr<HloModule> hlo_module =
//           std::move(statusOrHloModule.ValueOrDie());

//       return OptimizeAndConvertHloToLmhlo(std::move(hlo_module), module,
//                                           platform_, optimize_xla_hlo_);
//     }();
//     if (!status.ok()) {
//       module.emitError() << status.ToString();
//       return signalPassFailure();
//     }
//   }

//   Option<std::string> platform_{
//       *this, "platform",
//       llvm::cl::desc("The platform to use for the XLA optimization
//       pipeline."), llvm::cl::init("Host")};
//   Option<bool> optimize_xla_hlo_{
//       *this, "optimize-xla-hlo",
//       llvm::cl::desc("Whether to apply HLO optimizations."),
//       llvm::cl::init(true)};
// };

// }  // namespace

// Creates MLIR operands corresponding to operands and results of the XLA HLO
// instruction. If `num_operands` is valid, then only the first `num_operands`
// operands of the HLO instruction will be considered.
Status LhloDialectEmitter::CreateOperands(
    const HloInstruction* instr, absl::optional<int64_t> num_operands,
    TokenLoweringMode token_mode, llvm::SmallVectorImpl<Value>& operands,
    size_t& num_arguments, size_t& num_results) {
  if (num_operands.value_or(0) > instr->operand_count())
    return itex_xla::InvalidArgument("num_operands must be <= operand count");
  for (int64_t i = 0; i < num_operands.value_or(instr->operand_count()); ++i) {
    TF_RETURN_IF_ERROR(GetOrCreateView(instr->operand(i), &operands,
                                       /*result_subset=*/{}, token_mode));
  }
  num_arguments = operands.size();
  TF_RETURN_IF_ERROR(
      GetOrCreateView(instr, &operands, /*result_subset=*/{}, token_mode));
  num_results = operands.size() - num_arguments;
  return Status::OK();
}

template <typename OpType>
OpType LhloDialectEmitter::CreateOpWithoutAttrs(const HloInstruction* instr,
                                                ValueRange operands) {
  Location loc = getLocation(instr);
  return builder_.create<OpType>(loc, llvm::None, operands,
                                 llvm::ArrayRef<NamedAttribute>{});
}

template <typename OpType>
StatusOr<OpType> LhloDialectEmitter::CreateOpWithoutAttrs(
    const HloInstruction* instr, size_t& num_arguments, size_t& num_results,
    absl::optional<int64_t> num_operands) {
  llvm::SmallVector<Value, 4> operands;
  TF_RETURN_IF_ERROR(CreateOperands(instr, num_operands,
                                    TokenLoweringMode::kFailToLower, operands,
                                    num_arguments, num_results));
  return CreateOpWithoutAttrs<OpType>(instr, operands);
}

StatusOr<mlir::Operation*> LhloDialectEmitter::CreateOpInFusion(
    const HloInstruction* instr, ValueRange buffer_operands,
    size_t num_arguments, size_t num_results) {
  Location loc = getLocation(instr);
  std::vector<Value> buffers(buffer_operands.begin(), buffer_operands.end());
  absl::Span<Value> arguments =
      absl::MakeSpan(buffers).subspan(0, num_arguments);
  absl::Span<Value> results =
      absl::MakeSpan(buffers).subspan(num_arguments, num_results);

  mlir::lmhlo::FusionOp fusion = builder_.create<mlir::lmhlo::FusionOp>(loc);
  mlir::OpBuilder b(&fusion.getRegion());

  llvm::SmallVector<mlir::Value, 4> loads;
  for (Value arg : arguments) {
    auto load = b.create<mlir::bufferization::ToTensorOp>(loc, arg);
    Shape shape = itex_xla::TypeToShape(arg.getType());
    TF_RET_CHECK(shape.IsArray());
    if (shape.layout() !=
        itex_xla::LayoutUtil::MakeDescendingLayout(shape.dimensions().size())) {
      load->setAttr("xla_shape",
                    b.getStringAttr(shape.ToString(/*print_layout=*/true)));
    }
    loads.push_back(load);
  }
  mlir::Operation* op = nullptr;
  if (instr->opcode() == itex_xla::HloOpcode::kReduce) {
    TF_RET_CHECK(loads.size() % 2 == 0);
    std::vector<int64_t> dimensions(instr->dimensions().begin(),
                                    instr->dimensions().end());
    auto reduce_op = b.create<mhlo::ReduceOp>(
        loc, llvm::makeArrayRef(loads).take_front(loads.size() / 2),
        llvm::makeArrayRef(loads).drop_front(loads.size() / 2),
        GetI64DenseElementsAttr(dimensions));

    TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
        *instr->called_computations()[0], &reduce_op.getBody(), &builder_,
        /*flatten_region_arg_tuple=*/true));
    op = reduce_op;
  } else {
    TF_ASSIGN_OR_RETURN(
        op, itex_xla::HloFunctionImporter::ImportInstruction(
                instr, loads, &b,
                itex_xla::DynamicShapeHandlingMode::kConvertToStatic));
  }
  TF_RET_CHECK(op->getNumResults() == num_results);
  for (int i = 0; i < results.size(); i++) {
    b.create<mlir::memref::TensorStoreOp>(loc, op->getResult(i), results[i]);
  }
  return op;
}

StatusOr<mlir::Operation*> LhloDialectEmitter::CreateOpInFusion(
    const HloInstruction* instr) {
  llvm::SmallVector<Value, 4> operands;
  size_t num_arguments, num_results;
  TF_RETURN_IF_ERROR(CreateOperands(instr, absl::nullopt,
                                    TokenLoweringMode::kFailToLower, operands,
                                    num_arguments, num_results));
  TF_ASSIGN_OR_RETURN(
      auto op, CreateOpInFusion(instr, operands, num_arguments, num_results));
  return op->getParentOp();
}

StatusOr<mlir::Operation*> LhloDialectEmitter::EmitOp(
    const HloInstruction* instr) {
  using itex_xla::HloOpcode;
  switch (instr->opcode()) {
    case HloOpcode::kAddDependency:
      return nullptr;
    case HloOpcode::kAfterAll:
      // LMHLO is already ordered. This assumption may be broken after
      // introducing async regions and partial orders.
      return nullptr;
    case HloOpcode::kAllToAll:
      return EmitAllToAllOp(instr);
    case HloOpcode::kAllGather:
      return EmitAllGatherOp(instr);
    case HloOpcode::kAllReduce:
      return EmitAllReduceOp(instr);
    case HloOpcode::kAllReduceStart:
      return EmitAllReduceStartOp(instr);
    case HloOpcode::kAllReduceDone:
      return EmitAllReduceDoneOp(instr);
    case HloOpcode::kReduceScatter:
      return EmitReduceScatterOp(instr);
    case HloOpcode::kBitcast:
      return EmitBitcast(instr);
    case HloOpcode::kCollectivePermute:
      return EmitCollectivePermuteOp(instr);
    case HloOpcode::kConditional:
      return EmitCaseOp(instr);
    case HloOpcode::kFft:
      return EmitFftOp(instr);
    case HloOpcode::kGetTupleElement:
      return nullptr;
    case HloOpcode::kInfeed:
      return EmitInfeedOp(instr);
    case HloOpcode::kOutfeed:
      return EmitOutfeedOp(instr);
    case HloOpcode::kPartitionId:
      return CreateOpWithoutAttrs<lmhlo::PartitionIdOp>(instr);
    case HloOpcode::kReplicaId:
      return CreateOpWithoutAttrs<lmhlo::ReplicaIdOp>(instr);
    case HloOpcode::kTriangularSolve:
      return EmitTriangularSolveOp(instr);
    case HloOpcode::kTuple:
      return nullptr;
    case HloOpcode::kSort:
      return EmitSortOp(instr);
    case HloOpcode::kFusion:
      return EmitFusionOp(instr);
    case HloOpcode::kScatter:
      return EmitScatterOp(instr);
    case HloOpcode::kSelectAndScatter:
      return EmitSelectAndScatterOp(instr);
    case HloOpcode::kCustomCall:
      return EmitCustomCallOp(instr);
    case HloOpcode::kConstant:
      return EmitConstant(instr);
    case HloOpcode::kRngGetAndUpdateState:
      return EmitRngGetAndUpdateStateOp(instr);
    case HloOpcode::kWhile:
      return EmitWhileOp(instr);

    case HloOpcode::kAbs:
    case HloOpcode::kAdd:
    case HloOpcode::kAnd:
    case HloOpcode::kAtan2:
    case HloOpcode::kBitcastConvert:
    case HloOpcode::kBroadcast:
    case HloOpcode::kCeil:
    case HloOpcode::kCbrt:
    case HloOpcode::kClamp:
    case HloOpcode::kClz:
    case HloOpcode::kCompare:
    case HloOpcode::kComplex:
    case HloOpcode::kConcatenate:
    case HloOpcode::kConvert:
    case HloOpcode::kCos:
    case HloOpcode::kDivide:
    case HloOpcode::kDot:
    case HloOpcode::kDynamicSlice:
    case HloOpcode::kDynamicUpdateSlice:
    case HloOpcode::kExp:
    case HloOpcode::kExpm1:
    case HloOpcode::kFloor:
    case HloOpcode::kGather:
    case HloOpcode::kImag:
    case HloOpcode::kIota:
    case HloOpcode::kIsFinite:
    case HloOpcode::kLog:
    case HloOpcode::kLog1p:
    case HloOpcode::kMap:
    case HloOpcode::kMaximum:
    case HloOpcode::kMinimum:
    case HloOpcode::kMultiply:
    case HloOpcode::kNegate:
    case HloOpcode::kNot:
    case HloOpcode::kOr:
    case HloOpcode::kPad:
    case HloOpcode::kPopulationCount:
    case HloOpcode::kPower:
    case HloOpcode::kReal:
    case HloOpcode::kReshape:
    case HloOpcode::kReducePrecision:
    case HloOpcode::kReduceWindow:
    case HloOpcode::kRemainder:
    case HloOpcode::kReverse:
    case HloOpcode::kRoundNearestAfz:
    case HloOpcode::kRoundNearestEven:
    case HloOpcode::kRsqrt:
    case HloOpcode::kSelect:
    case HloOpcode::kShiftLeft:
    case HloOpcode::kShiftRightLogical:
    case HloOpcode::kShiftRightArithmetic:
    case HloOpcode::kSign:
    case HloOpcode::kSin:
    case HloOpcode::kSlice:
    case HloOpcode::kSqrt:
    case HloOpcode::kSubtract:
    case HloOpcode::kTanh:
    case HloOpcode::kTranspose:
    case HloOpcode::kXor:
    case HloOpcode::kCopy:
    case HloOpcode::kReduce:
      return CreateOpInFusion(instr);
    default:
      llvm::errs() << instr->ToString();
      return itex::errors::Internal(absl::StrCat(
          "LHLO opcode ", itex_xla::HloOpcodeString(instr->opcode()),
          " is not supported."));
  }
}

Status LhloDialectEmitter::DefaultAction(const HloInstruction* instr) {
  return EmitOp(instr).status();
}

StatusOr<lmhlo::SortOp> LhloDialectEmitter::EmitSortOp(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto sort, CreateOpWithoutAttrs<lmhlo::SortOp>(instr));
  auto* sort_instr = itex_xla::Cast<itex_xla::HloSortInstruction>(instr);
  sort.setDimensionAttr(
      builder_.getI64IntegerAttr(sort_instr->sort_dimension()));
  sort.setIsStableAttr(builder_.getBoolAttr(sort_instr->is_stable()));
  TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
      *sort_instr->called_computations()[0], &sort.getComparator(), &builder_));
  return sort;
}

// Walks MHLO::TupleOp recursively.
Status WalkTuplePostOrder(Value v,
                          const std::function<Status(Value)>& visitor) {
  if (auto* op = v.getDefiningOp()) {
    if (auto tuple = dyn_cast<mhlo::TupleOp>(op)) {
      for (Value sub_v : tuple.getVal()) {
        TF_RETURN_IF_ERROR(WalkTuplePostOrder(sub_v, visitor));
      }
      return Status::OK();
    }
  }
  return visitor(v);
}

StatusOr<Value> LhloDialectEmitter::RewriteFusionOperand(
    const HloInstruction* root, const Shape& shape,
    itex_xla::ShapeIndex* shape_index, OpBuilder* b, Location loc) {
  if (shape.IsTuple()) {
    llvm::SmallVector<Value, 4> values;
    for (int i = 0; i < shape.tuple_shapes_size(); ++i) {
      shape_index->push_back(i);
      TF_ASSIGN_OR_RETURN(
          auto v, RewriteFusionOperand(root, shape.tuple_shapes(i), shape_index,
                                       b, loc));
      values.push_back(v);
      shape_index->pop_back();
    }
    return Value(b->create<mhlo::TupleOp>(loc, values));
  }
  TF_ASSIGN_OR_RETURN(Value memref,
                      GetOrCreateArrayView(root, shape, *shape_index));
  auto load = b->create<bufferization::ToTensorOp>(loc, memref);
  if (shape.layout() !=
      itex_xla::LayoutUtil::MakeDescendingLayout(shape.dimensions().size())) {
    llvm::SmallVector<int64_t, 4> minor_to_major(
        shape.layout().minor_to_major().begin(),
        shape.layout().minor_to_major().end());
    load->setAttr("xla_shape",
                  b->getStringAttr(shape.ToString(/*print_layout=*/true)));
  }
  return load.getResult();
}

// Emit a lmhlo.fusion based on XLA HLO fusion. Structurally they are not neatly
// equivalent. Specifically, XLA HLO fusion:
//     fused_computation {
//       %p0 = parameter(0)
//       %p1 = parameter(1)
//       ...
//       ROOT %ret = ...
//     }
// will be converted to
//     lmhlo.fusion() {  // no explicit operands
//       // capturing outside buffers
//       %p0 = bufferization.to_tensor(%arg0) : memref<...> -> tensor<...>
//       %p1 = bufferization.to_tensor(%arg1) : memref<...> -> tensor<...>
//       ...
//       tensor_store ..., %ret // store a tensor to a memref
//     }
StatusOr<lmhlo::FusionOp> LhloDialectEmitter::EmitFusionOp(
    const HloInstruction* instr) {
  Location loc = getLocation(instr);

  auto* fusion_instr = itex_xla::Cast<itex_xla::HloFusionInstruction>(instr);

  auto fusion = builder_.create<lmhlo::FusionOp>(getLocation(instr));
  auto after_fusion = builder_.saveInsertionPoint();
  auto reverter = absl::MakeCleanup(
      [this, after_fusion] { builder_.restoreInsertionPoint(after_fusion); });
  builder_ = mlir::OpBuilder(fusion);

  auto region_builder = OpBuilder::atBlockBegin(&fusion.getRegion().front());

  llvm::SmallVector<Value, 8> arguments;
  for (int i = 0; i < instr->operands().size(); ++i) {
    const HloInstruction* operand = instr->operand(i);
    itex_xla::ShapeIndex shape_index;
    TF_ASSIGN_OR_RETURN(
        auto arg, RewriteFusionOperand(operand, operand->shape(), &shape_index,
                                       &region_builder, loc));
    arguments.push_back(arg);
  }

  TF_ASSIGN_OR_RETURN(Value result,
                      itex_xla::HloFunctionImporter::ImportInstructions(
                          *fusion_instr->fused_instructions_computation(),
                          arguments, &region_builder));
  {
    int i = 0;
    llvm::SmallVector<Value, 4> output;
    TF_RETURN_IF_ERROR(GetOrCreateView(instr, &output));
    TF_RETURN_IF_ERROR(WalkTuplePostOrder(result, [&](Value v) mutable {
      region_builder.create<memref::TensorStoreOp>(loc, v, output[i++]);
      return Status::OK();
    }));
    if (i != output.size()) {
      return itex_xla::InternalError("output sizes don't match");
    }
  }

  // Fold GTE/Tuple pairs.
  //
  // Since the fused region refers to values in its parent region, we can't
  // call applyPatternAndFoldGreedily. We optimize it manually.
  //
  // Only walk once, because post-ordering is exactly what we need for GTE
  // optimizations.
  fusion.getRegion().walk([](mhlo::GetTupleElementOp gte) {
    SmallVector<Value, 4> folded_values;
    if (succeeded(OpBuilder(gte).tryFold(gte, folded_values))) {
      gte.replaceAllUsesWith(folded_values[0]);
    }
  });

  // Effectively a DCE on the region.
  {
    llvm::SmallVector<mlir::Operation*, 4> ops;
    fusion.getRegion().walk([&](mlir::Operation* op) { ops.push_back(op); });
    // Visit the user first.
    std::reverse(ops.begin(), ops.end());
    for (auto op : ops) {
      if (isOpTriviallyDead(op)) op->erase();
    }
  }

  return fusion;
}

StatusOr<mhlo::ScatterDimensionNumbersAttr>
LhloDialectEmitter::GetScatterDimensionNumbers(const HloInstruction* instr,
                                               mlir::MLIRContext* context) {
  auto* scatter_instr = itex_xla::Cast<itex_xla::HloScatterInstruction>(instr);

  const itex_xla::ScatterDimensionNumbers& xla_scatter_dim =
      scatter_instr->scatter_dimension_numbers();

  auto get_i64_array = [](absl::Span<const int64_t> container) {
    return ArrayRef<int64_t>{container.data(),
                             static_cast<size_t>(container.size())};
  };
  auto scatter_dimension_numbers = mhlo::ScatterDimensionNumbersAttr::get(
      context, get_i64_array(xla_scatter_dim.update_window_dims()),
      get_i64_array(xla_scatter_dim.inserted_window_dims()),
      get_i64_array(xla_scatter_dim.scatter_dims_to_operand_dims()),
      xla_scatter_dim.index_vector_dim());
  return scatter_dimension_numbers;
}

StatusOr<lmhlo::ScatterOp> LhloDialectEmitter::EmitScatterOp(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto scatter,
                      CreateOpWithoutAttrs<lmhlo::ScatterOp>(instr));

  // copy attributes
  auto* scatter_instr = itex_xla::Cast<itex_xla::HloScatterInstruction>(instr);

  TF_ASSIGN_OR_RETURN(auto scatter_dimension_numbers,
                      GetScatterDimensionNumbers(instr, builder_.getContext()));
  scatter.setScatterDimensionNumbersAttr(scatter_dimension_numbers);
  scatter.setIndicesAreSortedAttr(
      builder_.getBoolAttr(scatter_instr->indices_are_sorted()));
  scatter.setUniqueIndicesAttr(
      builder_.getBoolAttr(scatter_instr->unique_indices()));

  // import update computation as region
  TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
      *scatter_instr->called_computations()[0], &scatter.getUpdateComputation(),
      &builder_));

  return scatter;
}

StatusOr<lmhlo::SelectAndScatterOp> LhloDialectEmitter::EmitSelectAndScatterOp(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto select_and_scatter,
                      CreateOpWithoutAttrs<lmhlo::SelectAndScatterOp>(instr));

  // copy attributes
  auto* select_and_scatter_instr =
      itex_xla::Cast<itex_xla::HloSelectAndScatterInstruction>(instr);
  const itex_xla::Window& window = select_and_scatter_instr->window();

  if (itex_xla::window_util::HasDilation(window)) {
    return itex_xla::Unimplemented(
        "Dilation for SelectAndScatter is not supported");
  }

  select_and_scatter.setWindowDimensionsAttr(
      GetWindowElements(window, [](const itex_xla::WindowDimension& dim) {
        return static_cast<int64_t>(dim.size());
      }));
  select_and_scatter.setWindowStridesAttr(
      GetWindowElements(window, [](const itex_xla::WindowDimension& dim) {
        return static_cast<int64_t>(dim.stride());
      }));
  select_and_scatter.setPaddingAttr(
      GetWindowElements(window, [](const itex_xla::WindowDimension& dim) {
        return static_cast<int64_t>(dim.padding_low());
      }));

  // import select and scatter computation as region
  TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
      *select_and_scatter_instr->select(), &select_and_scatter.getSelect(),
      &builder_));
  TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
      *select_and_scatter_instr->scatter(), &select_and_scatter.getScatter(),
      &builder_));
  return select_and_scatter;
}

StatusOr<mlir::Operation*> LhloDialectEmitter::EmitCustomCallOp(
    const HloInstruction* instr) {
  auto* custom_call_instr =
      itex_xla::Cast<itex_xla::HloCustomCallInstruction>(instr);

  if (itex_xla::gpu::IsCustomCallToCusolver(*instr)) {
    return EmitCholesky(custom_call_instr);
  }

  if (itex_xla::gpu::IsCublasGemm(*instr)) {
    return EmitGemm(custom_call_instr);
  }
  if (itex_xla::gpu::IsCustomCallToDnnConvolution(*instr)) {
    return EmitDnnConvolution(custom_call_instr);
  }
  // For custom call, if there are any token operands or results, they will not
  // be represented in LHLO so we need to remember the mapping. First create
  // operands where each token is replaced with a null Value.
  llvm::SmallVector<Value, 4> operands;
  size_t num_arguments, num_results;
  TF_RETURN_IF_ERROR(CreateOperands(instr, /*num_operands=*/absl::nullopt,
                                    TokenLoweringMode::kUseNull, operands,
                                    num_arguments, num_results));

  // Now check if any of the operands is Null, which would indicate the presence
  // of a token in the input or output.
  bool has_token = llvm::any_of(operands, [](Value v) { return !v; });

  lmhlo::CustomCallTargetArgMappingAttr target_mapping;
  if (has_token) {
    // If there was a token, squeeze all the non-token arguments and results
    // (in-place) and remember the mapping.
    int next_index = 0;
    llvm::SmallVector<int64_t> arg_to_target_arg_mapping;
    for (int i = 0; i < num_arguments; ++i) {
      if (operands[i]) {
        arg_to_target_arg_mapping.push_back(i);
        operands[next_index++] = operands[i];
      }
    }
    // Size of arg_to_target_arg_mapping is the number of arguments in LHLO.
    llvm::SmallVector<int64_t> result_to_target_result_mapping;
    for (int i = num_arguments; i < operands.size(); ++i) {
      if (operands[i]) {
        result_to_target_result_mapping.push_back(i - num_arguments);
        operands[next_index++] = operands[i];
      }
    }

    // Build the mapping attribute.
    target_mapping = lmhlo::CustomCallTargetArgMappingAttr::get(
        builder_.getContext(), num_arguments, num_results,
        arg_to_target_arg_mapping, result_to_target_result_mapping);

    // Drop the remaining operands and adjust num_arguments and num_results
    // for LMHLO creation.
    operands.resize(next_index);
    num_arguments = arg_to_target_arg_mapping.size();
    num_results = result_to_target_result_mapping.size();
  }

  auto custom_call = CreateOpWithoutAttrs<lmhlo::CustomCallOp>(instr, operands);
  TF_ASSIGN_OR_RETURN(
      auto mlir_api_version,
      ConvertCustomCallApiVersion(custom_call_instr->api_version()));
  custom_call.setCallTargetNameAttr(
      builder_.getStringAttr(custom_call_instr->custom_call_target()));
  custom_call.setBackendConfigAttr(
      builder_.getStringAttr(custom_call_instr->opaque()));
  custom_call.setApiVersionAttr(mhlo::CustomCallApiVersionAttr::get(
      builder_.getContext(), mlir_api_version));
  const int32_t segments[2] = {static_cast<int32_t>(num_arguments),
                               static_cast<int32_t>(num_results)};
  custom_call->setAttr(lmhlo::CustomCallOp::getOperandSegmentSizeAttr(),
                       builder_.getDenseI32ArrayAttr(segments));
  if (target_mapping) custom_call.setTargetArgMappingAttr(target_mapping);
  return custom_call.getOperation();
}

StatusOr<lmhlo_gpu::CholeskyOp> LhloDialectEmitter::EmitCholesky(
    const HloCustomCallInstruction* custom_call) {
  TF_ASSIGN_OR_RETURN(auto cholesky_op,
                      CreateOpWithoutAttrs<lmhlo_gpu::CholeskyOp>(custom_call));
  TF_ASSIGN_OR_RETURN(itex_xla::CholeskyOptions options,
                      custom_call->backend_config<itex_xla::CholeskyOptions>());
  cholesky_op.setIsLowerAttr(builder_.getBoolAttr(options.lower()));
  return cholesky_op;
}

namespace {

template <typename OpT>
void SetMatmulAttributes(OpT op, const itex_xla::gpu::GemmBackendConfig& config,
                         OpBuilder& builder) {
  auto arrayref = [](absl::Span<const int64_t> array) {
    return llvm::ArrayRef<int64_t>{array.data(), array.size()};
  };

  auto hlo_dims = config.dot_dimension_numbers();
  auto mlir_dims = mhlo::DotDimensionNumbersAttr::get(
      builder.getContext(), arrayref(hlo_dims.lhs_batch_dimensions()),
      arrayref(hlo_dims.rhs_batch_dimensions()),
      arrayref(hlo_dims.lhs_contracting_dimensions()),
      arrayref(hlo_dims.rhs_contracting_dimensions()));
  op.setDotDimensionNumbersAttr(mlir_dims);
  op.setAlphaRealAttr(builder.getF64FloatAttr(config.alpha_real()));
  op.setAlphaImagAttr(builder.getF64FloatAttr(config.alpha_imag()));
  op.setBetaAttr(builder.getF64FloatAttr(config.beta()));
  if (config.algorithm_case() ==
      itex_xla::gpu::GemmBackendConfig::kSelectedAlgorithm) {
    op.setAlgorithmAttr(builder.getI64IntegerAttr(config.selected_algorithm()));
  }
}
}  // namespace

StatusOr<Operation*> LhloDialectEmitter::EmitGemm(
    const HloCustomCallInstruction* custom_call) {
  TF_ASSIGN_OR_RETURN(
      auto const config,
      custom_call->backend_config<itex_xla::gpu::GemmBackendConfig>());

  if (custom_call->operand_count() == 2) {
    TF_RET_CHECK(config.beta() == 0.);
  } else if (custom_call->operand_count() != 3) {
    return itex_xla::InvalidArgument(
        "GEMM custom call should have 2 or 3 operands");
  }

  // GEMM may have two or three operands. However, in the three operand case,
  // the third operand is updated in-place, so we treat that as an output here.
  TF_ASSIGN_OR_RETURN(
      lmhlo_gpu::GEMMOp op,
      CreateOpWithoutAttrs<lmhlo_gpu::GEMMOp>(custom_call,
                                              /*num_operands=*/2));

  SetMatmulAttributes(op, config, builder_);
  return op.getOperation();
}

static StatusOr<mlir::lmhlo_gpu::Activation> GetLHLOActivation(
    itex_xla::gpu::ActivationMode activation) {
  switch (activation) {
    case itex_xla::gpu::ActivationMode::kNone:
      return mlir::lmhlo_gpu::Activation::None;
    case itex_xla::gpu::ActivationMode::kSigmoid:
      return mlir::lmhlo_gpu::Activation::Sigmoid;
    case itex_xla::gpu::ActivationMode::kRelu:
      return mlir::lmhlo_gpu::Activation::Relu;
    case itex_xla::gpu::ActivationMode::kRelu6:
      return mlir::lmhlo_gpu::Activation::Relu6;
    case itex_xla::gpu::ActivationMode::kReluX:
      return mlir::lmhlo_gpu::Activation::ReluX;
    case itex_xla::gpu::ActivationMode::kTanh:
      return mlir::lmhlo_gpu::Activation::Tanh;
    case itex_xla::gpu::ActivationMode::kBandPass:
      return mlir::lmhlo_gpu::Activation::BandPass;
    default:
      return itex_xla::InternalError("Unknown activation");
  }
}

StatusOr<Operation*> LhloDialectEmitter::EmitDnnConvolution(
    const HloCustomCallInstruction* custom_call) {
  TF_ASSIGN_OR_RETURN(
      auto const backend_config,
      custom_call->backend_config<itex_xla::gpu::CudnnConvBackendConfig>());

  TF_ASSIGN_OR_RETURN(const itex_xla::gpu::CudnnConvKind kind,
                      itex_xla::gpu::GetCudnnConvKind(custom_call));

  auto get_layout_attribute = [&](const itex_xla::Layout& layout) {
    std::vector<int64_t> minor_to_major(layout.minor_to_major_size());
    absl::c_transform(layout.minor_to_major(), minor_to_major.begin(),
                      [](int64_t x) { return static_cast<int64_t>(x); });
    return minor_to_major;
  };

  auto set_common_conv_attributes = [&, this](auto op) -> Operation* {
    const itex_xla::Window& window = custom_call->window();
    // Window size for Cudnn Conv is same as the kernel size.
    NamedAttrList attrs(op->getAttrDictionary());
    DenseIntElementsAttr window_strides;
    attrs.set(op.getWindowStridesAttrName(),
              window_strides = GetWindowElements(
                  window, [](const itex_xla::WindowDimension& dim) {
                    return static_cast<int64_t>(dim.stride());
                  }));
    // Cudnn Conv requires low and high padding to be equal.
    attrs.set(
        op.getPaddingAttrName(),
        GetWindowElements(window, [](const itex_xla::WindowDimension& dim) {
          return static_cast<int64_t>(dim.padding_low());
        }));
    // LHS dilation is encoded in base_dilation of the backend config.
    // RHS dilation is encoded in window_dilation of the backend config.
    attrs.set(
        op.getLhsDilationAttrName(),
        GetWindowElements(window, [](const itex_xla::WindowDimension& dim) {
          return static_cast<int64_t>(dim.base_dilation());
        }));
    attrs.set(
        op.getRhsDilationAttrName(),
        GetWindowElements(window, [](const itex_xla::WindowDimension& dim) {
          return static_cast<int64_t>(dim.window_dilation());
        }));
    // Setup window reversal.
    auto window_reversal = llvm::to_vector<4>(llvm::map_range(
        window.dimensions(), [](const itex_xla::WindowDimension& dim) {
          return dim.window_reversal();
        }));
    auto type = RankedTensorType::get(window_strides.getType().getShape(),
                                      builder_.getIntegerType(/*width=*/1));
    attrs.set(op.getWindowReversalAttrName(),
              DenseElementsAttr::get(type, window_reversal));

    attrs.set(op.getDimensionNumbersAttrName(),
              itex_xla::ConvertConvDimensionNumbers(
                  custom_call->convolution_dimension_numbers(), &builder_));
    attrs.set(op.getFeatureGroupCountAttrName(),
              builder_.getI64IntegerAttr(custom_call->feature_group_count()));
    attrs.set(op.getBatchGroupCountAttrName(),
              builder_.getI64IntegerAttr(custom_call->batch_group_count()));
    attrs.set(op.getPrecisionConfigAttrName(),
              itex_xla::ConvertPrecisionConfig(&custom_call->precision_config(),
                                               &builder_));
    attrs.set(op.getResultScaleAttrName(),
              builder_.getF64FloatAttr(backend_config.conv_result_scale()));

    auto config = mlir::lmhlo_gpu::ConvolutionBackendConfigAttr::get(
        builder_.getContext(),
        0,  // algorithm.algo_id(),
        false, std::vector<int64_t>(0), std::vector<int64_t>(0), false,

        // algorithm.math_type() ==
        //  stream_executor::dnn::AlgorithmProto::TENSOR_OP_MATH,
        // knob_ids, knob_values, algorithm.is_cudnn_frontend(),
        0,  // algorithm.has_workspace_size() ?
            // algorithm.workspace_size().value()
            //                             : -1,
        get_layout_attribute(custom_call->operand(0)->shape().layout()),
        get_layout_attribute(custom_call->operand(1)->shape().layout()),
        get_layout_attribute(custom_call->shape().tuple_shapes(0).layout()));
    attrs.set(op.getBackendConfigAttrName(), config);
    op->setAttrs(attrs.getDictionary(op->getContext()));

    return op.getOperation();
  };
  auto set_activation = [&, this](auto op) -> Status {
    auto se_activation = static_cast<itex_xla::gpu::ActivationMode>(
        backend_config.activation_mode());

    TF_ASSIGN_OR_RETURN(mlir::lmhlo_gpu::Activation activation,
                        GetLHLOActivation(se_activation));
    auto activation_attr = ::mlir::lmhlo_gpu::ActivationAttr::get(
        getLocation(custom_call).getContext(), activation);
    op.setActivationModeAttr(activation_attr);
    return Status::OK();
  };

  switch (kind) {
    case itex_xla::gpu::CudnnConvKind::kForward: {
      TF_ASSIGN_OR_RETURN(
          auto cnn_forward,
          CreateOpWithoutAttrs<lmhlo_gpu::ConvForwardOp>(custom_call));
      return set_common_conv_attributes(cnn_forward);
    }
    case itex_xla::gpu::CudnnConvKind::kBackwardInput: {
      TF_ASSIGN_OR_RETURN(
          auto cnn_backward,
          CreateOpWithoutAttrs<lmhlo_gpu::ConvBackwardInputOp>(custom_call));
      return set_common_conv_attributes(cnn_backward);
    }
    case itex_xla::gpu::CudnnConvKind::kBackwardFilter: {
      TF_ASSIGN_OR_RETURN(
          auto cnn_backward,
          CreateOpWithoutAttrs<lmhlo_gpu::ConvBackwardFilterOp>(custom_call));
      return set_common_conv_attributes(cnn_backward);
    }
    case itex_xla::gpu::CudnnConvKind::kForwardActivation: {
      // Fused conv can be either with side input or without.
      if (custom_call->operand_count() == 3) {
        TF_ASSIGN_OR_RETURN(
            auto cnn_fused,
            CreateOpWithoutAttrs<lmhlo_gpu::ConvForwardFusedOp>(custom_call));
        TF_RETURN_IF_ERROR(set_activation(cnn_fused));
        return set_common_conv_attributes(cnn_fused);
      }

      TF_RET_CHECK(custom_call->operand_count() == 4);
      TF_ASSIGN_OR_RETURN(
          auto cnn_fused_side_input,
          CreateOpWithoutAttrs<lmhlo_gpu::ConvForwardFusedSideInputOp>(
              custom_call));
      cnn_fused_side_input.setSideInputScaleAttr(
          builder_.getF64FloatAttr(backend_config.side_input_scale()));
      TF_RETURN_IF_ERROR(set_activation(cnn_fused_side_input));
      return set_common_conv_attributes(cnn_fused_side_input);
    }
  }
}

// Convert an XLA HLO constant to a global_memref + get_global_memref pair.
StatusOr<mlir::memref::GetGlobalOp> LhloDialectEmitter::EmitConstant(
    const HloInstruction* instr) {
  auto& cached_value = slices_[std::make_pair(instr, itex_xla::ShapeIndex())];
  if (cached_value) {
    return dyn_cast<mlir::memref::GetGlobalOp>(cached_value.getDefiningOp());
  }

  // Insert a global_memref in the module.
  Location loc = getLocation(instr);

  auto const_instr = itex_xla::Cast<itex_xla::HloConstantInstruction>(instr);
  TF_RET_CHECK(const_instr->shape().IsArray() &&
               const_instr->shape().is_static());
  TF_ASSIGN_OR_RETURN(Type type, itex_xla::ConvertShapeToType<MemRefType>(
                                     const_instr->shape(), builder_));
  auto memref_type = type.dyn_cast<MemRefType>();
  TF_RET_CHECK(memref_type != nullptr);

  TF_ASSIGN_OR_RETURN(
      DenseElementsAttr initial_value,
      CreateDenseElementsAttrFromLiteral(const_instr->literal(), builder_));

  std::string constant_name = itex_xla::llvm_ir::ConstantNameToGlobalName(
      itex_xla::llvm_ir::SanitizeConstantName(instr->name()));

  // Insert the global memref at the top level.
  {
    OpBuilder::InsertionGuard guard(builder_);
    builder_.clearInsertionPoint();
    auto global_var = builder_.create<memref::GlobalOp>(
        loc, constant_name, builder_.getStringAttr("private"), memref_type,
        initial_value, true, /*alignment=*/IntegerAttr());
    SymbolTable(module_).insert(global_var);
    global_var.getOperation()->moveBefore(&module_.front());

    // For operations that do not fold this constant value in their codegen, we
    // still need to materialize it into a buffer. Since buffer allocation is
    // already done, annotate the global_memref with the information to get to
    // the allocated buffer slice for this constant if need be.
    TF_ASSIGN_OR_RETURN(BufferAllocation::Slice slice,
                        assignment_.GetUniqueTopLevelSlice(instr));
    global_var->setAttr(
        "lmhlo.alloc",
        builder_.getIndexAttr(allocations_.find(slice.allocation())
                                  ->second.cast<BlockArgument>()
                                  .getArgNumber()));
    TF_RET_CHECK(slice.offset() == 0)
        << "Each constant should have its own allocation from BufferAssignment";
    TF_RET_CHECK(slice.allocation()->size() == slice.size())
        << "Each constant should have its own allocation from BufferAssignment";
  }

  auto get_global_memref =
      builder_.create<memref::GetGlobalOp>(loc, memref_type, constant_name);

  // Update the cache to remember this value.
  cached_value = get_global_memref;
  return get_global_memref;
}

namespace {
template <typename OpT>
void SetupChannelIdAttribute(OpT op,
                             const itex_xla::HloChannelInstruction* instr,
                             mlir::Builder builder) {
  if (instr->channel_id().has_value()) {
    op.setChannelIdAttr(mlir::mhlo::ChannelHandleAttr::get(
        builder.getContext(), *instr->channel_id(), 0));
  }
}

template <typename OpT>
Status SetupCommonCollectiveOpAttributes(OpT op, const HloInstruction* instr,
                                         mlir::OpBuilder& builder) {
  auto* collective = itex_xla::Cast<itex_xla::HloCollectiveInstruction>(instr);
  auto replica_groups_attr =
      itex_xla::HloFunctionImporter::ConvertReplicaGroups(
          collective->replica_groups(), &builder);
  op->setAttr(replica_groups_attr.getName(), replica_groups_attr.getValue());
  op.setConstrainLayoutAttr(
      builder.getBoolAttr(collective->constrain_layout()));
  SetupChannelIdAttribute(op, collective, builder);
  return Status::OK();
}
}  // namespace

StatusOr<lmhlo::AllToAllOp> LhloDialectEmitter::EmitAllToAllOp(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto all_to_all_op,
                      CreateOpWithoutAttrs<lmhlo::AllToAllOp>(instr));
  auto* all_to_all = itex_xla::Cast<itex_xla::HloAllToAllInstruction>(instr);
  TF_RETURN_IF_ERROR(
      SetupCommonCollectiveOpAttributes(all_to_all_op, instr, builder_));
  if (all_to_all->split_dimension().has_value()) {
    all_to_all_op.setSplitDimensionAttr(
        builder_.getI64IntegerAttr(*all_to_all->split_dimension()));
  }
  return all_to_all_op;
}

StatusOr<lmhlo::AllGatherOp> LhloDialectEmitter::EmitAllGatherOp(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto all_gather_op,
                      CreateOpWithoutAttrs<lmhlo::AllGatherOp>(instr));
  auto* all_gather = itex_xla::Cast<itex_xla::HloAllGatherInstruction>(instr);
  TF_RETURN_IF_ERROR(
      SetupCommonCollectiveOpAttributes(all_gather_op, instr, builder_));
  all_gather_op.setUseGlobalDeviceIdsAttr(
      builder_.getBoolAttr(all_gather->use_global_device_ids()));
  all_gather_op.setAllGatherDimensionAttr(
      builder_.getI64IntegerAttr(all_gather->all_gather_dimension()));
  return all_gather_op;
}

StatusOr<lmhlo::AllReduceOp> LhloDialectEmitter::EmitAllReduceOp(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto all_reduce_op,
                      CreateOpWithoutAttrs<lmhlo::AllReduceOp>(instr));
  auto* all_reduce = itex_xla::Cast<itex_xla::HloAllReduceInstruction>(instr);
  TF_RETURN_IF_ERROR(
      SetupCommonCollectiveOpAttributes(all_reduce_op, instr, builder_));
  all_reduce_op.setUseGlobalDeviceIdsAttr(
      builder_.getBoolAttr(all_reduce->use_global_device_ids()));
  TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
      *instr->called_computations()[0], &all_reduce_op.getComputation(),
      &builder_));
  return all_reduce_op;
}

StatusOr<lmhlo_gpu::AllReduceStartOp> LhloDialectEmitter::EmitAllReduceStartOp(
    const HloInstruction* instr) {
  llvm::SmallVector<Value, 4> operands;
  for (const HloInstruction* operand : instr->operands()) {
    TF_RETURN_IF_ERROR(GetOrCreateView(operand, &operands));
  }
  TF_RETURN_IF_ERROR(GetOrCreateView(instr, &operands, /*result_subset=*/{}));

  Location loc = getLocation(instr);
  mlir::Type token_type = mlir::mhlo::TokenType::get(builder_.getContext());
  std::array<mlir::Type, 1> result_types = {token_type};
  lmhlo_gpu::AllReduceStartOp all_reduce_start_op =
      builder_.create<lmhlo_gpu::AllReduceStartOp>(loc, result_types, operands);

  auto* all_reduce = itex_xla::Cast<itex_xla::HloAllReduceInstruction>(instr);
  TF_RETURN_IF_ERROR(
      SetupCommonCollectiveOpAttributes(all_reduce_start_op, instr, builder_));
  all_reduce_start_op.setUseGlobalDeviceIdsAttr(
      builder_.getBoolAttr(all_reduce->use_global_device_ids()));
  TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
      *instr->called_computations()[0], &all_reduce_start_op.getComputation(),
      &builder_));

  TF_RET_CHECK(all_reduce_start_ops_.emplace(instr, all_reduce_start_op).second)
      << "all-reduce-start already lowered";
  return all_reduce_start_op;
}

StatusOr<lmhlo_gpu::AllReduceDoneOp> LhloDialectEmitter::EmitAllReduceDoneOp(
    const HloInstruction* instr) {
  auto it = all_reduce_start_ops_.find(instr->operand(0));
  TF_RET_CHECK(it != all_reduce_start_ops_.end())
      << "didn't find all-reduce-start op";

  llvm::SmallVector<Value, 4> operands;
  operands.push_back(it->second.getToken());
  all_reduce_start_ops_.erase(it);

  for (const HloInstruction* operand : instr->operands()) {
    TF_RETURN_IF_ERROR(GetOrCreateView(operand, &operands));
  }
  // We don't need to add buffers for the outputs, as these always alias inputs.
  return builder_.create<lmhlo_gpu::AllReduceDoneOp>(
      getLocation(instr), /*resultTypes=*/llvm::None, operands);
}

StatusOr<lmhlo::ReduceScatterOp> LhloDialectEmitter::EmitReduceScatterOp(
    const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto reduce_scatter_op,
                      CreateOpWithoutAttrs<lmhlo::ReduceScatterOp>(instr));
  auto* ars = itex_xla::Cast<itex_xla::HloReduceScatterInstruction>(instr);
  TF_RETURN_IF_ERROR(
      SetupCommonCollectiveOpAttributes(reduce_scatter_op, instr, builder_));
  reduce_scatter_op.setUseGlobalDeviceIdsAttr(
      builder_.getBoolAttr(ars->use_global_device_ids()));
  TF_RETURN_IF_ERROR(itex_xla::HloFunctionImporter::ImportAsRegion(
      *instr->called_computations()[0], &reduce_scatter_op.getComputation(),
      &builder_));
  reduce_scatter_op.setScatterDimensionAttr(
      builder_.getI64IntegerAttr(ars->scatter_dimension()));
  return reduce_scatter_op;
}

StatusOr<lmhlo::CollectivePermuteOp>
LhloDialectEmitter::EmitCollectivePermuteOp(const HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(auto permute_op,
                      CreateOpWithoutAttrs<lmhlo::CollectivePermuteOp>(instr));
  auto* permute =
      itex_xla::Cast<itex_xla::HloCollectivePermuteInstruction>(instr);
  SetupChannelIdAttribute(permute_op, permute, builder_);
  mlir::NamedAttribute source_target_pairs_attr =
      itex_xla::HloFunctionImporter::ConvertSourceTargetPairs(
          permute->source_target_pairs(), &builder_);
  permute_op->setAttr(source_target_pairs_attr.getName(),
                      source_target_pairs_attr.getValue());
  return permute_op;
}

StatusOr<lmhlo::InfeedOp> LhloDialectEmitter::EmitInfeedOp(
    const HloInstruction* instr) {
  const HloInfeedInstruction* infeed =
      itex_xla::Cast<HloInfeedInstruction>(instr);
  // HLO Infeed instruction has a single operand of token type and a tuple
  // with buffers and a token as its output. LMHLO Infeed operation does not
  // need the token operand or result, so drop it.
  SmallVector<Value, 2> operands;
  TF_RETURN_IF_ERROR(GetOrCreateView(instr, &operands, /*result_subset=*/{0}));
  auto infeed_op = CreateOpWithoutAttrs<lmhlo::InfeedOp>(instr, operands);
  infeed_op.setConfigAttr(builder_.getStringAttr(infeed->infeed_config()));
  return infeed_op;
}

StatusOr<lmhlo::OutfeedOp> LhloDialectEmitter::EmitOutfeedOp(
    const HloInstruction* instr) {
  const HloOutfeedInstruction* outfeed =
      itex_xla::Cast<HloOutfeedInstruction>(instr);
  // HLO outfeed instruction has 2 operands, the source and a token, and a
  // single token output. LMHLO Outfeed does not need the token operand and
  // result, do drop it.
  SmallVector<Value, 2> operands;
  TF_RETURN_IF_ERROR(GetOrCreateView(instr->operand(0), &operands));
  auto outfeed_op = CreateOpWithoutAttrs<lmhlo::OutfeedOp>(instr, operands);
  outfeed_op.setConfigAttr(builder_.getStringAttr(outfeed->outfeed_config()));
  return outfeed_op;
}

itex_xla::StatusOr<lmhlo::RngGetAndUpdateStateOp>
LhloDialectEmitter::EmitRngGetAndUpdateStateOp(
    const itex_xla::HloInstruction* instr) {
  TF_ASSIGN_OR_RETURN(
      auto rng, CreateOpWithoutAttrs<lmhlo::RngGetAndUpdateStateOp>(instr));
  auto hlo_rng =
      itex_xla::Cast<itex_xla::HloRngGetAndUpdateStateInstruction>(instr);
  rng.setDeltaAttr(builder_.getI64IntegerAttr(hlo_rng->delta()));
  return rng;
}

itex_xla::StatusOr<lmhlo::FftOp> LhloDialectEmitter::EmitFftOp(
    const HloInstruction* instr) {
  auto hlo_fft = itex_xla::Cast<itex_xla::HloFftInstruction>(instr);
  TF_ASSIGN_OR_RETURN(auto fft, CreateOpWithoutAttrs<lmhlo::FftOp>(instr));
  TF_ASSIGN_OR_RETURN(mlir::mhlo::FftType fft_type,
                      itex_xla::ConvertFftType(hlo_fft->fft_type()));
  fft.setFftTypeAttr(
      mlir::mhlo::FftTypeAttr::get(builder_.getContext(), fft_type));
  fft.setFftLengthAttr(GetI64DenseElementsAttr(instr->fft_length()));
  return fft;
}

itex_xla::StatusOr<lmhlo::TriangularSolveOp>
LhloDialectEmitter::EmitTriangularSolveOp(
    const itex_xla::HloInstruction* instr) {
  auto hlo_triangular_solve =
      itex_xla::Cast<itex_xla::HloTriangularSolveInstruction>(instr);
  TF_ASSIGN_OR_RETURN(auto triangular_solve,
                      CreateOpWithoutAttrs<lmhlo::TriangularSolveOp>(instr));
  const itex_xla::TriangularSolveOptions& options =
      hlo_triangular_solve->triangular_solve_options();
  triangular_solve.setLeftSideAttr(builder_.getBoolAttr(options.left_side()));
  triangular_solve.setLowerAttr(builder_.getBoolAttr(options.lower()));
  triangular_solve.setUnitDiagonalAttr(
      builder_.getBoolAttr(options.unit_diagonal()));
  TF_ASSIGN_OR_RETURN(mlir::mhlo::Transpose transpose,
                      itex_xla::ConvertTranspose(options.transpose_a()));
  triangular_solve.setTransposeAAttr(
      mlir::mhlo::TransposeAttr::get(builder_.getContext(), transpose));
  triangular_solve.setLayoutAAttr(
      GetLayoutAttribute(instr->operand(0)->shape().layout(), &builder_));
  triangular_solve.setLayoutBAttr(
      GetLayoutAttribute(instr->operand(1)->shape().layout(), &builder_));
  triangular_solve.setLayoutOutputAttr(
      GetLayoutAttribute(instr->shape().layout(), &builder_));
  return triangular_solve;
}

itex_xla::StatusOr<Operation*> LhloDialectEmitter::EmitBitcast(
    const itex_xla::HloInstruction* instr) {
  // XLA buffer assignment should assign the same slice to a bitcast input and
  // output.
  const itex_xla::ShapeIndex top_index;
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice result_slice,
                      assignment_.GetUniqueSlice(instr, top_index));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice input_slice,
                      assignment_.GetUniqueSlice(instr->operand(0), top_index));

  if (input_slice != result_slice) {
    return itex_xla::InvalidArgument(
        "Bitcast input and result slice should be same");
  }
  return nullptr;
}

mlir::DenseIntElementsAttr LhloDialectEmitter::GetLayoutAttribute(
    const itex_xla::Layout& layout, Builder* builder) {
  llvm::SmallVector<int64_t, 4> minor_to_major(layout.minor_to_major().begin(),
                                               layout.minor_to_major().end());
  return builder->getIndexTensorAttr(minor_to_major);
}

Status LhloDialectEmitter::ImportAsLmhloRegion(
    itex_xla::HloComputation* computation, mlir::Region* region) {
  auto after = builder_.saveInsertionPoint();
  auto reverter = absl::MakeCleanup(
      [this, after] { builder_.restoreInsertionPoint(after); });

  builder_ = OpBuilder(region);
  const itex_xla::HloInstructionSequence* schedule =
      assignment_.hlo_ordering().SequentialOrder(*computation);
  if (!schedule)
    return itex_xla::Unimplemented(
        "Missing sequential order for the computation");
  TF_RETURN_IF_ERROR(
      computation->AcceptOrdered(this, schedule->instructions()));
  builder_.create<lmhlo::TerminatorOp>(builder_.getUnknownLoc());
  return Status::OK();
}

StatusOr<lmhlo::CaseOp> LhloDialectEmitter::EmitCaseOp(
    const HloInstruction* instr) {
  Location loc = getLocation(instr);
  llvm::SmallVector<Value, 4> operands;
  size_t num_arguments, num_results;
  TF_RETURN_IF_ERROR(CreateOperands(instr, 1, TokenLoweringMode::kUseNull,
                                    operands, num_arguments, num_results));

  auto case_op =
      builder_.create<lmhlo::CaseOp>(loc, operands[0], instr->branch_count());

  for (int i = 0; i < instr->branch_count(); i++) {
    case_op.getBranches()[i].push_back(new mlir::Block());
    TF_RETURN_IF_ERROR(ImportAsLmhloRegion(instr->called_computations()[i],
                                           &case_op.getBranches()[i]));
  }

  return case_op;
}

itex_xla::StatusOr<lmhlo::WhileOp> LhloDialectEmitter::EmitWhileOp(
    const itex_xla::HloInstruction* instr) {
  Location loc = getLocation(instr);
  SmallVector<Value, 1> operands;
  TF_RETURN_IF_ERROR(GetOrCreateView(
      instr->called_computations()[1]->root_instruction(), &operands));
  TF_RET_CHECK(operands.size() == 1);

  TF_ASSIGN_OR_RETURN(
      auto config, instr->backend_config<itex_xla::WhileLoopBackendConfig>());
  mlir::IntegerAttr trip_count;
  if (config.has_known_trip_count()) {
    trip_count = builder_.getI64IntegerAttr(config.known_trip_count().n());
  }
  lmhlo::WhileOp while_op =
      builder_.create<lmhlo::WhileOp>(loc, operands[0], trip_count);

  while_op.getCond().push_back(new mlir::Block());
  while_op.getBody().push_back(new mlir::Block());
  TF_RETURN_IF_ERROR(ImportAsLmhloRegion(instr->called_computations()[1],
                                         &while_op.getCond()));

  TF_RETURN_IF_ERROR(ImportAsLmhloRegion(instr->called_computations()[0],
                                         &while_op.getBody()));

  return while_op;
}

StatusOr<Value> LhloDialectEmitter::GetOrCreateArrayView(
    const itex_xla::HloInstruction* instr, const itex_xla::Shape& current_shape,
    const itex_xla::ShapeIndex& shape_index) {
  // For constants, the cache is managed inside EmitConstant since it can
  // be called either from here or when we see a top-level HloConstant instr.
  if (instr->IsConstant() && shape_index.empty()) {
    TF_ASSIGN_OR_RETURN(Value constant_memref, EmitConstant(instr));
    return constant_memref;
  }

  // Cache generated ViewOp and StaticMemRefCastOp by (instruction,
  // shape_index).
  auto& cached_value = slices_[std::make_pair(instr, shape_index)];
  if (cached_value) {
    return cached_value;
  }

  // If the shape happens to have dynamic dimensions, create the memref using
  // the underlying static shape.
  // TODO(jurahul): Revisit this when we can model memrefs with dynamic shape
  // but static bounds in MLIR.
  const Shape static_shape =
      itex_xla::ShapeUtil::MakeStaticShape(current_shape);

  TF_ASSIGN_OR_RETURN(Type out_type, itex_xla::ConvertShapeToType<MemRefType>(
                                         static_shape, builder_));
  TF_ASSIGN_OR_RETURN(BufferAllocation::Slice slice,
                      assignment_.GetUniqueSlice(instr, shape_index));
  Value alloc = allocations_[slice.allocation()];

  // TODO(timshen): revisit location handling.
  Location loc = builder_.getUnknownLoc();

  Value byte_shift =
      builder_.create<arith::ConstantIndexOp>(alloc.getLoc(), slice.offset());

  itex_xla::Shape physical_shape =
      itex_xla::ShapeUtil::MakeShapeWithDescendingLayoutAndSamePhysicalLayout(
          static_shape);
  TF_ASSIGN_OR_RETURN(
      Type physical_out_type,
      itex_xla::ConvertShapeToType<MemRefType>(physical_shape, builder_));

  // ViewOp only takes memrefs without affine maps (layouts). Let ViewOp
  // produce the physical shape (where dimensions are ordered in major to
  // minor) first, then follow up with a MemRefReinterpretCast to cast the
  // resulting memref to the original layout.
  Value result =
      builder_.create<memref::ViewOp>(loc, physical_out_type, alloc, byte_shift,
                                      /*sizes=*/ValueRange{});
  if (result.getType() != out_type) {
    int64_t out_offset;
    SmallVector<int64_t, 4> out_strides;
    auto out_memref_type = out_type.dyn_cast<MemRefType>();
    if (!out_memref_type)
      return itex::errors::Internal(
          "Expected memref type when creating a view for leaf type of a "
          "tuple.");
    if (failed(getStridesAndOffset(out_memref_type, out_strides, out_offset)))
      return itex::errors::Internal(
          "Failed to get strides and offset from the output type.");
    result = builder_.create<memref::ReinterpretCastOp>(
        loc, out_memref_type, result, out_offset, out_memref_type.getShape(),
        out_strides);
  }
  return cached_value = result;
}

Status LhloDialectEmitter::GetOrCreateViewImpl(
    const HloInstruction* instr, const Shape& current_shape,
    itex_xla::ShapeIndex* current_shape_index, SmallVectorImpl<Value>* values,
    TokenLoweringMode token_mode) {
  if (current_shape.IsTuple()) {
    for (int i = 0; i < current_shape.tuple_shapes().size(); ++i) {
      current_shape_index->push_back(i);
      TF_RETURN_IF_ERROR(
          GetOrCreateViewImpl(instr, current_shape.tuple_shapes(i),
                              current_shape_index, values, token_mode));
      current_shape_index->pop_back();
    }
    return Status::OK();
  }
  if (current_shape.IsArray()) {
    TF_ASSIGN_OR_RETURN(auto v, GetOrCreateArrayView(instr, current_shape,
                                                     *current_shape_index));
    values->push_back(v);
    return Status::OK();
  }
  if (current_shape.IsToken()) {
    switch (token_mode) {
      case TokenLoweringMode::kFailToLower:
        return itex_xla::InternalError(
            "Unexpected token kind for %s and shape index %s",
            instr->ToString(), current_shape_index->ToString());

      case TokenLoweringMode::kUseNull:
        values->push_back(Value{});
        return Status::OK();
    }
  }
  return itex_xla::InternalError(
      "Unexpected shape kind for %s and shape index %s", instr->ToString(),
      current_shape_index->ToString());
}

// Returns a view for the result of an instruction.
// We first get a view for the slice in the allocation, and then may need to
// create another view to adjust the slice for the shape of the instruction.
Status LhloDialectEmitter::GetOrCreateView(
    const HloInstruction* instr, SmallVectorImpl<Value>* values,
    const itex_xla::ShapeIndex& result_subset, TokenLoweringMode token_mode) {
  itex_xla::ShapeIndex shape_index = result_subset;
  const Shape& sub_shape =
      itex_xla::ShapeUtil::GetSubshape(instr->shape(), shape_index);
  return GetOrCreateViewImpl(instr, sub_shape, &shape_index, values,
                             token_mode);
}

Status LhloDialectEmitter::Initialize() {
  TF_RET_CHECK(computation_.IsEntryComputation());

  mlir::IntegerAttr unique_id =
      builder_.getI32IntegerAttr(computation_.parent()->unique_id());
  module_->setAttr("hlo.unique_id", unique_id);
  std::string function_name =
      computation_.name().empty() ? "__compute" : computation_.name();

  // Create the function as () -> (), we'll compute the arguments from the
  // buffer allocation and update the type then.
  auto func_op = func::FuncOp::create(builder_.getUnknownLoc(), function_name,
                                      builder_.getFunctionType({}, {}));

  {
    // This is an optional attribute used by the XLA backend. If the resulting
    // LMHLO doesn't go through XLA, this is not needed.
    const Shape& shape = computation_.root_instruction()->shape();
    func_op->setAttr(
        "result_xla_shape",
        builder_.getStringAttr(shape.ToString(/*print_layout=*/true)));
  }
  Block* block = func_op.addEntryBlock();

  llvm::SmallVector<const BufferAllocation*, 8> ordered_allocations;
  for (const BufferAllocation& alloc : assignment_.Allocations())
    ordered_allocations.push_back(&alloc);

  if (computation_.IsEntryComputation()) {
    // Sort the rather arbitrarily ordered allocations to match the input/output
    // parameters. Specifically we want to sort buffer allocations in the
    // following order:
    // * Parameters always order before non-parameters.
    // * Different parameters order by parameter number.
    // * Different allocations for the same parameter order by the shape index.
    //
    // TODO(timshen): there should be only one non-parameter buffer, the temp
    // buffer. Check on that.
    const auto allocation_comparator = [](const BufferAllocation* lhs,
                                          const BufferAllocation* rhs) {
      if (lhs->is_entry_computation_parameter() !=
          rhs->is_entry_computation_parameter()) {
        return lhs->is_entry_computation_parameter() >
               rhs->is_entry_computation_parameter();
      }
      if (lhs->is_entry_computation_parameter()) {
        return std::tuple<int, const itex_xla::ShapeIndex&>(
                   lhs->parameter_number(), lhs->param_shape_index()) <
               std::tuple<int, const itex_xla::ShapeIndex&>(
                   rhs->parameter_number(), rhs->param_shape_index());
      }
      return false;
    };

    std::stable_sort(ordered_allocations.begin(), ordered_allocations.end(),
                     allocation_comparator);
  }

  absl::flat_hash_map<const BufferAllocation*,
                      std::pair<const Shape*, itex_xla::ShapeIndex>>
      allocation_to_output_info;
  TF_RETURN_IF_ERROR(itex_xla::ShapeUtil::ForEachSubshapeWithStatus(
      computation_.root_instruction()->shape(),
      [&](const Shape& sub_shape, itex_xla::ShapeIndex index) -> Status {
        TF_ASSIGN_OR_RETURN(
            auto slice,
            assignment_.GetUniqueSlice(computation_.root_instruction(), index));
        const BufferAllocation* alloc = slice.allocation();
        TF_RET_CHECK(slice.offset() == 0);
        TF_RET_CHECK(slice.size() == alloc->size());
        allocation_to_output_info[alloc] = std::make_pair(&sub_shape, index);
        return Status::OK();
      }));

  // The function signature will be composed of:
  // - one memref for each of the parameters.
  // - one memref for each other buffer allocation.
  llvm::SmallVector<DictionaryAttr, 8> args_attrs;
  for (const BufferAllocation* alloc : ordered_allocations) {
    if (alloc->is_thread_local()) {
      continue;
    }

    // There are optional attributes to help the program run through XLA. XLA
    // defines ExecutionInput and ExecutionOutput structures to carry
    // input-output type and buffer information, therefore any information they
    // need (mainly the type structure, potentially containing tuples) to be
    // preserved. They are not needed if the generated LMHLO is not sent to XLA.
    NamedAttrList arg_attr_list;
    mlir::Type arg_type = MemRefType::get({alloc->size()}, i8_type_);

    // Propagate source location information for every HLOInstruction that
    // uses this allocation.
    std::vector<mlir::Location> buf_locs;
    buf_locs.reserve(alloc->assigned_buffers().size());
    for (const auto& entry : alloc->assigned_buffers()) {
      const itex_xla::HloValue* hlo_value = entry.first;
      buf_locs.push_back(getLocation(hlo_value->instruction()));
    }
    mlir::Location loc = builder_.getFusedLoc(buf_locs);

    if (alloc->is_entry_computation_parameter()) {
      arg_attr_list.set("lmhlo.params",
                        builder_.getIndexAttr(alloc->parameter_number()));
      if (!alloc->param_shape_index().empty()) {
        arg_attr_list.set("lmhlo.param_shape_index",
                          builder_.getI64TensorAttr(llvm::makeArrayRef(
                              alloc->param_shape_index().begin(),
                              alloc->param_shape_index().end())));
      }
    }
    // Optional: an attribute for optimization. If a kernel uses this
    // allocation, but the allocation has lmhlo.constant_name, then the kernel
    // will instead use the global value indicated by the name for potentially
    // more optimizations (e.g. constant propagation).
    if (alloc->is_constant()) {
      arg_attr_list.set(
          "lmhlo.constant_name",
          builder_.getStringAttr(
              itex_xla::llvm_ir::ConstantBufferAllocationToGlobalName(*alloc)));
    }
    auto iter = allocation_to_output_info.find(alloc);
    if (iter != allocation_to_output_info.end()) {
      const Shape* sub_shape = iter->second.first;
      const itex_xla::ShapeIndex& shape_index = iter->second.second;
      if (!sub_shape->IsArray()) {
        continue;
      }
      arg_attr_list.set("lmhlo.output_index",
                        builder_.getI64TensorAttr(llvm::makeArrayRef(
                            shape_index.begin(), shape_index.end())));
      if (auto alias = computation_.parent()
                           ->input_output_alias_config()
                           .GetAliasedParameter(shape_index)) {
        if (alias->must_alias()) {
          arg_attr_list.set("lmhlo.must_alias", builder_.getUnitAttr());
        }
      }
    }
    block->addArgument(arg_type, loc);
    allocations_[alloc] = block->getArguments().back();
    args_attrs.push_back(arg_attr_list.getDictionary(builder_.getContext()));
  }

  FunctionType function_type =
      builder_.getFunctionType(block->getArgumentTypes(), {});
  func_op.setType(function_type);
  func_op.setAllArgAttrs(args_attrs);

  SymbolTable symbol_table(module_);
  symbol_table.insert(func_op);
  builder_.setInsertionPointToEnd(block);

  auto return_op =
      builder_.create<lmhlo::TerminatorOp>(builder_.getUnknownLoc());
  builder_ = OpBuilder(return_op);

  return Status::OK();
}

// std::unique_ptr<OperationPass<ModuleOp>> createXlaHloToLhloWithXlaPass() {
//   return std::make_unique<XlaHloToLhloPass>();
// }

Status HloToLhloModule(const BufferAssignment& assignment,
                       const HloModule& hlo_module, ModuleOp module) {
  module.getContext()
      ->loadDialect<arith::ArithDialect, bufferization::BufferizationDialect,
                    func::FuncDialect, memref::MemRefDialect, mhlo::MhloDialect,
                    lmhlo::LmhloDialect, lmhlo_gpu::LmhloGpuDialect>();

  module->setLoc(mlir::NameLoc::get(
      mlir::StringAttr::get(module.getContext(), hlo_module.name())));

  // Store the HloModule's unique_id in the MLIR module.
  Builder builder(module.getContext());
  module->setAttr("mhlo.unique_id",
                  builder.getI64IntegerAttr(hlo_module.unique_id()));

  const HloComputation* computation = hlo_module.entry_computation();

  LhloDialectEmitter emitter(assignment, *computation, module);
  TF_RETURN_IF_ERROR(emitter.Initialize());

  const itex_xla::HloInstructionSequence* schedule =
      assignment.hlo_ordering().SequentialOrder(*computation);
  if (!schedule)
    return itex_xla::Unimplemented(
        "Missing sequential order for the computation");

  StatusScopedDiagnosticHandler status_handler(module.getContext());

  const std::vector<HloInstruction*>& ordering = schedule->instructions();
  TF_RETURN_IF_ERROR(computation->AcceptOrdered(&emitter, ordering));
  TF_RETURN_IF_ERROR(status_handler.ConsumeStatus());

  (void)mlir::verify(module);
  return status_handler.ConsumeStatus();
}
/*
OwningOpRef<mlir::ModuleOp> HloTextToLhloTranslateFunction(
    llvm::StringRef input, MLIRContext* context, bool optimize_xla_hlo) {
  StatusOr<std::unique_ptr<HloModule>> maybe_module =
      itex_xla::ParseAndReturnUnverifiedModule(
          absl::string_view(input.data(), input.size()));
  ITEX_CHECK_OK(maybe_module.status());

  OwningOpRef<mlir::ModuleOp> module =
      ModuleOp::create(UnknownLoc::get(context));

  ITEX_CHECK_OK(OptimizeAndConvertHloToLmhlo(maybe_module.ConsumeValueOrDie(),
                                           module.get(), "Host",
                                           optimize_xla_hlo));

  return module;
}

void RegisterMhloToLhloWithXlaPass() {
  static PassRegistration<XlaHloToLhloPass> registration;
}
*/
}  // namespace mlir
