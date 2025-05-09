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

// Utilities common between the client and server for working with
// StreamExecutor devices.

#ifndef ITEX_CORE_COMPILER_XLA_DEVICE_UTIL_H_
#define ITEX_CORE_COMPILER_XLA_DEVICE_UTIL_H_

#include <string>

#include "absl/strings/str_cat.h"
#include "itex/core/compiler/xla/types.h"
#include "itex/core/utils/stream_executor_no_cuda.h"

namespace itex_xla {

// Returns a string that represents the device in terms of platform and ordinal;
// e.g. the first CUDA device will be "cuda:0"
std::string DeviceIdentifier(se::StreamExecutor* stream_exec) {
  return absl::StrCat(stream_exec->platform()->Name(), ":",
                      stream_exec->device_ordinal());
}

}  // namespace itex_xla

#endif  // ITEX_CORE_COMPILER_XLA_DEVICE_UTIL_H_
