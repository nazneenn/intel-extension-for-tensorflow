/* Copyright (c) 2023 Intel Corporation

Copyright 2021 The TensorFlow Authors. All Rights Reserved.

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

#ifdef TFType_DIALECT
#else
#define TFType_DIALECT

include "mlir/IR/EnumAttr.td"
include "mlir/IR/OpBase.td"
include "mlir/Interfaces/InferTypeOpInterface.td"


// ODS Definition for the dialect, see https://mlir.llvm.org/docs/OpDefinitions/
// for more information.


//===----------------------------------------------------------------------===//
// TF Type dialect definitions
//===----------------------------------------------------------------------===//

def ITEXTypeDialect : Dialect {
  let name = "itex_type";

  let summary = "This dialect defines TensorFlow Types.";
  let description = [{
    This dialect is only provide types and attributes used by various dialects
    in TensorFlow, but does not include any operation.
    It is separated from the other dialect for modularity and binary-size
    control.
  }];

  let cppNamespace = "::mlir::itex_type";
  let extraClassDeclaration = [{
     ::mlir::Type parseType(::mlir::DialectAsmParser &parser) const override;
     void printType(::mlir::Type type, ::mlir::DialectAsmPrinter &printer) const override;
  }];
  let useDefaultAttributePrinterParser = 1;
}

//===----------------------------------------------------------------------===//
// TFType dialect types and attributes definitions
//===----------------------------------------------------------------------===//

include "itex/core/ir/types/types.td"
include "itex/core/ir/types/attributes.td"

#endif // TFG_DIALECT
