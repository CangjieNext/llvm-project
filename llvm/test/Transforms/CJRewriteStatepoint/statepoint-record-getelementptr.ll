; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s


%struct = type { i8 addrspace(1)* }

define void @booo() gc "cangjie" {
entry:
  %r = alloca %struct
  br label %bb0

; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @testcase, i32 0, i32 0)
;
bb0:
  call void @testcase()
  br label %bb1

bb1:
  %0 = bitcast %struct* %r to i8*
  %1 = addrspacecast %struct* %r to %struct addrspace(1)*
  %2 = getelementptr inbounds %struct, %struct addrspace(1)* %1, i64 0, i32 0  ; CastInst and GEP are not a meaningful use point.
  br label %bb2

; CHECK:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @testcase2, i32 1, i32 0, i8 addrspace(1)* %3)
;
bb2:
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %0)
  call void @llvm.memset.p0i8.i64(i8* %0, i8 0, i64 8, i1 false)
  %3 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %2
  call void @testcase2(i8 addrspace(1)* %3)
  ret void
}


declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly, i8, i64, i1 immarg)
declare void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture)

declare void @testcase() gc "cangjie"
declare void @testcase2(i8 addrspace(1)* %arg0) gc "cangjie"
