; Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
; This source file is part of the Cangjie project, licensed under Apache-2.0
; with Runtime Library Exception.

; See https://cangjie-lang.cn/pages/LICENSE for license information.

; RUN: llc -O0 --cangjie-pipeline -mtriple x86_64-pc-linux-gnu < %s | FileCheck %s

; Repeated callsites with the same derived/base location sequence should share
; one compressed DerivedInfo entry instead of appending duplicate table rows.

; CHECK:      #StackMapItem nums:2
; CHECK:      #[RegIdx: -1, SlotIdx: 0, LNIdx: -1, DerivedStartIdx: 0, SPRegIdx: -1, SPSlotIdx: -1]
; CHECK:      #[RegIdx: -1, SlotIdx: 0, LNIdx: -1, DerivedStartIdx: 0, SPRegIdx: -1, SPSlotIdx: -1]
; CHECK:      #DerivedInfoNums: 1
; CHECK-NEXT: {{.*}}#Idx[0]: RegIdx: -1, SlotIdx: 1

declare cangjiegccc void @g0()
declare token @llvm.cj.gc.statepoint(...)
declare i8 addrspace(1)* @llvm.cj.gc.relocate.p1i8(token, i32 immarg,
                                                    i32 immarg)

define i8 addrspace(1)* @dedup(i8 addrspace(1)* %base,
                                i8 addrspace(1)* %derived) gc "cangjie" {
entry:
  %t0 = call cangjiegccc token (...) @llvm.cj.gc.statepoint(i64 1, i32 0, void ()* @g0, i32 0, i32 0) [ "gc-live"(i8 addrspace(1)* %base, i8 addrspace(1)* %derived) ]
  %base0 = call coldcc i8 addrspace(1)* @llvm.cj.gc.relocate.p1i8(token %t0, i32 0, i32 0)
  %derived0 = call coldcc i8 addrspace(1)* @llvm.cj.gc.relocate.p1i8(token %t0, i32 0, i32 1)
  %t1 = call cangjiegccc token (...) @llvm.cj.gc.statepoint(i64 2, i32 0, void ()* @g0, i32 0, i32 0) [ "gc-live"(i8 addrspace(1)* %base0, i8 addrspace(1)* %derived0) ]
  %derived1 = call coldcc i8 addrspace(1)* @llvm.cj.gc.relocate.p1i8(token %t1, i32 0, i32 1)
  ret i8 addrspace(1)* %derived1
}