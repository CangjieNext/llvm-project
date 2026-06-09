; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i32, i8 addrspace(1)* }

declare void @g0(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0) #0 gc "cangjie"
declare void @g1(%record %arg0) #0 gc "cangjie"

define i8 addrspace(1)* @foo(i1 %cond, %record %arg0, %record addrspace(1)* %arg2, i8 addrspace(1)* %arg1) #0 gc "cangjie" {
entry:
  %0 = alloca %record
  %1 = getelementptr inbounds %record, %record* %0, i32 0, i32 1
  store %record %arg0, %record* %0
  br i1 %cond, label %for, label %end

; CHECK: for:
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record addrspace(1)*, i8 addrspace(1)*)* @g0, i32 2, i32 0, %record addrspace(1)* %.0, i8 addrspace(1)* %.03) [ "gc-live"(%record addrspace(1)* %.0, i8 addrspace(1)* %.03, %record %arg0), "struct-live"(%record* %0) ]
;
for:                                    ; preds = %entry, %for
  %phi_i = phi i32 [ 0, %entry ], [ %i, %for ]
  %phi = phi i8 addrspace(1)** [ %1, %entry], [ %2, %for ]
  %i = add i32 %phi_i, 1
  %2 = getelementptr inbounds i8 addrspace(1)*, i8 addrspace(1)** %phi, i32 0
  call void @g0(%record addrspace(1)* %arg2, i8 addrspace(1)* %arg1)
  %loop.cond = icmp ult i32 %i, 10
  br i1 %loop.cond, label %for, label %end

; CHECK:  end:
; CHECK:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record)* @g1, i32 1, i32 0, %record %arg0) [ "gc-live"(i8 addrspace(1)* %load) ]
;
end:                                    ; preds = %entry, %for
  %phi_end = phi i8 addrspace(1)** [ %1, %entry], [ %phi, %for ]
  %load = load i8 addrspace(1)*, i8 addrspace(1)** %phi_end
  call void @g1(%record %arg0)
  ret i8 addrspace(1)* %load
}

attributes #0 = { "record_mut" }
