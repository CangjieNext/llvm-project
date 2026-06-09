; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i1, i8 addrspace(1)* }
%object = type { %record, i8 addrspace(1)*, i64, i64 }

declare i8 addrspace(1)* @g0(i8 addrspace(1)* %arg0, i64 %arg1) #0 gc "cangjie"
declare void @g1(i8* %arg0, i8* %arg1, i64 %arg2, i1 %arg3) #0 gc "cangjie"
declare void @g2(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %add, i64 addrspace(1)* %arg2) #0 gc "cangjie"
declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly, i8, i64, i1 immarg)

define void @foo(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0) #0 gc "cangjie" {
entry:
  %0 = alloca %record
  %1 = alloca %record
  %cast0 = bitcast %record* %0 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %cast0, i8 0, i64 16, i1 false)
  %cast1 = bitcast %record* %1 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %cast1, i8 0, i64 16, i1 false)
  br label %body

body:                                   ; preds = %entry
  %2 = bitcast %record addrspace(1)* %arg1 to %object addrspace(1)*
  %3 = getelementptr inbounds %object, %object addrspace(1)* %2, i32 0, i32 3
  %4 = load i64, i64 addrspace(1)* %3
  %5 = bitcast %record addrspace(1)* %arg1 to %object addrspace(1)*
  %6 = getelementptr inbounds %object, %object addrspace(1)* %5, i32 0, i32 2
  %7 = load i64, i64 addrspace(1)* %6
  %icmpslt = icmp slt i64 %4, %7
  br i1 %icmpslt, label %ifthen, label %ifelse

; CHECK:  ifthen:
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, i64)* @g0, i32 2, i32 0, i8 addrspace(1)* %10, i64 %13) [ "gc-live"(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0), "struct-live"(%record* %0) ]
; CHECK:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i8*, i64, i1)* @g1, i32 4, i32 0, i8* %17, i8* %18, i64 16, i1 false) [ "gc-live"(%record addrspace(1)* %arg1.reloc.casted, i8 addrspace(1)* %arg0.reloc), "struct-live"(%record* %0, %record* %1) ]
; CHECK:  %token6 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record addrspace(1)*, i8 addrspace(1)*, i64, i64 addrspace(1)*)* @g2, i32 4, i32 0, %record addrspace(1)* %arg1.reloc3.casted, i8 addrspace(1)* %arg0.reloc4, i64 %add, i64 addrspace(1)* %23)
;
ifthen:                                      ; preds = %body
  %8 = bitcast %record addrspace(1)* %arg1 to %object addrspace(1)*
  %9 = getelementptr inbounds %object, %object addrspace(1)* %8, i32 0, i32 1
  %10 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %9
  %11 = bitcast %record addrspace(1)* %arg1 to %object addrspace(1)*
  %12 = getelementptr inbounds %object, %object addrspace(1)* %11, i32 0, i32 3
  %13 = load i64, i64 addrspace(1)* %12
  %14 = call i8 addrspace(1)* @g0(i8 addrspace(1)* %10, i64 %13)
  %15 = getelementptr inbounds %record, %record* %1, i32 0, i32 0
  store i1 false, i1* %15
  %16 = getelementptr inbounds %record, %record* %1, i32 0, i32 1
  store i8 addrspace(1)* %14, i8 addrspace(1)** %16
  %17 = bitcast %record* %0 to i8*
  %18 = bitcast %record* %1 to i8*
  call void @g1(i8* %17, i8* %18, i64 16, i1 false)
  %19 = bitcast %record addrspace(1)* %arg1 to %object addrspace(1)*
  %20 = getelementptr inbounds %object, %object addrspace(1)* %19, i32 0, i32 3
  %21 = load i64, i64 addrspace(1)* %20
  %add = add i64 %21, 1
  %22 = bitcast %record addrspace(1)* %arg1 to %object addrspace(1)*
  %23 = getelementptr inbounds %object, %object addrspace(1)* %22, i32 0, i32 3
  call void @g2(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %add, i64 addrspace(1)* %23)
  ret void

ifelse:                                      ; preds = %body
  ret void
}

attributes #0 = { "record_mut" }
