/* Copyright (c) 2023 Intel Corporation

Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

#ifndef ITEX_CORE_COMPILER_XLA_STREAM_EXECUTOR_PLATFORM_PORT_H_
#define ITEX_CORE_COMPILER_XLA_STREAM_EXECUTOR_PLATFORM_PORT_H_
#include <string>

#include "itex/core/utils/macros.h"
#include "itex/core/utils/types.h"

namespace stream_executor {

using itex::int16;
using itex::int32;
using itex::int8;

using itex::uint16;
using itex::uint32;
using itex::uint64;
using itex::uint8;

#if !defined(PLATFORM_GOOGLE)
using std::string;
#endif

#define SE_FALLTHROUGH_INTENDED TF_FALLTHROUGH_INTENDED

}  // namespace stream_executor

#define SE_DISALLOW_COPY_AND_ASSIGN TF_DISALLOW_COPY_AND_ASSIGN
#define SE_MUST_USE_RESULT TF_MUST_USE_RESULT
#define SE_PREDICT_TRUE TF_PREDICT_TRUE
#define SE_PREDICT_FALSE TF_PREDICT_FALSE

#endif  // ITEX_CORE_COMPILER_XLA_STREAM_EXECUTOR_PLATFORM_PORT_H_
