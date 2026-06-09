; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i64, i8 addrspace(1)* }
%object = type { %record, i64 }
%array = type { %object, [0 x i8 addrspace(1)*] }

declare void @g0(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0) #0 gc "cangjie"
declare void @g1(i8 addrspace(1)* %arg) #0 gc "cangjie"

define i8 addrspace(1)* @foo(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %arg2, i8 addrspace(1)* %arg3) #0 gc "cangjie" {
; CHECK-LABEL: @foo(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    icmp slt i64 %arg2, 0
; CHECK-NEXT:    bitcast %record addrspace(1)* [[ARG1:%.*]] to %object addrspace(1)*
; CHECK-NEXT:    getelementptr inbounds %object, %object addrspace(1)* %1, i32 0, i32 1
; CHECK-NEXT:    load i64, i64 addrspace(1)* %2
; CHECK-NEXT:    icmp sge i64 %arg2, %3
; CHECK-NEXT:    or i1 %0, %4
; CHECK-NEXT:    br i1 %5, label %ifelse, label %ifend
;
entry:
  %0 = icmp slt i64 %arg2, 0
  %1 = bitcast %record addrspace(1)* %arg1 to %object addrspace(1)*
  %2 = getelementptr inbounds %object, %object addrspace(1)* %1, i32 0, i32 1
  %3 = load i64, i64 addrspace(1)* %2
  %4 = icmp sge i64 %arg2, %3
  %5 = or i1 %0, %4
  br i1 %5, label %ifelse, label %ifend

; CHECK:    ifelse:
; CHECK:    %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record addrspace(1)*, i8 addrspace(1)*)* @g0, i32 2, i32 0, %record addrspace(1)* %arg1, i8 addrspace(1)* null) [ "gc-live"(i8 addrspace(1)* %arg3) ]
; CHECK:    %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g1, i32 1, i32 0, i8 addrspace(1)* %arg3.reloc) [ "gc-live"(i8 addrspace(1)* %arg3.reloc) ]
;
ifelse:                                      ; preds = %body
  call void @g0(%record addrspace(1)* %arg1, i8 addrspace(1)* null)
  call void @g1(i8 addrspace(1)* %arg3)
  ret i8 addrspace(1)* %arg3

; CHECK:    ifend:
; CHECK:    %token5 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record addrspace(1)*, i8 addrspace(1)*)* @g0, i32 2, i32 0, %record addrspace(1)* %arg1, i8 addrspace(1)* %8) [ "gc-live"(i8 addrspace(1)* %8) ]
;
ifend:                                       ; preds = %body
  %6 = bitcast %record addrspace(1)* %arg1 to %array addrspace(1)*
  %7 = getelementptr inbounds %array, %array addrspace(1)* %6, i32 0, i32 1, i64 %arg2
  %8 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %7
  call void @g0(%record addrspace(1)* %arg1, i8 addrspace(1)* %8)
  ret i8 addrspace(1)* %8
}

attributes #0 = { "record_mut" }
