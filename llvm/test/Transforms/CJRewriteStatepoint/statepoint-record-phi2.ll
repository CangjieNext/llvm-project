; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i32, i8 addrspace(1)* }

declare void @g0(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1) #0 gc "cangjie"
declare i8 @g1(%record %arg0, i64 %arg1) #0 gc "cangjie"
declare void @g2(i8 addrspace(1)* %arg0, i64 %arg1) #0 gc "cangjie"
declare void @llvm.memcpy.p0i8.p0i8.i64(i8* noalias nocapture writeonly, i8* noalias nocapture readonly, i64, i1 immarg)
declare void @llvm.memcpy.p0i8.p1i8.i64(i8* noalias nocapture writeonly, i8 addrspace(1)* noalias nocapture readonly, i64, i1 immarg)

define void @foo(i64 %index, i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1, %record %arg2) #0 gc "cangjie" {
entry:
  %0 = alloca %record
  %1 = alloca %record
  %2 = alloca %record
  %3 = alloca i8
  %4 = alloca i64
  br label %body1

; CHECK:  body1:
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, %record addrspace(1)*)* @g0, i32 2, i32 0, i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1) [ "gc-live"(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1, %record %arg2), "struct-live"(%record* %1) ]
; CHECK:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 (%record, i64)* @g1, i32 2, i32 0, %record %arg2, i64 0) [ "gc-live"(i8 addrspace(1)* %arg0.reloc, %record addrspace(1)* %arg1.reloc.casted, %record %arg2), "struct-live"(%record* %1) ]
;
body1:                                    ; preds = %entry
  %sub = sub i64 %index, 1
  store i64 %sub, i64* %4
  store i8 0, i8* %3
  %5 = bitcast %record* %1 to i8*
  %6 = add i64 %sub, 1
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %5, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  call void @g0(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1)
  %7 = bitcast %record* %2 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %7, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  %8 = call i8 @g1(%record %arg2, i64 0)
  br label %body2

body2:                                    ; preds = %ifthen2, %body1
  %9 = load i64, i64* %4
  %icmpsge = icmp sge i64 %9, 0
  br i1 %icmpsge, label %ifthen1, label %ifelse1

; CHECK:  ifthen1:
; CHECK:  %token6 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 (%record, i64)* @g1, i32 2, i32 0, %record %arg2, i64 %9) [ "gc-live"(i8 addrspace(1)* %.0, %record addrspace(1)* %.015, %record %arg2), "struct-live"(%record* %1) ]
;
ifthen1:                                       ; preds = %body2
  %10 = load i64, i64* %4
  %11 = call i8 @g1(%record %arg2, i64 %10)
  store i8 %11, i8* %3
  %12 = load i8, i8* %3
  %13 = load i8, i8 addrspace(1)* %arg0
  %icmpuge = icmp uge i8 %12, %13
  br i1 %icmpuge, label %tmp1, label %tmp2

ifelse1:                                       ; preds = %body2
  br label %ifelseend

tmp1:                                         ; preds = %ifthen1
  %14 = load i8, i8* %3
  %15 = load i8, i8 addrspace(1)* %arg0
  %icmpult = icmp ult i8 %14, %15
  br label %tmpend1

tmp2:                                         ; preds = %ifthen1
  br label %tmpend1

tmpend1:                                         ; preds = %tmp1, %tmp2
  %conv = phi i1 [ false, %tmp2 ], [ %icmpult, %tmp1 ]
  br i1 %conv, label %ifthen2, label %ifelse2

ifthen2:                                       ; preds = %tmpend1
  %16 = load i64, i64* %4
  %sub2 = sub i64 %16, 1
  store i64 %sub2, i64* %4
  br label %body2

; CHECK:  ifelse2:
; CHECK:  %token10 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i64)* @g2, i32 2, i32 0, i8 addrspace(1)* %arg0.reloc7, i64 %16) [ "gc-live"(i8 addrspace(1)* %arg0.reloc7, %record addrspace(1)* %arg1.reloc8.casted), "struct-live"(%record* %1) ]
;
ifelse2:                                       ; preds = %tmpend1
  %17 = load i64, i64* %4
  %18 = bitcast %record* %1 to i8*
  %19 = bitcast %record* %0 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %19, i8* align 8 %18, i64 16, i1 false)
  call void @g2(i8 addrspace(1)* %arg0, i64 %17)
  br label %ifelseend

ifelseend:                                     ; preds = %ifelse1, %ifelse2
  %20 = getelementptr inbounds %record, %record* %1, i32 0, i32 0
  %21 = load i32, i32* %20
  %icmpeq = icmp eq i32 %21, 0
  br i1 %icmpeq, label %tmp3, label %tmp4

tmp3:                                        ; preds = %ifelseend
  %22 = getelementptr inbounds %record, %record* %1, i32 0, i32 1
  %23 = load i8 addrspace(1)*, i8 addrspace(1)** %22
  %24 = load i8, i8 addrspace(1)* %23
  %25 = add i8 %24, 1
  %icmpeq6 = icmp eq i8 %25, -1
  br label %tmpend2

tmp4:                                        ; preds = %ifelseend
  br label %tmpend2

tmpend2:                                        ; preds = %tmp3, %tmp4
  %conv8 = phi i1 [ false, %tmp4 ], [ %icmpeq6, %tmp3 ]
  br i1 %conv8, label %ifthen3, label %ifelse3

; CHECK:  ifthen3:
; CHECK:  %token14 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, %record addrspace(1)*)* @g0, i32 2, i32 0, i8 addrspace(1)* %.1, %record addrspace(1)* %.116)
;
ifthen3:                                       ; preds = %tmpend2
  call void @g0(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1)
  ret void

ifelse3:                                       ; preds = %tmpend2
  ret void
}

attributes #0 = { "hasRcdParam" }
