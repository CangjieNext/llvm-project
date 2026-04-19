; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i8 addrspace(1)* }

declare void @consume(i8 addrspace(1)*) gc "cangjie"
declare void @consume_struct(%record*) gc "cangjie"

define void @call_only_gc(i8 addrspace(1)* %arg) gc "cangjie" {
; CHECK-LABEL: @call_only_gc(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[TOKEN:%.*]] = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @consume, i32 1, i32 0, i8 addrspace(1)* [[ARG:%.*]])
; CHECK-NEXT:    ret void
entry:
  call void @consume(i8 addrspace(1)* %arg)
  ret void
}

define void @call_only_struct() gc "cangjie" {
; CHECK-LABEL: @call_only_struct(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    [[STACK:%.*]] = alloca %record, align 8
; CHECK-NEXT:    [[SLOT:%.*]] = getelementptr inbounds %record, %record* [[STACK]], i32 0, i32 0
; CHECK-NEXT:    store i8 addrspace(1)* null, i8 addrspace(1)** [[SLOT]], align 8
; CHECK-NEXT:    [[TOKEN:%.*]] = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record*)* @consume_struct, i32 1, i32 0, %record* [[STACK]])
; CHECK-NEXT:    ret void
entry:
  %stack = alloca %record, align 8
  %slot = getelementptr inbounds %record, %record* %stack, i32 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %slot, align 8
  call void @consume_struct(%record* %stack)
  ret void
}
