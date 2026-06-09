; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i8 addrspace(1)*, i64 }
%object = type { %record, i64 }
%array = type { %object, [0 x i8] }
%struct = type { i64, i64 }

@data = global i64 0

declare i64 @g0(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare i64 @g1(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0) #0 gc "cangjie"
declare void @g2(%record addrspace(1)* %arg2, i8 addrspace(1)* %arg1) #0 gc "cangjie"
declare i8 addrspace(1)* @g3(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %arg2, i64 %arg3) #0 gc "cangjie"
declare void @g4(i8 addrspace(1)* %arg0, i64 %arg1) #0 gc "cangjie"
declare i64 @g5(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %arg2) #0 gc "cangjie"
declare void @g6(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %arg2, i64 %arg3) #0 gc "cangjie"
declare void @g7(i8 addrspace(1)* %arg0, i8 addrspace(1)* %arg1, i64 %arg2, i64 %arg3, i64 %arg4) #0 gc "cangjie"
declare i64 @g8(i64 %arg0, i64 %arg1) #0 gc "cangjie"
declare i1 @g9(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare void @g10(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare i1 @g11(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0) #0 gc "cangjie"
declare i64 @g12(i8 addrspace(1)* %arg0, i64 %arg1) #0 gc "cangjie"
declare void @llvm.memcpy.p0i8.p0i8.i64(i8* noalias nocapture writeonly, i8* noalias nocapture readonly, i64, i1 immarg)
declare void @llvm.memcpy.p0i8.p1i8.i64(i8* noalias nocapture writeonly, i8 addrspace(1)* noalias nocapture readonly, i64, i1 immarg)

define void @foo(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg2, i8 addrspace(1)* %arg0) #0 gc "cangjie" {
entry:
  %0 = alloca %record
  %1 = alloca i64
  %2 = alloca %record
  %3 = alloca %record
  %4 = alloca %record
  %5 = alloca i8 addrspace(1)*
  %6 = alloca i1
  %7 = alloca i64
  %8 = alloca i64
  %9 = alloca i64
  %10 = alloca i64
  %11 = alloca i64
  %12 = alloca i64
  %13 = alloca i64
  %14 = alloca %struct
  %15 = alloca i8 addrspace(1)*
  %16 = alloca i64
  %17 = alloca i64
  %18 = alloca %record
  %19 = alloca i64
  %20 = alloca i1
  %21 = alloca i64
  %22 = alloca i64
  %23 = alloca i64
  %24 = bitcast %record* %2 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %24, i8 addrspace(1)* %arg0, i64 16, i1 false)
  %25 = bitcast %record* %3 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %25, i8 addrspace(1)* %arg0, i64 16, i1 false)
  %26 = bitcast %record* %4 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %26, i8 addrspace(1)* %arg0, i64 16, i1 false)
  %27 = bitcast %record* %18 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %27, i8 addrspace(1)* %arg0, i64 16, i1 false)
  br label %body1

; CHECK:  body1:
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i1 (i8 addrspace(1)*)* @g9, i32 1, i32 0, i8 addrspace(1)* %arg2) [ "gc-live"(i8 addrspace(1)* %arg0, i8 addrspace(1)* %arg2, %record addrspace(1)* %arg1), "struct-live"(%record* %2, %record* %4, %record* %18) ]
;
body1:                                    ; preds = %entry
  %28 = call i1 @g9(i8 addrspace(1)* %arg2)
  br i1 %28, label %ifthen1, label %ifelse1

ifthen1:                                       ; preds = %body1
  %29 = bitcast %record* %0 to i8*
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* align 8 %29, i8 addrspace(1)* %arg0, i64 16, i1 false)
  ret void

ifelse1:                                       ; preds = %body1
  br label %ifend1

; CHECK:  ifend1:
; CHECK:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record addrspace(1)*, i8 addrspace(1)*)* @g1, i32 2, i32 0, %record addrspace(1)* %arg1.reloc.casted, i8 addrspace(1)* %arg0.reloc) [ "gc-live"(i8 addrspace(1)* %arg2.reloc, %record addrspace(1)* %arg1.reloc.casted, i8 addrspace(1)* %arg0.reloc), "struct-live"(%record* %2, %record* %4, %record* %18) ]
;
ifend1:                                        ; preds = %ifelse1
  %30 = getelementptr inbounds %record, %record addrspace(1)* %arg1, i32 0, i32 1
  %31 = load i64, i64 addrspace(1)* %30
  store i64 %31, i64* %23
  store i64 %31, i64* %22
  store i64 %31, i64* %21
  %32 = load i64, i64* %21
  store i64 %31, i64* %22
  %33 = load i64, i64* %22
  store i1 true, i1* %20
  %34 = call i64 @g1(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0)
  store i64 %34, i64* %19
  %35 = load i64, i64* %21
  %icmpeq = icmp eq i64 %35, 0
  br i1 %icmpeq, label %ifthen2, label %ifelse2

; CHECK:  ifthen2:
; CHECK:  %token7 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record addrspace(1)*, i8 addrspace(1)*)* @g2, i32 2, i32 0, %record addrspace(1)* %arg1.reloc4.casted, i8 addrspace(1)* %arg0.reloc5) [ "gc-live"(i8 addrspace(1)* %arg2.reloc3) ]
; CHECK:  %token10 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g10, i32 1, i32 0, i8 addrspace(1)* %arg2.reloc8)
;
ifthen2:                                       ; preds = %ifend1
  call void @g2(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0)
  call void @g10(i8 addrspace(1)* %arg2)
  unreachable

ifelse2:                                       ; preds = %ifend1
  br label %ifend2

; CHECK:  ifend2:
; CHECK:  %token12 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i1 (i8 addrspace(1)*)* @g9, i32 1, i32 0, i8 addrspace(1)* %arg2.reloc3) [ "gc-live"(i8 addrspace(1)* %arg2.reloc3, %record addrspace(1)* %arg1.reloc4.casted, i8 addrspace(1)* %arg0.reloc5), "struct-live"(%record* %2, %record* %4, %record* %18) ]
;
ifend2:                                        ; preds = %ifelse2
  %36 = call i1 @g9(i8 addrspace(1)* %arg2)
  %lnot = xor i1 %36, true
  br i1 %lnot, label %ifthen3, label %ifelse3

ifthen3:                                       ; preds = %ifend2
  %37 = load i64, i64* %19
  store i64 %37, i64* %22
  br label %ifend3

ifelse3:                                       ; preds = %ifend2
  br label %ifend3

ifend3:                                        ; preds = %ifelse3, %ifthen3
  %38 = load i1, i1* %20
  br i1 %38, label %tmp1, label %tmp2

tmp2:                                         ; preds = %ifend3
  br label %tmpend1

tmp1:                                         ; preds = %ifend3
  %39 = load i64, i64* %21
  %icmpeq1 = icmp eq i64 %39, 1
  br label %tmpend1

tmpend1:                                         ; preds = %tmp1, %tmp2
  %conv = phi i1 [ false, %tmp2 ], [ %icmpeq1, %tmp1 ]
  br i1 %conv, label %ifthen4, label %ifelse4

ifthen4:                                       ; preds = %tmpend1
  %40 = load i1, i1* %20
  br i1 %40, label %ifthen5, label %ifelse5

ifthen5:                                       ; preds = %ifthen4
  %41 = load i64, i64* %22
  %add = add i64 %41, 1
  store i64 %add, i64* %22
  br label %ifend5

ifelse5:                                       ; preds = %ifthen4
  br label %ifend5

ifend5:                                        ; preds = %ifelse5, %ifthen5
  %42 = load i64, i64* %23
  %icmpslt = icmp slt i64 %42, 0
  br i1 %icmpslt, label %tmp3, label %tmp4

tmp3:                                        ; preds = %ifend5
  br label %tmpend2

tmp4:                                          ; preds = %ifend5
  %43 = load i64, i64* %23
  %44 = load i64, i64* %19
  %icmpsge = icmp sge i64 %43, %44
  br label %tmpend2

tmpend2:                                          ; preds = %tmp4, %tmp3
  %conv3 = phi i1 [ true, %tmp3 ], [ %icmpsge, %tmp4 ]
  br i1 %conv3, label %tmp5, label %tmp6

tmp5:                                        ; preds = %tmpend2
  br label %tmpend3

tmp6:                                         ; preds = %tmpend2
  %45 = load i64, i64* %22
  %icmpsle = icmp sle i64 %45, 0
  br label %tmpend3

tmpend3:                                         ; preds = %tmp6, %tmp5
  %conv7 = phi i1 [ true, %tmp5 ], [ %icmpsle, %tmp6 ]
  br i1 %conv7, label %tmp7, label %tmp8

tmp7:                                        ; preds = %tmpend3
  br label %tmpend4

tmp8:                                         ; preds = %tmpend3
  %46 = load i64, i64* %22
  %47 = load i64, i64* %19
  %icmpsgt = icmp sgt i64 %46, %47
  br label %tmpend4

tmpend4:                                        ; preds = %tmp8, %tmp7
  %conv11 = phi i1 [ true, %tmp7 ], [ %icmpsgt, %tmp8 ]
  br i1 %conv11, label %ifthen6, label %ifelse6

; CHECK:  ifthen6:
; CHECK:  %token17 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g10, i32 1, i32 0, i8 addrspace(1)* %arg2.reloc13)
;
ifthen6:                                       ; preds = %tmpend4
  call void @g10(i8 addrspace(1)* %arg2)
  unreachable

ifelse6:                                       ; preds = %tmpend4
  br label %ifend6

; CHECK:  ifend6:
; CHECK:  %token19 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (%record addrspace(1)*, i8 addrspace(1)*, i64, i64)* @g3, i32 4, i32 0, %record addrspace(1)* %arg1.reloc14.casted, i8 addrspace(1)* %arg0.reloc15, i64 %48, i64 %49) [ "struct-live"(%record* %18) ]
; CHECK:  %token21 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i64)* @g4, i32 2, i32 0, i8 addrspace(1)* %50, i64 %sub) [ "struct-live"(%record* %18) ]
;
ifend6:                                        ; preds = %ifelse6
  %48 = load i64, i64* %23
  %49 = load i64, i64* %22
  %50 = call i8 addrspace(1)* @g3(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %48, i64 %49)
  %51 = load i64, i64* %22
  %52 = load i64, i64* %23
  %sub = sub i64 %51, %52
  call void @g4(i8 addrspace(1)* %50, i64 %sub)
  %53 = bitcast %record* %0 to i8*
  %54 = bitcast %record* %18 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %53, i8* align 8 %54, i64 16, i1 false)
  ret void

ifelse4:                                       ; preds = %tmpend1
  br label %ifend4

ifend4:                                        ; preds = %ifelse4
  store i64 0, i64* %17
  store i64 0, i64* %16
  %55 = load i64, i64* %21
  %icmpsgt13 = icmp sgt i64 %55, 0
  br i1 %icmpsgt13, label %ifthen7, label %ifelse7

ifthen7:                                       ; preds = %ifend4
  %56 = load i1, i1* %20
  %lnot14 = xor i1 %56, true
  br i1 %lnot14, label %ifthen8, label %ifelse8

ifthen8:                                       ; preds = %ifthen7
  %57 = load i64, i64* %22
  %sub15 = sub i64 %57, 1
  store i64 %sub15, i64* %22
  br label %ifend8

ifelse8:                                       ; preds = %ifthen7
  br label %ifend8

ifend8:                                        ; preds = %ifelse8, %ifthen8
  %58 = load i64, i64* %23
  %icmpslt16 = icmp slt i64 %58, -1
  br i1 %icmpslt16, label %tmp9, label %tmp10

tmp9:                                       ; preds = %ifend8
  br label %tmpend5

tmp10:                                        ; preds = %ifend8
  %59 = load i64, i64* %22
  %60 = load i64, i64* %19
  %icmpsge19 = icmp sge i64 %59, %60
  br label %tmpend5

tmpend5:                                        ; preds = %tmp10, %tmp9
  %conv21 = phi i1 [ true, %tmp9 ], [ %icmpsge19, %tmp10 ]
  br i1 %conv21, label %ifthen9, label %ifelse9

; CHECK:  ifthen9:
; CHECK:  %token23 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g10, i32 1, i32 0, i8 addrspace(1)* %arg2.reloc13)
;
ifthen9:                                       ; preds = %tmpend5
  call void @g10(i8 addrspace(1)* %arg2)
  unreachable

ifelse9:                                       ; preds = %tmpend5
  br label %ifend9

ifend9:                                        ; preds = %ifelse9
  %61 = load i64, i64* %22
  %62 = load i64, i64* %23
  %sub23 = sub i64 %61, %62
  %63 = load i64, i64* %21
  %icmpsle24 = icmp sle i64 %sub23, -9223372036854775808
  %icmpeq25 = icmp eq i64 %63, -1
  br i1 %icmpsle24, label %tmp11, label %tmp12

tmp12:                                       ; preds = %ifend9
  br label %tmpend6

tmp11:                                       ; preds = %ifend9
  br label %tmpend6

tmpend6:                                       ; preds = %tmp11, %tmp12
  %conv29 = phi i1 [ false, %tmp12 ], [ %icmpeq25, %tmp11 ]
  br i1 %conv29, label %tmp13, label %tmp14

; CHECK:  tmp14:
; CHECK:  %token25 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (i64, i64)* @g8, i32 2, i32 0, i64 %sub23, i64 %63) [ "gc-live"(i8 addrspace(1)* %arg2.reloc13, %record addrspace(1)* %arg1.reloc14.casted, i8 addrspace(1)* %arg0.reloc15), "struct-live"(%record* %2, %record* %4) ]
;
tmp14:                                      ; preds = %tmpend6
  %64 = call i64 @g8(i64 %sub23, i64 %63)
  store i64 %64, i64* %1
  br label %tmpend7

tmp13:                                         ; preds = %tmpend6
  store i64 -9223372036854775808, i64* %1
  br label %tmpend7

tmpend7:                                            ; preds = %tmp13, %tmp14
  %65 = load i64, i64* %1
  %add31 = add i64 %65, 1
  store i64 %add31, i64* %17
  %66 = load i64, i64* %21
  store i64 %66, i64* %16
  %67 = load i64, i64* %22
  %add32 = add i64 %67, 1
  store i64 %add32, i64* %22
  br label %ifend7

ifelse7:                                       ; preds = %ifend4
  %68 = load i1, i1* %20
  %lnot33 = xor i1 %68, true
  br i1 %lnot33, label %ifthen10, label %ifelse10

ifthen10:                                       ; preds = %ifelse7
  %69 = load i64, i64* %22
  %add34 = add i64 %69, 1
  store i64 %add34, i64* %22
  br label %ifend10

ifelse10:                                       ; preds = %ifelse7
  br label %ifend10

ifend10:                                        ; preds = %ifelse10, %ifthen10
  %70 = load i64, i64* %23
  %71 = load i64, i64* %19
  %icmpsge35 = icmp sge i64 %70, %71
  br i1 %icmpsge35, label %tmp15, label %tmp16

tmp15:                                       ; preds = %ifend10
  br label %tmpend8

tmp16:                                        ; preds = %ifend10
  %72 = load i64, i64* %22
  %icmpslt38 = icmp slt i64 %72, 0
  br label %tmpend8

tmpend8:                                        ; preds = %tmp16, %tmp15
  %conv40 = phi i1 [ true, %tmp15 ], [ %icmpslt38, %tmp16 ]
  br i1 %conv40, label %ifthen11, label %ifelse11

; CHECK:  ifthen11:
; CHECK:  %token30 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g10, i32 1, i32 0, i8 addrspace(1)* %arg2.reloc13)
;
ifthen11:                                       ; preds = %tmpend8
  call void @g10(i8 addrspace(1)* %arg2)
  unreachable

ifelse11:                                       ; preds = %tmpend8
  br label %ifend11

ifend11:                                        ; preds = %ifelse11
  %73 = load i64, i64* %23
  %74 = load i64, i64* %22
  %sub42 = sub i64 %73, %74
  %75 = load i64, i64* %21
  %sub43 = sub i64 0, %75
  %icmpsle44 = icmp sle i64 %sub42, -9223372036854775808
  %icmpeq45 = icmp eq i64 %sub43, -1
  br i1 %icmpsle44, label %tmp17, label %tmp18

tmp18:                                       ; preds = %ifend11
  br label %tmpend9

tmp17:                                       ; preds = %ifend11
  br label %tmpend9

tmpend9:                                       ; preds = %tmp17, %tmp18
  %conv49 = phi i1 [ false, %tmp18 ], [ %icmpeq45, %tmp17 ]
  br i1 %conv49, label %tmp19, label %tmp20

; CHECK:  tmp20:
; CHECK:  %token32 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (i64, i64)* @g8, i32 2, i32 0, i64 %sub42, i64 %sub43) [ "gc-live"(i8 addrspace(1)* %arg2.reloc13, %record addrspace(1)* %arg1.reloc14.casted, i8 addrspace(1)* %arg0.reloc15), "struct-live"(%record* %2, %record* %4) ]
;
tmp20:                                    ; preds = %tmpend9
  %76 = call i64 @g8(i64 %sub42, i64 %sub43)
  store i64 %76, i64* %1
  br label %tmpend10

tmp19:                                       ; preds = %tmpend9
  store i64 -9223372036854775808, i64* %1
  br label %tmpend10

tmpend10:                                            ; preds = %tmp19, %tmp20
  %77 = load i64, i64* %1
  %add53 = add i64 %77, 1
  store i64 %add53, i64* %17
  %78 = load i64, i64* %23
  %add54 = add i64 %78, 1
  store i64 %add54, i64* %22
  %79 = load i64, i64* %23
  %80 = load i64, i64* %17
  %sub55 = sub i64 %80, 1
  %81 = load i64, i64* %21
  %mul = mul i64 %sub55, %81
  %add56 = add i64 %79, %mul
  store i64 %add56, i64* %23
  %82 = load i64, i64* %21
  %sub57 = sub i64 0, %82
  store i64 %sub57, i64* %16
  br label %ifend7

ifend7:                                        ; preds = %tmpend10, %tmpend7
  %83 = load i64, i64* %17
  %84 = load i64, i64* @data
  %mul58 = mul i64 %83, %84
  %icmpsge1 = icmp sge i64 %mul58, 0
  br i1 %icmpsge1, label %tmp21, label %tmp22

; CHECK:  tmp22:
; CHECK:  %token37 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g10, i32 1, i32 0, i8 addrspace(1)* %.265)
;
tmp22:                                  ; preds = %ifend7
  call void @g10(i8 addrspace(1)* %arg2)
  unreachable

; CHECK:  tmp21:
; CHECK:  %token39 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (%record addrspace(1)*, i8 addrspace(1)*, i64)* @g5, i32 3, i32 0, %record addrspace(1)* %.268, i8 addrspace(1)* %.2, i64 %86) [ "gc-live"(i8 addrspace(1)* %.265, %record addrspace(1)* %.268, i8 addrspace(1)* %.2), "struct-live"(%record* %2, %record* %4, i8 addrspace(1)** %15) ]
; CHECK:  %token44 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record addrspace(1)*, i8 addrspace(1)*, i64, i64)* @g6, i32 4, i32 0, %record addrspace(1)* %arg1.reloc41.casted, i8 addrspace(1)* %arg0.reloc42, i64 %86, i64 %87) [ "gc-live"(i8 addrspace(1)* %arg2.reloc40, %record addrspace(1)* %arg1.reloc41.casted), "struct-live"(%record* %2, %record* %4, i8 addrspace(1)** %15) ]
;
tmp21:                                   ; preds = %ifend7
  %85 = bitcast i8 addrspace(1)* %arg2 to %array addrspace(1)*
  store i8 addrspace(1)* %arg2, i8 addrspace(1)** %15
  %86 = load i64, i64* %23
  %87 = call i64 @g5(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %86)
  call void @g6(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0, i64 %86, i64 %87)
  %88 = getelementptr inbounds %struct, %struct* %14, i32 0, i32 0
  %89 = load i64, i64* %88
  store i64 %89, i64* %13
  %90 = getelementptr inbounds %struct, %struct* %14, i32 0, i32 1
  %91 = load i64, i64* %90
  store i64 %91, i64* %12
  store i64 0, i64* %11
  store i64 0, i64* %10
  store i64 0, i64* %9
  %92 = load i64, i64* %23
  store i64 %92, i64* %8
  %93 = load i64, i64* %22
  store i64 %93, i64* %7
  store i1 true, i1* %6
  br label %body2

body2:                                    ; preds = %ifend12, %tmp21
  %94 = load i64, i64* %8
  %95 = load i64, i64* %7
  %icmpslt61 = icmp slt i64 %94, %95
  br i1 %icmpslt61, label %ifthen12, label %ifelse12

ifthen12:                                       ; preds = %body2
  %96 = load i1, i1* %6
  br i1 %96, label %ifthen13, label %ifelse13

ifthen13:                                       ; preds = %ifthen12
  store i1 false, i1* %6
  br label %ifend13

ifelse13:                                       ; preds = %ifthen12
  %97 = load i64, i64* %8
  %add62 = add i64 %97, 1
  store i64 %add62, i64* %8
  br label %ifend13

ifend13:                                        ; preds = %ifelse13, %ifthen13
  %98 = load i64, i64* %8
  %99 = load i64, i64* %7
  %icmpslt63 = icmp slt i64 %98, %99
  br i1 %icmpslt63, label %ifthen14, label %ifelse14

; CHECK:  ifthen14:
; CHECK:  %token48 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i64 (i8 addrspace(1)*, i64)* @g12, i32 2, i32 0, i8 addrspace(1)* %101, i64 %102) [ "gc-live"(i8 addrspace(1)* %.3, %record addrspace(1)* %.369), "struct-live"(%record* %2, %record* %4, i8 addrspace(1)** %15) ]
;
ifthen14:                                       ; preds = %ifend13
  %100 = getelementptr inbounds %record, %record addrspace(1)* %arg1, i32 0, i32 0
  %101 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %100
  %102 = load i64, i64* %13
  %103 = call i64 @g12(i8 addrspace(1)* %101, i64 %102)
  store i64 %103, i64* %9
  %104 = load i64, i64* %11
  %105 = load i64, i64* %16
  %icmpeq64 = icmp eq i64 %104, %105
  br i1 %icmpeq64, label %tmp23, label %tmp24

tmp23:                                       ; preds = %ifthen14
  br label %tmpend12

tmp24:                                        ; preds = %ifthen14
  %106 = load i64, i64* %11
  %icmpeq67 = icmp eq i64 %106, 0
  br label %tmpend12

tmpend12:                                        ; preds = %tmp24, %tmp23
  %conv69 = phi i1 [ true, %tmp23 ], [ %icmpeq67, %tmp24 ]
  br i1 %conv69, label %ifthen15, label %ifelse15

; CHECK:  ifthen15:
; CHECK:  %token52 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, i64, i64, i64)* @g7, i32 5, i32 0, i8 addrspace(1)* %108, i8 addrspace(1)* %109, i64 %110, i64 %111, i64 %112) [ "gc-live"(i8 addrspace(1)* %arg2.reloc49, %record addrspace(1)* %arg1.reloc50.casted), "struct-live"(%record* %2, %record* %4, i8 addrspace(1)** %15) ]
;
ifthen15:                                       ; preds = %tmpend12
  %107 = getelementptr inbounds %record, %record addrspace(1)* %arg1, i32 0, i32 0
  %108 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %107
  %109 = load i8 addrspace(1)*, i8 addrspace(1)** %15
  %110 = load i64, i64* %13
  %111 = load i64, i64* %10
  %112 = load i64, i64* %9
  call void @g7(i8 addrspace(1)* %108, i8 addrspace(1)* %109, i64 %110, i64 %111, i64 %112)
  %113 = load i64, i64* %10
  %114 = load i64, i64* %9
  %add71 = add i64 %113, %114
  store i64 %add71, i64* %10
  store i64 1, i64* %11
  br label %ifend15

ifelse15:                                       ; preds = %tmpend12
  %115 = load i64, i64* %11
  %add72 = add i64 %115, 1
  store i64 %add72, i64* %11
  br label %ifend15

ifend15:                                        ; preds = %ifelse15, %ifthen15
  %116 = load i64, i64* %13
  %117 = load i64, i64* %9
  %add73 = add i64 %116, %117
  store i64 %add73, i64* %13
  br label %ifend14

ifelse14:                                       ; preds = %ifend13
  br label %ifend14

ifend14:                                        ; preds = %ifelse14, %ifend15
  br label %ifend12

ifelse12:                                       ; preds = %body2
  br label %end1

ifend12:                                        ; preds = %ifend14
  br label %body2

end1:                                     ; preds = %ifelse12
  %118 = load i64, i64* %10
  %119 = add i64 %118, 1
  %icmpsge2 = icmp sge i64 %119, 0
  br i1 %icmpsge2, label %tmp25, label %tmp26

; CHECK:  tmp26:
; CHECK:  %token56 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g10, i32 1, i32 0, i8 addrspace(1)* %.3)
;
tmp26:                                ; preds = %end1
  call void @g10(i8 addrspace(1)* %arg2)
  unreachable

; CHECK:  tmp25:
; CHECK:  %token58 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, i64, i64, i64)* @g7, i32 5, i32 0, i8 addrspace(1)* %121, i8 addrspace(1)* %122, i64 0, i64 0, i64 %123) [ "struct-live"(%record* %2, %record* %4, i8 addrspace(1)** %5) ]
; CHECK:  %token60 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i64)* @g4, i32 2, i32 0, i8 addrspace(1)* %124, i64 %125) [ "struct-live"(%record* %2, %record* %4) ]
;
tmp25:                                 ; preds = %end1
  %120 = bitcast i8 addrspace(1)* %arg2 to %array addrspace(1)*
  store i8 addrspace(1)* %arg2, i8 addrspace(1)** %5
  %121 = load i8 addrspace(1)*, i8 addrspace(1)** %15
  %122 = load i8 addrspace(1)*, i8 addrspace(1)** %5
  %123 = load i64, i64* %10
  call void @g7(i8 addrspace(1)* %121, i8 addrspace(1)* %122, i64 0, i64 0, i64 %123)
  %124 = load i8 addrspace(1)*, i8 addrspace(1)** %5
  %125 = load i64, i64* %17
  call void @g4(i8 addrspace(1)* %124, i64 %125)
  %126 = bitcast %record* %3 to i8*
  %127 = bitcast %record* %4 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %126, i8* align 8 %127, i64 16, i1 false)
  %128 = load i64, i64* %21
  %icmpslt79 = icmp slt i64 %128, 0
  br i1 %icmpslt79, label %ifthen16, label %ifelse16

; CHECK:  ifthen16:
; CHECK:  %token62 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g10, i32 1, i32 0, i8 addrspace(1)* null) [ "struct-live"(%record* %2) ]
;
ifthen16:                                       ; preds = %tmp25
  %129 = addrspacecast %record* %3 to %record addrspace(1)*
  call void @g10(i8 addrspace(1)* null)
  %130 = bitcast %record* %3 to i8*
  %131 = bitcast %record* %2 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %130, i8* align 8 %131, i64 16, i1 false)
  br label %ifend16

ifelse16:                                       ; preds = %tmp25
  br label %ifend16

ifend16:                                        ; preds = %ifelse16, %ifthen16
  %132 = bitcast %record* %0 to i8*
  %133 = bitcast %record* %3 to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* align 8 %132, i8* align 8 %133, i64 16, i1 false)
  ret void
}

attributes #0 = { "record_mut" }
