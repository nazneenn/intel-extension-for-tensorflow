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

#ifndef ITEX_CORE_COMPILER_XLA_SERVICE_STREAM_POOL_H_
#define ITEX_CORE_COMPILER_XLA_SERVICE_STREAM_POOL_H_

#include <memory>
#include <vector>

#include "itex/core/compiler/xla/stream_executor/stream_executor.h"
#include "itex/core/compiler/xla/types.h"

namespace itex_xla {

// Pool of stream_executor::Streams, which are created as needed and
// destroyed when the pool is destroyed.
class StreamPool {
 public:
  struct PtrDeleter {
    void operator()(se::Stream* stream) { pool->ReturnStream(stream); }
    StreamPool* pool;
  };

  // Stream pointer type returned by BorrowStream, which returns the
  // stream to the pool on destruction.
  using Ptr = std::unique_ptr<se::Stream, PtrDeleter>;

  StreamPool() {}

  // Returns a pointer to a stream in the pool, creating a new stream
  // if none are available in the pool. The returned smart pointer
  // returns the stream to the pool on destruction.
  //
  // This method is thread-safe.
  Ptr BorrowStream(se::StreamExecutor* executor);

 private:
  // Puts a pointer to a stream back into the pool, leaving it free
  // for future use. Streams that have previously encountered errors
  // are deleted, and not returned to the pool.
  //
  // This method is thread-safe.
  void ReturnStream(se::Stream* stream);

  absl::Mutex mu_;
  std::vector<std::unique_ptr<se::Stream>> streams_ ABSL_GUARDED_BY(mu_);
};

}  // namespace itex_xla

#endif  // ITEX_CORE_COMPILER_XLA_SERVICE_STREAM_POOL_H_
