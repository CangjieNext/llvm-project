; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%struct = type { i8 addrspace(1)*, i64 }

declare void @tail_target()
declare void @gc_target(i8 addrspace(1)*)
declare void @struct_target(%struct*)
declare void @CJ_MCC_HandleSafepoint()

define void @tail_empty() gc "cangjie" {
; CHECK-LABEL: @tail_empty(
; CHECK:       entry:
; CHECK-NEXT:    tail call void @tail_target()
; CHECK-NEXT:    ret void
; CHECK-NOT:   @llvm.cj.gc.statepoint
entry:
  tail call void @tail_target()
  ret void
}

define void @tail_with_gc_live(i8 addrspace(1)* %obj) gc "cangjie" {
; CHECK-LABEL: @tail_with_gc_live(
; CHECK:       entry:
; CHECK-NEXT:    tail call void @gc_target(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
entry:
  tail call void @gc_target(i8 addrspace(1)* %obj)
  ret void
}

define void @tail_with_struct_live() gc "cangjie" {
; CHECK-LABEL: @tail_with_struct_live(
; CHECK:       entry:
; CHECK-NEXT:    [[S:%.*]] = alloca %struct, align 8
; CHECK-NEXT:    tail call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%struct*)* @struct_target, i32 1, i32 0, %struct* [[S]]) [ "struct-live"(%struct* [[S]]) ]
; CHECK-NEXT:    ret void
entry:
  %s = alloca %struct, align 8
  tail call void @struct_target(%struct* %s)
  ret void
}

define void @tail_special_safepoint() gc "cangjie" {
; CHECK-LABEL: @tail_special_safepoint(
; CHECK:       entry:
; CHECK-NEXT:    tail call token (...) @llvm.cj.gc.statepoint(i64 3, i32 0, void ()* @CJ_MCC_HandleSafepoint, i32 0, i32 0)
; CHECK-NEXT:    ret void
entry:
  tail call void @CJ_MCC_HandleSafepoint()
  ret void
}
