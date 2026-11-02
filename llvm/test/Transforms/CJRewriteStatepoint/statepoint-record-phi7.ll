; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s


%struct_1 = type { i32, i32 }
%struct_2 = type { i8 addrspace(1)* }

%struct_all = type { %struct_1, %struct_2 }

define void @main() gc "cangjie" {
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 ()* @tmp_use, i32 0, i32 0)
;
entry:
  %r = alloca %struct_all
  %0 = bitcast %struct_all* %r to i8*
  call void @llvm.memset.p0i8.i64(i8* %0, i8 0, i64 8, i1 false)
  %cmp = call i64 @tmp_use()
  %icmpeq.i = icmp eq i64 %cmp, 0
  br i1 %icmpeq.i, label %bb1, label %bb2

bb1:
  %1 = getelementptr inbounds %struct_all, %struct_all* %r, i64 0, i32 0
  br label %bbend

bb2:
  %2 = getelementptr inbounds %struct_all, %struct_all* %r, i64 0, i32 0
  br label %bbend

; CHECK:  [[GC:%.*]] = addrspacecast %struct_1* %sink to %struct_1 addrspace(1)*
; CHECK:  %token3 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%struct_1 addrspace(1)*)* @test, i32 1, i32 0, %struct_1 addrspace(1)* [[GC]])
;
bbend:
  %sink = phi %struct_1* [%1, %bb1], [%2, %bb2]
  %3 = addrspacecast %struct_1* %sink to %struct_1 addrspace(1)*
  call void @test(%struct_1 addrspace(1)* %3)
  ret void
}

declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly, i8, i64, i1 immarg)

declare i64 @tmp_use() gc "cangjie"
declare void @test(%struct_1 addrspace(1)* %arg0) gc "cangjie"
