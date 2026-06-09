; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i64, i8 addrspace(1)* }
%object = type { %record, i32, i64 }
@data = global i16 0

declare void @g0(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1) #0 gc "cangjie"
declare void @g1(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1) #0 gc "cangjie"
declare i1   @g2(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1) #0 gc "cangjie"
declare void @g3(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1, i1 %arg2) #0 gc "cangjie"

define i64 @foo(i8 addrspace(1)* %a, %record addrspace(1)* %arg) #0 gc "cangjie" {
entry:
  %0 = alloca %record
  %1 = alloca %record addrspace(1)*
  br label %body

; CHECK:  body:
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, %record addrspace(1)*)* @g0, i32 2, i32 0, i8 addrspace(1)* %a, %record addrspace(1)* %arg) [ "gc-live"(%record addrspace(1)* %arg) ]
;
body:                                     ; preds = %entry
  call void @g0(i8 addrspace(1)* %a, %record addrspace(1)* %arg)
  store %record addrspace(1)* %arg, %record addrspace(1)** %1
  %2 = load %record addrspace(1)*, %record addrspace(1)** %1
  %3 = bitcast %record addrspace(1)* %2 to %object addrspace(1)*
  %4 = getelementptr inbounds %object, %object addrspace(1)* %3, i32 0, i32 1
  %5 = load i32, i32 addrspace(1)* %4
  %icmpeq = icmp eq i32 %5, 1
  br i1 %icmpeq, label %tmp1, label %tmp2

tmp2:                                         ; preds = %body
  br label %tmpend

tmp1:                                         ; preds = %body
  %6 = load i16, i16* @data
  %icmpeq1 = icmp eq i16 %6, 3
  br label %tmpend

tmpend:                                         ; preds = %tmp1, %tmp2
  %conv = phi i1 [ false, %tmp2 ], [ %icmpeq1, %tmp1 ]
  br i1 %conv, label %ifthen, label %ifelse

; CHECK:  ifthen:
; CHECK:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, %record addrspace(1)*)* @g1, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %9) [ "struct-live"(%record* %0) ]
; CHECK:  %token4 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i1 (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %9) [ "struct-live"(%record* %0) ]
; CHECK:  %token6 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, %record addrspace(1)*, i1)* @g3, i32 3, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %11, i1 %10) [ "struct-live"(%record* %0) ]
;
ifthen:                                        ; preds = %tmpend
  %7 = add i32 %5, 1
  %8 = load %record, %record addrspace(1)* %2
  store %record %8, %record* %0
  %9 = addrspacecast %record* %0 to %record addrspace(1)*
  call void @g1(i8 addrspace(1)* null, %record addrspace(1)* %9)
  %10 = call i1 @g2(i8 addrspace(1)* null, %record addrspace(1)* %9)
  %11 = addrspacecast %record* %0 to %record addrspace(1)*
  call void @g3(i8 addrspace(1)* null, %record addrspace(1)* %11, i1 %10)
  br label %ifend

ifelse:                                        ; preds = %tmpend
  br label %ifend

ifend:                                         ; preds = %ifelse, %ifthen
  ret i64 0
}

attributes #0 = { "hasRcdParam" }
