/* Copyright (c) 2023 Intel Corporation

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

#ifndef HS_OPS
#define HS_OPS


include "mlir/IR/RegionKindInterface.td"
include "mlir/Interfaces/CallInterfaces.td"
include "mlir/IR/SymbolInterfaces.td"
include "mlir/Interfaces/SideEffectInterfaces.td"
include "mlir/Interfaces/ControlFlowInterfaces.td"
include "mlir/IR/OpAsmInterface.td"

include "xpuautoshard/common/mlir/dialect.td"
include "xpuautoshard/common/mlir/attributes.td"



// Base class for the operations in this dialect.

class HS_Op<string mnemonic, list<Trait> traits = []> :
    Op<HSDialect, mnemonic, traits> {
    let extraClassDeclaration = [{
    //===------------------------------------------------------------------===//
    // OpAsmOpInterface
    //===------------------------------------------------------------------===//

    // This will filter the `hs.` prefix in front of operations inside the
    // graph body.
    static StringRef getDefaultDialect() {
      return "hs";
    }
    }];
  } 
//===----------------------------------------------------------------------===//
// HS-IR shard_op definitions
//===----------------------------------------------------------------------===//
def HS_ShardOp : HS_Op<"shard_op",
  []> {
  let summary = "shards the unsharded tensor with the HSP and outputs a sharded tensor.";
  let description = [{
    TODO: clarify the logic of handling scalars and control edges.
    Shards the unsharded tensor with the HSP and outputs a sharded tensor.
    This `hs.shard_op` comes from two ways :
     1. Converted from `tfg.hint_op`. This is user given hint.
     2. Added by propagation pass.
    Normally, every input tensor of the graph should be attatched to a `shard_op`.

    Input:
    - value : Tensor to be sharded
    - hsp : Argument representing sharding strategy.
  }];

  let arguments = (ins
    AnyType:$value,
    ShardingPropertyAttr:$hsp
  );
  let results = (outs
    AnyType:$result
  );
}

//===----------------------------------------------------------------------===//
// HS-IR Reshard_op definitions
//===----------------------------------------------------------------------===//

def HS_ReshardOp : HS_Op<"reshard_op",
  [Pure]> {
  let summary = "ReShard a tensor. ";
  let description = [{
    Reshards a tensor. It may need to do communication before the reshard. 
    Input:
    - value : Tensor to be sharded
    - hsp : Argument representing sharding strategy.
  }];

  let arguments = (ins
    AnyTensor:$value,
    ShardingPropertyAttr:$hsp
  );
  let results = (outs
    AnyTensor:$result
  );

}



//===----------------------------------------------------------------------===//
// HS-IR unshard_op definitions
//===----------------------------------------------------------------------===//

// May refer to https://llvm.discourse.group/t/rfc-reshape-ops-restructuring/3310
def HS_UnshardOp : HS_Op<"unshard_op",
  [Pure]> {
  let summary = "Unshards a sharded tensor. ";
  let description = [{
   Unshards a sharded tensor. It may need to do communication before the reshard. 
   Normally the communication op is generated by hs.post_op.
  }];

  let arguments = (ins
    AnyType:$value
  );
  let results = (outs
    AnyType:$result
  );

}


#endif // HS_OPS