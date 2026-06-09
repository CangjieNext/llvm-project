; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record1 = type { i8*, i8 addrspace(1)* }
%record2 = type { i8 addrspace(1)*, i64 }
%object = type { %record2, i64 }
%array = type { %object, [0 x i8] }

declare i64 @g0(%record2 addrspace(1)* %arg2, i8 addrspace(1)* %arg0) #0 gc "cangjie"
declare i64 @g1(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare i64 @g2(%record2 addrspace(1)* %arg1, i8 addrspace(1)* %arg0) #0 gc "cangjie"
declare void @g3(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare void @g4(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare void @g5(i8 addrspace(1)* %arg0, i8 addrspace(1)* %arg1, i64 %arg2, i64 %arg3, i64 %arg4) #0 gc "cangjie"
declare void @llvm.memcpy.p0i8.p0i8.i64(i8* noalias nocapture writeonly, i8* noalias nocapture readonly, i64, i1 immarg)
declare void @llvm.memcpy.p0i8.p1i8.i64(i8* noalias nocapture writeonly, i8 addrspace(1)* noalias nocapture readonly, i64, i1 immarg)

define void @foo(%record2 addrspace(1)* %arg1, i8 addrspace(1)* %arg0, %record2 addrspace(1)* %arg3, i8 addrspace(1)* %arg2, %record2 addrspace(1)* %arg5, i8 addrspace(1)* %arg4) #0 gc "cangjie" {
entry:
  %0 = alloca %record2
  %1 = alloca i64
  %2 = alloca i64
  %3 = alloca i64
  %4 = alloca i64
  %5 = alloca i8 addrspace(1)*
  %6 = alloca i64
  %7 = alloca i64
  %8 = alloca %record1
  %9 = alloca %record1
  %10 = alloca %record1
  %11 = alloca %record1
  %12 = alloca i64
  %13 = alloca i8*
  %14 = alloca i8*
  %15 = alloca %record2
  %16 = alloca i64
  %17 = alloca i64
  %18 = alloca i64
  %19 = alloca i8 addrspace(1)*
  %20 = alloca i64
  %21 = alloca i64
  %22 = alloca i64
  %23 = alloca i64
  %24 = alloca i64
  %25 = alloca i8 addrspace(1)*
  %26 = alloca i8 addrspace(1)*
  %27 = alloca %record2
  %28 = bitcast %record2* %0 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %28, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  %29 = bitcast %record1* %8 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %29, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  %30 = bitcast %record1* %9 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %30, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  %31 = bitcast %record1* %10 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %31, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  %32 = bitcast %record1* %11 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %32, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  %33 = bitcast %record2* %15 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %33, i8 addrspace(1)* align 8 %arg0, i64 16, i1 false)
  br label %start1

start1:                                   ; preds = %entry
  br label %body1

; CHECK:  body1:
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record2 addrspace(1)*, i8 addrspace(1)*)* @g0, i32 2, i32 0, %record2 addrspace(1)* %arg1, i8 addrspace(1)* %arg0) [ "gc-live"(%record2 addrspace(1)* %arg5, i8 addrspace(1)* %arg0, i8 addrspace(1)* %arg4, i8 addrspace(1)* %arg2, %record2 addrspace(1)* %arg1, %record2 addrspace(1)* %arg3) ]
;
body1:                                    ; preds = %start1
  %34 = call i64 @g0(%record2 addrspace(1)* %arg1, i8 addrspace(1)* %arg0)
  %icmpeq = icmp eq i64 %34, 0
  br i1 %icmpeq, label %tmp1, label %tmp2

tmp2:                                         ; preds = %body1
  br label %tmpend1

; CHECK:  tmp1:
; CHECK:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record2 addrspace(1)*, i8 addrspace(1)*)* @g0, i32 2, i32 0, %record2 addrspace(1)* %arg3.reloc.casted, i8 addrspace(1)* %arg2.reloc) [ "gc-live"(%record2 addrspace(1)* %arg5.reloc.casted, i8 addrspace(1)* %arg0.reloc, i8 addrspace(1)* %arg4.reloc, i8 addrspace(1)* %arg2.reloc, %record2 addrspace(1)* %arg1.reloc.casted, %record2 addrspace(1)* %arg3.reloc.casted) ]
;
tmp1:                                         ; preds = %body1
  %35 = call i64 @g0(%record2 addrspace(1)* %arg3, i8 addrspace(1)* %arg2)
  %icmpeq1 = icmp eq i64 %35, 0
  br label %tmpend1

tmpend1:                                         ; preds = %tmp1, %tmp2
  %conv = phi i1 [ false, %tmp2 ], [ %icmpeq1, %tmp1 ]
  br i1 %conv, label %ifthen1, label %ifelse1

ifthen1:                                       ; preds = %tmpend1
  %36 = bitcast %record2* %27 to i8*
  %37 = bitcast %record2 addrspace(1)* %arg5 to i8 addrspace(1)*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %36, i8 addrspace(1)* align 8 %37, i64 16, i1 false)
  ret void

ifelse1:                                       ; preds = %tmpend1
  br label %ifend1

; CHECK:  ifend1:
; CHECK:  %token10 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (i8 addrspace(1)*)* @g1, i32 1, i32 0, i8 addrspace(1)* %42) [ "gc-live"(i8 addrspace(1)* %.055, i8 addrspace(1)* %.056, i8 addrspace(1)* %.057, %record2 addrspace(1)* %.0, %record2 addrspace(1)* %.058), "struct-live"(i8 addrspace(1)** %25) ]
; CHECK:  %token17 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (i8 addrspace(1)*)* @g1, i32 1, i32 0, i8 addrspace(1)* %44) [ "gc-live"(i8 addrspace(1)* %arg0.reloc11, i8 addrspace(1)* %arg4.reloc12, i8 addrspace(1)* %arg2.reloc13, %record2 addrspace(1)* %arg5.reloc14.casted, %record2 addrspace(1)* %arg1.reloc15.casted), "struct-live"(i8 addrspace(1)** %25) ]
;
ifend1:                                        ; preds = %ifelse1
  %38 = getelementptr inbounds %record2, %record2 addrspace(1)* %arg3, i32 0, i32 0
  %39 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %38
  store i8 addrspace(1)* %39, i8 addrspace(1)** %26
  %40 = getelementptr inbounds %record2, %record2 addrspace(1)* %arg5, i32 0, i32 0
  %41 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %40
  store i8 addrspace(1)* %41, i8 addrspace(1)** %25
  %42 = load i8 addrspace(1)*, i8 addrspace(1)** %26
  %43 = call i64 @g1(i8 addrspace(1)* %42)
  store i64 %43, i64* %24
  %44 = load i8 addrspace(1)*, i8 addrspace(1)** %25
  %45 = call i64 @g1(i8 addrspace(1)* %44)
  store i64 %45, i64* %23
  %46 = load i64, i64* %24
  %icmpeq2 = icmp eq i64 %46, 0
  br i1 %icmpeq2, label %ifthen2, label %ifelse2

; CHECK:  ifthen2:
; CHECK:  %token24 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record2 addrspace(1)*, i8 addrspace(1)*)* @g2, i32 2, i32 0, %record2 addrspace(1)* %arg1.reloc22.casted, i8 addrspace(1)* %arg0.reloc18) [ "gc-live"(i8 addrspace(1)* %arg0.reloc18, i8 addrspace(1)* %arg4.reloc19, i8 addrspace(1)* %arg2.reloc20, %record2 addrspace(1)* %arg5.reloc21.casted, %record2 addrspace(1)* %arg1.reloc22.casted), "struct-live"(i8 addrspace(1)** %25) ]
; CHECK:  %token31 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record2 addrspace(1)*, i8 addrspace(1)*)* @g0, i32 2, i32 0, %record2 addrspace(1)* %arg1.reloc29.casted, i8 addrspace(1)* %arg0.reloc25) [ "gc-live"(i8 addrspace(1)* %arg0.reloc25, i8 addrspace(1)* %arg4.reloc26, i8 addrspace(1)* %arg2.reloc27, %record2 addrspace(1)* %arg5.reloc28.casted, %record2 addrspace(1)* %arg1.reloc29.casted), "struct-live"(i8 addrspace(1)** %25) ]
; CHECK:  %token38 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record2 addrspace(1)*, i8 addrspace(1)*)* @g2, i32 2, i32 0, %record2 addrspace(1)* %arg1.reloc36.casted, i8 addrspace(1)* %arg0.reloc32) [ "gc-live"(i8 addrspace(1)* %arg0.reloc32, i8 addrspace(1)* %arg4.reloc33, i8 addrspace(1)* %arg2.reloc34, %record2 addrspace(1)* %arg5.reloc35.casted), "struct-live"(i8 addrspace(1)** %25) ]
; CHECK:  %token44 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record2 addrspace(1)*, i8 addrspace(1)*)* @g2, i32 2, i32 0, %record2 addrspace(1)* %arg5.reloc42.casted, i8 addrspace(1)* %arg4.reloc40) [ "gc-live"(i8 addrspace(1)* %arg0.reloc39, i8 addrspace(1)* %arg4.reloc40, i8 addrspace(1)* %arg2.reloc41), "struct-live"(i8 addrspace(1)** %25) ]
;
ifthen2:                                       ; preds = %ifend1
  %47 = call i64 @g2(%record2 addrspace(1)* %arg1, i8 addrspace(1)* %arg0)
  %add = add i64 %47, 1
  store i64 %add, i64* %22
  %48 = call i64 @g0(%record2 addrspace(1)* %arg1, i8 addrspace(1)* %arg0)
  %49 = load i64, i64* %23
  %50 = load i64, i64* %22
  %mul = mul i64 %49, %50
  %add3 = add i64 %48, %mul
  store i64 %add3, i64* %21
  %51 = call i64 @g2(%record2 addrspace(1)* %arg1, i8 addrspace(1)* %arg0)
  %52 = call i64 @g2(%record2 addrspace(1)* %arg5, i8 addrspace(1)* %arg4)
  %53 = load i64, i64* %22
  %mul4 = mul i64 %52, %53
  %add5 = add i64 %51, %mul4
  store i64 %add5, i64* %20
  %54 = load i64, i64* %21
  %icmpsge1 = icmp sge i64 %54, 0
  br i1 %icmpsge1, label %tmp3, label %tmp4

; CHECK:  tmp4:
; CHECK:  %token49 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %arg0.reloc45) [ "gc-live"(i8 addrspace(1)* %arg2.reloc47) ]
; CHECK:  %token52 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g4, i32 1, i32 0, i8 addrspace(1)* %arg2.reloc50)
;
tmp4:                                  ; preds = %ifthen2
  call void @g3(i8 addrspace(1)* %arg0)
  call void @g4(i8 addrspace(1)* %arg2)
  ret void

; CHECK:  tmp3:
; CHECK:  %token54 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, i64, i64, i64)* @g5, i32 5, i32 0, i8 addrspace(1)* %58, i8 addrspace(1)* %59, i64 0, i64 %60, i64 %61)
;
tmp3:                                   ; preds = %ifthen2
  %55 = add i64 %54, 1
  %56 = add i64 %55, 1
  %57 = bitcast i8 addrspace(1)* %arg4 to %array addrspace(1)*
  store i8 addrspace(1)* %arg0, i8 addrspace(1)** %19
  store i64 0, i64* %18
  store i64 0, i64* %17
  store i64 0, i64* %16
  %58 = load i8 addrspace(1)*, i8 addrspace(1)** %25
  %59 = load i8 addrspace(1)*, i8 addrspace(1)** %19
  %60 = load i64, i64* %16
  %61 = load i64, i64* %23
  call void @g5(i8 addrspace(1)* %58, i8 addrspace(1)* %59, i64 0, i64 %60, i64 %61)
  %62 = load i64, i64* %16
  %63 = load i64, i64* %23
  %add6 = add i64 %62, %63
  store i64 %add6, i64* %16
  ret void

ifelse2:                                       ; preds = %ifend1
  ret void
}

attributes #0 = { "record_mut" }
