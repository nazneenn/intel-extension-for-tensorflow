/* Copyright (c) 2023 Intel Corporation

Copyright 2022 The TensorFlow Authors. All Rights Reserved.

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

#ifndef ITEX_CORE_COMPILER_XLA_SERVICE_GPU_MATMUL_UTILS_H_
#define ITEX_CORE_COMPILER_XLA_SERVICE_GPU_MATMUL_UTILS_H_

#include <cstdint>
#include <vector>

#include "absl/types/span.h"
#include "itex/core/compiler/xla/service/hlo_instruction.h"
#include "itex/core/compiler/xla/shape.h"
#include "itex/core/compiler/xla/statusor.h"
#include "itex/core/compiler/xla/stream_executor/blas.h"
#include "itex/core/compiler/xla/stream_executor/device_memory.h"
#include "lhlo_gpu/IR/lhlo_gpu_ops.h"
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "protos/xla_data.pb.h"

// namespace stream_executor {
// namespace blas {
// // Specifies whether the input matrix will be transposed or
// // transposed+conjugated before any BLAS operations.
// enum class Transpose { kNoTranspose, kTranspose, kConjugateTranspose };

// // This struct contains the metadata of a matrix, e.g., its base address and
// // dimensions.
// struct MatrixDescriptor {
//   DeviceMemoryBase data;
//   Transpose transpose;
//   int64_t num_rows;
//   int64_t num_cols;
//   int64_t stride;

//   int64_t reduced_dim() const {
//     return transpose == Transpose::kTranspose ? num_rows : num_cols;
//   }

//   template <typename T>
//   DeviceMemory<T> cast() const {
//     return DeviceMemory<T>(data);
//   }
// };
// } // namespace blas
// } // namespace stream_executor

namespace itex_xla {
namespace gpu {

StatusOr<std::vector<int64_t>> GetNonContractingDims(
    const Shape& shape, absl::Span<const int64_t> batch_dims,
    absl::Span<const int64_t> contracting_dims);

// Normalize shape to (batch, rows, columns) logical dimensions.
StatusOr<Shape> GetBatchRowColumnShape(const Shape& shape,
                                       absl::Span<const int64_t> batch_dims,
                                       absl::Span<const int64_t> row_dims,
                                       absl::Span<const int64_t> col_dims);

struct MatrixLayout {
  enum class Order {
    kRowMajor,     // Elements in the same row are contiguous in memory.
    kColumnMajor,  // Elements in the same column are contiguous in memory.
  };

  // Returns the matrix layout for a logical shape (batch, rows, columns).
  static StatusOr<MatrixLayout> For(const Shape& shape);
  static StatusOr<MatrixLayout> For(const Shape& shape,
                                    absl::Span<const int64_t> batch_dims,
                                    absl::Span<const int64_t> row_dims,
                                    absl::Span<const int64_t> col_dims);

  PrimitiveType dtype;
  // `num_rows` / `num_cols` are for the "logical" matrix shape:
  // i.e. the contracting dim has size `num_cols` for LHS operands and
  // `num_rows` for RHS operands.
  int64_t num_rows;
  int64_t num_cols;
  Order order;
  int64_t leading_dim_stride;
  int64_t batch_size;
  int64_t batch_stride;  // `batch_stride` is set to `0` when `batch_size == 1`.
};

struct GemmConfig {
  static StatusOr<GemmConfig> For(const HloInstruction* gemm);
  static StatusOr<GemmConfig> For(mlir::lmhlo_gpu::GEMMOp op,
                                  bool use_cublaslt);

  static StatusOr<GemmConfig> For(
      const Shape& lhs_shape, absl::Span<const int64_t> lhs_batch_dims,
      absl::Span<const int64_t> lhs_contracting_dims, const Shape& rhs_shape,
      absl::Span<const int64_t> rhs_batch_dims,
      absl::Span<const int64_t> rhs_contracting_dims, const Shape& output_shape,
      double alpha_real, double alpha_imag, double beta,
      std::optional<int64_t> algorithm, bool use_cublaslt);

  MatrixLayout lhs_layout;
  MatrixLayout rhs_layout;
  MatrixLayout output_layout;
  complex128 alpha;
  double beta;
  absl::optional<int64_t> algorithm;
  bool use_cublaslt;
};

se::blas::MatrixDescriptor GetMatrixDesc(const MatrixLayout& layout,
                                         se::DeviceMemoryBase data);

void MakeBlasGemmCompatible(se::blas::MatrixDescriptor& lhs,
                            se::blas::MatrixDescriptor& rhs,
                            se::blas::MatrixDescriptor& output);

}  // namespace gpu
}  // namespace itex_xla

#endif  // ITEX_CORE_COMPILER_XLA_SERVICE_GPU_MATMUL_UTILS_H_
