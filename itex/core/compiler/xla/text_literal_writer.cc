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

#include "itex/core/compiler/xla/text_literal_writer.h"

#include <memory>
#include <string>

#include "absl/strings/str_cat.h"
#include "absl/strings/str_join.h"
#include "absl/types/span.h"
#include "itex/core/compiler/xla/literal.h"
#include "itex/core/compiler/xla/shape_util.h"
#include "itex/core/compiler/xla/status_macros.h"
#include "itex/core/compiler/xla/types.h"
#include "itex/core/utils/env.h"

namespace itex_xla {

/* static */ Status TextLiteralWriter::WriteToPath(const Literal& literal,
                                                   absl::string_view path) {
  std::unique_ptr<itex::WritableFile> f;
  auto s = itex::Env::Default()->NewWritableFile(std::string(path), &f);
  if (!s.ok()) {
    return s;
  }

  s = f->Append(ShapeUtil::HumanString(literal.shape()) + "\n");
  if (!s.ok()) {
    return s;
  }

  Status status;
  itex::WritableFile* f_ptr = f.get();
  literal.EachCellAsString([f_ptr, &status](absl::Span<const int64_t> indices,
                                            const std::string& value) {
    if (!status.ok()) {
      return;
    }
    std::string coordinates =
        absl::StrCat("(", absl::StrJoin(indices, ", "), ")");

    status = f_ptr->Append(absl::StrCat(coordinates, ": ", value, "\n"));
  });
  auto ignored = f->Close();
  return status;
}

}  // namespace itex_xla
