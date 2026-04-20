; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

declare void @gc_consume(i8 addrspace(1)*)

define void @call_only_gc(i8 addrspace(1)* %obj) gc "cangjie" {
; CHECK-LABEL: @call_only_gc(
; CHECK:       call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @gc_consume, i32 1, i32 0, i8 addrspace(1)* %obj)
; CHECK-NOT:   "gc-live"
; CHECK-NEXT:  ret void
entry:
  call void @gc_consume(i8 addrspace(1)* %obj)
  ret void
}

define void @call_then_reuse_gc(i8 addrspace(1)* %obj) gc "cangjie" {
; CHECK-LABEL: @call_then_reuse_gc(
; CHECK:       [[FIRST:%.*]] = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @gc_consume, i32 1, i32 0, i8 addrspace(1)* %obj) [ "gc-live"(i8 addrspace(1)* %obj) ]
; CHECK:       [[OBJ_RELOC:%.*]] = call coldcc i8 addrspace(1)* @llvm.cj.gc.relocate.p1i8(token [[FIRST]], i32 0, i32 0)
; CHECK:       [[SECOND:%.*]] = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @gc_consume, i32 1, i32 0, i8 addrspace(1)* [[OBJ_RELOC]])
; CHECK-NOT:   "gc-live"
; CHECK-NEXT:  ret void
entry:
  call void @gc_consume(i8 addrspace(1)* %obj)
  call void @gc_consume(i8 addrspace(1)* %obj)
  ret void
}
