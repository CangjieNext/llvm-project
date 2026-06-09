; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%record = type { i8 addrspace(1)*, i64 }
%object = type { %record, i64 }
%array = type { %object, [0 x i32] }

declare void @g0() #0 gc "cangjie"
declare void @g1(i8* %arg0, i64 %arg1, i64 %arg2) #0 gc "cangjie"
declare i8 addrspace(1)* @g2(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1) #0 gc "cangjie"
declare void @g3(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare i8 addrspace(1)* @g4(i64 %arg0, i8* %arg1) #0 gc "cangjie"
declare i8 addrspace(1)* @g5(i8* %arg0, i32 %arg1) #0 gc "cangjie"
declare void @g6(i8 addrspace(1)* %arg) #0 gc "cangjie"
declare void @g7(i8 addrspace(1)* %arg0, i8 addrspace(1)* %arg1, i8 addrspace(1)* addrspace(1)* %arg2) #0 gc "cangjie"
declare void @g8(i64 %arg0, i8 addrspace(1)* %arg1, i64 addrspace(1)* %arg2) #0 gc "cangjie"
declare i8 addrspace(1)* @g9(i8 addrspace(1)* %arg0, i64 %arg1, i8 addrspace(1)* %arg2) #0 gc "cangjie"
declare void @g10(i8 addrspace(1)* %arg0, i8 addrspace(1)* %arg1, %record addrspace(1)* %arg2) #0 gc "cangjie"
declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly %arg0, i8 %arg1, i64 %arg2, i1 immarg %arg3)
declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %arg0, i64 %arg1)
declare { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %arg0, i64 %arg1)

define i64 @foo(i8 addrspace(1)* %arg0, %record addrspace(1)* %arg1, i64 %arg2, i8* %arg3) #0 gc "cangjie" {
; CHECK-LABEL: foo
; CHECK:  entry:
; CHECK:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @g0, i32 0, i32 0) [ "gc-live"(i8 addrspace(1)* %arg0) ]
;
entry:
  %0 = alloca %record
  %1 = alloca %record
  %2 = alloca %record
  %3 = alloca %record
  %4 = alloca %record
  %5 = alloca %record
  %6 = alloca %record
  %7 = alloca %record
  %8 = alloca %record
  %9 = alloca %record
  %10 = alloca %record
  %11 = alloca %record
  %12 = alloca %record
  %13 = alloca %record
  %14 = alloca %record
  %15 = alloca %record
  %16 = alloca %record
  %17 = alloca %record
  %18 = alloca %record
  %19 = alloca %record
  %20 = alloca %record
  %21 = alloca %record
  %22 = alloca %record
  %23 = alloca %record
  %24 = bitcast %record* %22 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %24, i8 0, i64 16, i1 false)
  %25 = bitcast %record* %23 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %25, i8 0, i64 16, i1 false)
  %26 = getelementptr inbounds i8, i8 addrspace(1)* %arg0, i64 56
  %27 = bitcast i8 addrspace(1)* %26 to %array addrspace(1)* addrspace(1)*
  %28 = load %array addrspace(1)*, %array addrspace(1)* addrspace(1)* %27
  %29 = getelementptr inbounds %array, %array addrspace(1)* %28, i64 0, i32 1, i64 %arg2
  %30 = load i32, i32 addrspace(1)* %29
  call void @g0()
  switch i32 %30, label %tmp38 [
    i32 35, label %ifthen1
    i32 34, label %ifthen2
  ]

ifthen1:                                      ; preds = %entry
  %31 = bitcast %record* %21 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %31, i8 0, i64 16, i1 false)
  %32 = bitcast %record* %17 to i8*
  %33 = bitcast %record* %18 to i8*
  %34 = bitcast %record* %19 to i8*
  %35 = bitcast %record* %20 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %32, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %33, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %34, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %35, i8 0, i64 16, i1 false)
  %36 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %arg2, i64 1)
  %conv1 = extractvalue { i64, i1 } %36, 1
  br i1 %conv1, label %ifthen3, label %ifelse3

ifelse3:                                    ; preds = %ifthen1
  %37 = load %array addrspace(1)*, %array addrspace(1)* addrspace(1)* %27
  br label %body1

; CHECK:  ifthen3:
; CHECK:  %token39 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %20) ]
; CHECK:  %token41 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %38) [ "struct-live"(%record* %20) ]
; CHECK:  %token43 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %39)
;
ifthen3:                                     ; preds = %ifthen1
  call void @g1(i8* %arg3, i64 0, i64 0)
  %38 = addrspacecast %record* %20 to %record addrspace(1)*
  %39 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %38)
  call void @g3(i8 addrspace(1)* %39)
  unreachable

body1:                               ; preds = %ifthen7, %ifelse3
  %40 = phi { i64, i1 } [ %45, %ifthen7 ], [ %36, %ifelse3 ]
  %41 = extractvalue { i64, i1 } %40, 0
  %42 = getelementptr inbounds %array, %array addrspace(1)* %37, i64 0, i32 1, i64 %41
  %43 = load i32, i32 addrspace(1)* %42
  switch i32 %43, label %ifthen5 [
    i32 34, label %ifthen6
    i32 35, label %ifthen7
  ]

; CHECK:  ifthen5:
; CHECK:  %token45 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i64, i8*)* @g4, i32 2, i32 0, i64 0, i8* %arg3) [ "gc-live"(i8 addrspace(1)* %.0), "struct-live"(%record* %17, %record* %21) ]
;
ifthen5:                                  ; preds = %body1
  %44 = call i8 addrspace(1)* @g4(i64 0, i8* %arg3)
  br label %body2

; CHECK:  ifthen7:
; CHECK:  %token48 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @g0, i32 0, i32 0) [ "gc-live"(i8 addrspace(1)* %.0, %array addrspace(1)* %.0185), "struct-live"(%record* %19, %record* %17, %record* %21, %record* %18) ]
;
ifthen7:                                   ; preds = %body1
  %45 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %41, i64 1)
  %conv2 = extractvalue { i64, i1 } %45, 1
  call void @g0()
  br i1 %conv2, label %ifthen8, label %body1

; CHECK:  ifthen8:
; CHECK:  %token51 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %19) ]
; CHECK:  %token53 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %48) [ "struct-live"(%record* %19) ]
; CHECK:  %token55 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %49)
;
ifthen8:                                   ; preds = %ifthen7
  call void @g1(i8* %arg3, i64 0, i64 0)
  %46 = addrspacecast %record* %19 to %record addrspace(1)*
  %47 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %46)
  call void @g3(i8 addrspace(1)* %47)
  unreachable

ifthen6:                                ; preds = %body1
  %48 = extractvalue { i64, i1 } %40, 0
  %49 = call { i64, i1 } @llvm.ssub.with.overflow.i64(i64 %48, i64 %arg2)
  %conv3 = extractvalue { i64, i1 } %49, 1
  br i1 %conv3, label %ifthen9, label %ifelse9

ifelse9:                                ; preds = %ifthen6
  %conv24 = extractvalue { i64, i1 } %49, 0
  %50 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %conv24, i64 1)
  %conv4 = extractvalue { i64, i1 } %50, 0
  %conv5 = extractvalue { i64, i1 } %50, 1
  br i1 %conv5, label %ifthen10, label %ifelse10

; CHECK:  ifthen9:
; CHECK:  %token57 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %18) ]
; CHECK:  %token59 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %53) [ "struct-live"(%record* %18) ]
; CHECK:  %token61 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %54)
;
ifthen9:                                   ; preds = %ifthen6
  call void @g1(i8* %arg3, i64 0, i64 0)
  %51 = addrspacecast %record* %18 to %record addrspace(1)*
  %52 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %51)
  call void @g3(i8 addrspace(1)* %52)
  unreachable

ifelse10:                                ; preds = %ifelse9
  %icmpsgt1 = icmp sgt i64 %conv4, -1
  br i1 %icmpsgt1, label %tmp1, label %tmp2

; CHECK:  ifthen10:
; CHECK:  %token63 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %17) ]
; CHECK:  %token65 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %55) [ "struct-live"(%record* %17) ]
; CHECK:  %token67 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %56)
;
ifthen10:                                   ; preds = %ifelse9
  call void @g1(i8* %arg3, i64 0, i64 0)
  %53 = addrspacecast %record* %17 to %record addrspace(1)*
  %54 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %53)
  call void @g3(i8 addrspace(1)* %54)
  unreachable

; CHECK:  tmp2:
; CHECK:  %token69 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @g5, i32 2, i32 0, i8* %arg3, i32 40)
; CHECK:  %token71 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g6, i32 1, i32 0, i8 addrspace(1)* %57) [ "gc-live"(i8 addrspace(1)* %57) ]
; CHECK:  %token73 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %58)
;
tmp2:                              ; preds = %ifelse10
  %55 = call i8 addrspace(1)* @g5(i8* %arg3, i32 40)
  call void @g6(i8 addrspace(1)* %55)
  call void @g3(i8 addrspace(1)* %55)
  unreachable

; CHECK:  tmp1:
; CHECK:  %token75 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i64, i8*)* @g4, i32 2, i32 0, i64 %conv4, i8* %arg3) [ "gc-live"(i8 addrspace(1)* %.0), "struct-live"(%record* %17, %record* %21) ]
; CHECK:  %token78 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @g5, i32 2, i32 0, i8* %arg3, i32 24) [ "gc-live"(i8 addrspace(1)* %arg0.reloc76, i8 addrspace(1)* %59), "struct-live"(%record* %17, %record* %21) ]
;
tmp1:                               ; preds = %ifelse10
  %56 = call i8 addrspace(1)* @g4(i64 %conv4, i8* %arg3)
  %57 = call i8 addrspace(1)* @g5(i8* %arg3, i32 24)
  %58 = getelementptr inbounds i8, i8 addrspace(1)* %57, i64 8
  %59 = bitcast i8 addrspace(1)* %58 to i8 addrspace(1)* addrspace(1)*
  %60 = load i8, i8 addrspace(1)* %58
  %icmpsle1 = icmp sle i8 %60, 8
  br i1 %icmpsle1, label %tmp3, label %tmp4

; CHECK:  tmp4:
; CHECK-NEXT: [[REMAT0:%.*]] = getelementptr inbounds i8, i8 addrspace(1)* [[TMP0:%.*]], i64 8
; CHECK-NEXT: [[REMAT1:%.*]] = bitcast i8 addrspace(1)* [[REMAT0]] to i8 addrspace(1)* addrspace(1)*
; CHECK-NEXT:  %token81 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)* @g7, i32 3, i32 0, i8 addrspace(1)* %arg0.reloc79, i8 addrspace(1)* %60, i8 addrspace(1)* addrspace(1)* %.remat35) [ "gc-live"(i8 addrspace(1)* %arg0.reloc79, i8 addrspace(1)* %61, i8 addrspace(1)* %60), "struct-live"(%record* %17, %record* %21) ]
;
tmp4:                                               ; preds = %tmp1
  call void @g7(i8 addrspace(1)* %arg0, i8 addrspace(1)* %57, i8 addrspace(1)* addrspace(1)* %59)
  br label %tmpend2

tmp3:                                               ; preds = %tmp1
  store i8 addrspace(1)* %arg0, i8 addrspace(1)* addrspace(1)* %59
  br label %tmpend2

tmpend2:                                               ; preds = %tmp3, %tmp4
  %61 = getelementptr inbounds i8, i8 addrspace(1)* %57, i64 16
  %62 = bitcast i8 addrspace(1)* %61 to i64 addrspace(1)*
  br i1 %icmpsle1, label %tmp5, label %tmp6

; CHECK:  tmp6:
; CHECK-NEXT: [[REMAT2:%.*]] = getelementptr inbounds i8, i8 addrspace(1)* [[TMP1:%.*]], i64 16
; CHECK-NEXT: [[REMAT3:%.*]] = bitcast i8 addrspace(1)* [[REMAT2]] to i64 addrspace(1)*
; CHECK:  %token84 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i64, i8 addrspace(1)*, i64 addrspace(1)*)* @g8, i32 3, i32 0, i64 %arg2, i8 addrspace(1)* %.0191, i64 addrspace(1)* %.remat29) [ "gc-live"(i8 addrspace(1)* %.1, i8 addrspace(1)* %.0187, i8 addrspace(1)* %.0191), "struct-live"(%record* %17, %record* %21) ]
;
tmp6:                                               ; preds = %tmpend2
  call void @g8(i64 %arg2, i8 addrspace(1)* %57, i64 addrspace(1)* %62)
  br label %tmpend3

tmp5:                                               ; preds = %tmpend2
  store i64 %arg2, i64 addrspace(1)* %62
  br label %tmpend3

; CHECK:  tmpend3:
; CHECK:  %token87 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @g5, i32 2, i32 0, i8* %arg3, i32 24) [ "gc-live"(i8 addrspace(1)* %.2, i8 addrspace(1)* %.1188, i8 addrspace(1)* %.1192), "struct-live"(%record* %17, %record* %21) ]
;
tmpend3:                                               ; preds = %tmp5, %tmp6
  %63 = call i8 addrspace(1)* @g5(i8* %arg3, i32 24)
  %64 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %59
  %65 = getelementptr inbounds i8, i8 addrspace(1)* %63, i64 8
  %66 = bitcast i8 addrspace(1)* %65 to i8 addrspace(1)* addrspace(1)*
  %67 = load i8, i8 addrspace(1)* %65
  %icmpsle2 = icmp sle i8 %67, 8
  br i1 %icmpsle2, label %tmp7, label %tmp8

; CHECK:  tmp8:
; CHECK:  %token90 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)* @g7, i32 3, i32 0, i8 addrspace(1)* %74, i8 addrspace(1)* %71, i8 addrspace(1)* addrspace(1)* %76) [ "gc-live"(i8 addrspace(1)* %arg0.reloc88, i8 addrspace(1)* %72, i8 addrspace(1)* %71, i8 addrspace(1)* %73), "struct-live"(%record* %17, %record* %21) ]
;
tmp8:                                               ; preds = %tmpend3
  call void @g7(i8 addrspace(1)* %64, i8 addrspace(1)* %63, i8 addrspace(1)* addrspace(1)* %66)
  br label %tmpend4

tmp7:                                               ; preds = %tmpend3
  store i8 addrspace(1)* %64, i8 addrspace(1)* addrspace(1)* %66
  br label %tmpend4

tmpend4:                                               ; preds = %tmp7, %tmp8
  %68 = load i64, i64 addrspace(1)* %62
  %69 = getelementptr inbounds i8, i8 addrspace(1)* %63, i64 16
  %70 = bitcast i8 addrspace(1)* %69 to i64 addrspace(1)*
  br i1 %icmpsle2, label %tmp9, label %tmp10

; CHECK:  tmp10:
; CHECK:  %token93 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i64, i8 addrspace(1)*, i64 addrspace(1)*)* @g8, i32 3, i32 0, i64 %81, i8 addrspace(1)* %.0194, i64 addrspace(1)* %83) [ "gc-live"(i8 addrspace(1)* %.3, i8 addrspace(1)* %.2189, i8 addrspace(1)* %.0194), "struct-live"(%record* %17, %record* %21) ]
;
tmp10:                                               ; preds = %tmpend4
  call void @g8(i64 %68, i8 addrspace(1)* %63, i64 addrspace(1)* %70)
  br label %tmpend5

tmp9:                                               ; preds = %tmpend4
  store i64 %68, i64 addrspace(1)* %70
  br label %tmpend5

; CHECK:  tmpend5:
; CHECK:  %token96 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, i64, i8 addrspace(1)*)* @g9, i32 3, i32 0, i8 addrspace(1)* %.3190, i64 %conv4, i8 addrspace(1)* %.1195) [ "gc-live"(i8 addrspace(1)* %.4), "struct-live"(%record* %17, %record* %21) ]
;
tmpend5:                                               ; preds = %tmp9, %tmp10
  %71 = call i8 addrspace(1)* @g9(i8 addrspace(1)* %56, i64 %conv4, i8 addrspace(1)* %63)
  br label %body2

body2:                                              ; preds = %tmpend5, %ifthen5
  %72 = phi i8 addrspace(1)* [ %44, %ifthen5 ], [ %71, %tmpend5 ]
  %73 = getelementptr inbounds i8, i8 addrspace(1)* %72, i64 8
  %74 = bitcast i8 addrspace(1)* %73 to i64 addrspace(1)*
  %75 = load i64, i64 addrspace(1)* %74
  %icmpeq1 = icmp eq i64 %75, 0
  br i1 %icmpeq1, label %tmp11, label %tmp12

tmp12:                                               ; preds = %body2
  %76 = load %array addrspace(1)*, %array addrspace(1)* addrspace(1)* %27
  %77 = getelementptr inbounds %array, %array addrspace(1)* %76, i64 0, i32 1, i64 %arg2
  %78 = load i32, i32 addrspace(1)* %77
  %79 = getelementptr inbounds i8, i8 addrspace(1)* %arg0, i64 8
  %80 = bitcast i8 addrspace(1)* %79 to i32 addrspace(1)*
  %81 = load i32, i32 addrspace(1)* %80
  %icmpeq2 = icmp eq i32 %78, %81
  br i1 %icmpeq2, label %tmp13, label %tmp14

tmp14:                               ; preds = %tmp12
  %82 = bitcast %record* %12 to i8*
  %83 = bitcast %record* %13 to i8*
  %84 = bitcast %record* %14 to i8*
  %85 = bitcast %record* %15 to i8*
  %86 = bitcast %record* %16 to i8*
  %87 = bitcast i8 addrspace(1)* %72 to %array addrspace(1)*
  br label %tmpend7

; CHECK:  tmp13:
; CHECK:  %token99 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @g5, i32 2, i32 0, i8* %arg3, i32 40) [ "struct-live"(%record* %17) ]
; CHECK:  %token101 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, %record addrspace(1)*)* @g10, i32 3, i32 0, i8 addrspace(1)* %103, i8 addrspace(1)* null, %record addrspace(1)* %104) [ "gc-live"(i8 addrspace(1)* %103), "struct-live"(%record* %17) ]
; CHECK:  %token103 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %105)
;
tmp13:                                    ; preds = %ifelse16, %tmp12
  %88 = call i8 addrspace(1)* @g5(i8* %arg3, i32 40)
  %89 = addrspacecast %record* %17 to %record addrspace(1)*
  call void @g10(i8 addrspace(1)* %88, i8 addrspace(1)* null, %record addrspace(1)* %89)
  call void @g3(i8 addrspace(1)* %88)
  unreachable

tmpend7:                                     ; preds = %ifelse16, %tmp14
  %90 = phi %array addrspace(1)* [ %76, %tmp14 ], [ %117, %ifelse16 ]
  %91 = phi i32 [ %78, %tmp14 ], [ %120, %ifelse16 ]
  %92 = phi i64 [ %arg2, %tmp14 ], [ %conv17, %ifelse16 ]
  %icmpeq3 = icmp eq i32 %91, 34
  br i1 %icmpeq3, label %tmp15, label %tmp16

tmp15:                                    ; preds = %tmpend7
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %82, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %83, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %84, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %85, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %86, i8 0, i64 16, i1 false)
  %93 = load i64, i64 addrspace(1)* %74
  %94 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %93, i64 -1)
  %conv6 = extractvalue { i64, i1 } %94, 1
  br i1 %conv6, label %ifthen11, label %ifelse11

ifelse11:                     ; preds = %tmp15
  %95 = load %array addrspace(1)*, %array addrspace(1)* addrspace(1)* %27
  br label %ifend11

; CHECK:  ifthen11:
; CHECK:  %token105 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %16) ]
; CHECK:  %token107 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %112) [ "struct-live"(%record* %16) ]
; CHECK:  %token109 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %113)
;
ifthen11:                                    ; preds = %tmp15
  call void @g1(i8* %arg3, i64 0, i64 0)
  %96 = addrspacecast %record* %16 to %record addrspace(1)*
  %97 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %96)
  call void @g3(i8 addrspace(1)* %97)
  unreachable

ifend11:                               ; preds = %ifelse12, %ifelse11
  %98 = phi { i64, i1 } [ %106, %ifelse12 ], [ %94, %ifelse11 ]
  %99 = phi i64 [ %conv25, %ifelse12 ], [ %92, %ifelse11 ]
  %100 = extractvalue { i64, i1 } %98, 0
  %101 = getelementptr inbounds %array, %array addrspace(1)* %95, i64 0, i32 1, i64 %99
  %102 = load i32, i32 addrspace(1)* %101
  %103 = getelementptr inbounds %array, %array addrspace(1)* %87, i64 0, i32 1, i64 %100
  %104 = load i32, i32 addrspace(1)* %103
  %icmpeq4 = icmp eq i32 %102, %104
  br i1 %icmpeq4, label %tmp17, label %tmp18

tmp18:                                    ; preds = %ifend11
  br label %tmp16

tmp17:                                   ; preds = %ifend11
  %icmpeq5 = icmp eq i64 %100, 0
  br i1 %icmpeq5, label %tmp19, label %tmp20

tmp20:                                   ; preds = %tmp17
  %105 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %99, i64 1)
  %conv7 = extractvalue { i64, i1 } %105, 1
  br i1 %conv7, label %ifthen12, label %ifelse12

; CHECK:  ifelse12:
; CHECK:  %token111 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @g0, i32 0, i32 0) [ "gc-live"(%array addrspace(1)* %.0197, i8 addrspace(1)* %.7, i8 addrspace(1)* %.1199), "struct-live"(%record* %14, %record* %21, %record* %13, %record* %12, %record* %17, %record* %15) ]
;
ifelse12:                                 ; preds = %tmp20
  %conv25 = extractvalue { i64, i1 } %105, 0
  %106 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %100, i64 -1)
  %conv8 = extractvalue { i64, i1 } %106, 1
  call void @g0()
  br i1 %conv8, label %ifthen13, label %ifend11

; CHECK:  ifthen12:
; CHECK:  %token114 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %15) ]
; CHECK:  %token116 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %126) [ "struct-live"(%record* %15) ]
; CHECK:  %token118 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %127)
;
ifthen12:                                    ; preds = %tmp20
  call void @g1(i8* %arg3, i64 0, i64 0)
  %107 = addrspacecast %record* %15 to %record addrspace(1)*
  %108 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %107)
  call void @g3(i8 addrspace(1)* %108)
  unreachable

; CHECK:  ifthen13:
; CHECK:  %token120 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %14) ]
; CHECK:  %token122 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %128) [ "struct-live"(%record* %14) ]
; CHECK:  %token124 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %129)
;
ifthen13:                                   ; preds = %ifelse12
  call void @g1(i8* %arg3, i64 0, i64 0)
  %109 = addrspacecast %record* %14 to %record addrspace(1)*
  %110 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %109)
  call void @g3(i8 addrspace(1)* %110)
  unreachable

tmp19:                               ; preds = %tmp17
  %111 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %92, i64 %93)
  %conv9 = extractvalue { i64, i1 } %111, 1
  br i1 %conv9, label %ifthen14, label %ifelse14

ifelse14:                                ; preds = %tmp19
  %conv26 = extractvalue { i64, i1 } %111, 0
  %112 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %conv26, i64 -1)
  %conv10 = extractvalue { i64, i1 } %112, 1
  br i1 %conv10, label %ifthen15, label %ifelse15

; CHECK:  ifthen14:
; CHECK:  %token126 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %13) ]
; CHECK:  %token128 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %132) [ "struct-live"(%record* %13) ]
; CHECK:  %token130 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %133)
;
ifthen14:                                   ; preds = %tmp19
  call void @g1(i8* %arg3, i64 0, i64 0)
  %113 = addrspacecast %record* %13 to %record addrspace(1)*
  %114 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %113)
  call void @g3(i8 addrspace(1)* %114)
  unreachable

; CHECK:  ifthen15:
; CHECK:  %token132 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %12) ]
; CHECK:  %token134 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %134) [ "struct-live"(%record* %12) ]
; CHECK:  %token136 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %135)
;
ifthen15:                                   ; preds = %ifelse14
  call void @g1(i8* %arg3, i64 0, i64 0)
  %115 = addrspacecast %record* %12 to %record addrspace(1)*
  %116 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %115)
  call void @g3(i8 addrspace(1)* %116)
  unreachable

ifelse15:                                   ; preds = %ifelse14
  %conv27 = extractvalue { i64, i1 } %112, 0
  %icmpsgt2 = icmp sgt i64 %conv27, %92
  br i1 %icmpsgt2, label %body3, label %tmp16

tmp16:                                     ; preds = %ifelse15, %tmp18, %tmpend7
  %117 = phi %array addrspace(1)* [ %95, %tmp18 ], [ %90, %tmpend7 ], [ %95, %ifelse15 ]
  %118 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %92, i64 1)
  %conv11 = extractvalue { i64, i1 } %118, 1
  br i1 %conv11, label %ifthen16, label %ifelse16

; CHECK:  ifelse16:
; CHECK:  %token138 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @g0, i32 0, i32 0) [ "gc-live"(%array addrspace(1)* %base_phi, %array addrspace(1)* %136, i8 addrspace(1)* %.8, i8 addrspace(1)* %.2200), "struct-live"(%record* %17, %record* %21) ]
;
ifelse16:                                    ; preds = %tmp16
  %conv17 = extractvalue { i64, i1 } %118, 0
  %119 = getelementptr inbounds %array, %array addrspace(1)* %117, i64 0, i32 1, i64 %conv17
  %120 = load i32, i32 addrspace(1)* %119
  %121 = load i32, i32 addrspace(1)* %80
  %icmpeq6 = icmp eq i32 %120, %121
  call void @g0()
  br i1 %icmpeq6, label %tmp13, label %tmpend7

; CHECK:  ifthen16:
; CHECK:  %token141 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %21) ]
; CHECK:  %token143 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %144) [ "struct-live"(%record* %21) ]
; CHECK:  %token145 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %145)
;
ifthen16:                                       ; preds = %tmp16
  call void @g1(i8* %arg3, i64 0, i64 0)
  %122 = addrspacecast %record* %21 to %record addrspace(1)*
  %123 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %122)
  call void @g3(i8 addrspace(1)* %123)
  unreachable

body3:                                         ; preds = %ifelse15
  %conv12 = extractvalue { i64, i1 } %112, 0
  br label %tmp11

tmp11:                                      ; preds = %body3, %body2
  %124 = phi i64 [ %arg2, %body2 ], [ %conv12, %body3 ]
  %icmpeq7 = icmp eq i64 %124, -1
  br i1 %icmpeq7, label %ifthen4, label %tmp38

; CHECK:  ifthen4:
; CHECK:  %token147 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @g5, i32 2, i32 0, i8* %arg3, i32 40) [ "struct-live"(%record* %17) ]
; CHECK:  %token149 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, %record addrspace(1)*)* @g10, i32 3, i32 0, i8 addrspace(1)* %147, i8 addrspace(1)* null, %record addrspace(1)* %148) [ "gc-live"(i8 addrspace(1)* %147), "struct-live"(%record* %17) ]
; CHECK:  %token151 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %149)
;
ifthen4:                                      ; preds = %tmp11
  %125 = call i8 addrspace(1)* @g5(i8* %arg3, i32 40)
  %126 = addrspacecast %record* %17 to %record addrspace(1)*
  call void @g10(i8 addrspace(1)* %125, i8 addrspace(1)* null, %record addrspace(1)* %126)
  call void @g3(i8 addrspace(1)* %125)
  unreachable

tmp38:                                       ; preds = %entry, %end1, %tmp11
  %127 = phi i64 [ %124, %tmp11 ], [ %195, %end1 ], [ %arg2, %entry ]
  ret i64 %127

ifthen2:                                      ; preds = %entry
  %128 = bitcast %record* %6 to i8*
  %129 = bitcast %record* %7 to i8*
  %130 = bitcast %record* %8 to i8*
  %131 = bitcast %record* %9 to i8*
  %132 = bitcast %record* %10 to i8*
  %133 = bitcast %record* %11 to i8*
  %a0 = bitcast %record* %17 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %128, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %129, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %130, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %131, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %132, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %133, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %a0, i8 0, i64 16, i1 false)
  %134 = getelementptr inbounds i8, i8 addrspace(1)* %arg0, i64 64
  %135 = bitcast i8 addrspace(1)* %134 to i64 addrspace(1)*
  %136 = load i64, i64 addrspace(1)* %135
  %137 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %arg2, i64 3)
  %conv13 = extractvalue { i64, i1 } %137, 0
  %conv14 = extractvalue { i64, i1 } %137, 1
  br i1 %conv14, label %ifthen17, label %ifelse17

ifelse17:                                  ; preds = %ifthen2
  %icmpsgt3 = icmp sgt i64 %136, %conv13
  br i1 %icmpsgt3, label %tmp21, label %tmp22

; CHECK:  ifthen17:
; CHECK:  %token153 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %11) ]
; CHECK:  %token155 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %161) [ "struct-live"(%record* %11) ]
; CHECK:  %token157 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %162)
;
ifthen17:                                     ; preds = %ifthen2
  call void @g1(i8* %arg3, i64 0, i64 0)
  %138 = addrspacecast %record* %11 to %record addrspace(1)*
  %139 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %138)
  call void @g3(i8 addrspace(1)* %139)
  unreachable

tmp21:                                   ; preds = %ifelse17
  %140 = add nsw i64 %arg2, 1
  %141 = load %array addrspace(1)*, %array addrspace(1)* addrspace(1)* %27
  %142 = getelementptr inbounds %array, %array addrspace(1)* %141, i64 0, i32 1, i64 %140
  %143 = load i32, i32 addrspace(1)* %142
  %icmpeq8 = icmp eq i32 %143, 34
  br i1 %icmpeq8, label %tmp23, label %tmp24

tmp23:                                  ; preds = %tmp21
  %144 = add nsw i64 %arg2, 2
  %145 = getelementptr inbounds %array, %array addrspace(1)* %141, i64 0, i32 1, i64 %144
  %146 = load i32, i32 addrspace(1)* %145
  %icmpeq9 = icmp eq i32 %146, 34
  br i1 %icmpeq9, label %tmp25, label %tmp26

tmp25:                                  ; preds = %tmp23
  %147 = getelementptr inbounds %array, %array addrspace(1)* %141, i64 0, i32 1, i64 %conv13
  %148 = load i32, i32 addrspace(1)* %147
  switch i32 %148, label %ifthen18 [
    i32 10, label %ifthen19
    i32 13, label %ifthen20
  ]

ifthen20:                                     ; preds = %tmp25
  %149 = add nsw i64 %conv13, 1
  %icmpsgt4 = icmp sgt i64 %136, %149
  br i1 %icmpsgt4, label %tmp27, label %tmp28

tmp27:                                  ; preds = %ifthen20
  %150 = getelementptr inbounds %array, %array addrspace(1)* %141, i64 0, i32 1, i64 %149
  %151 = load i32, i32 addrspace(1)* %150
  %icmpeq10 = icmp eq i32 %151, 10
  br i1 %icmpeq10, label %tmp29, label %tmp30

ifthen18:                             ; preds = %tmp25
  br label %tmp30

ifthen19:                             ; preds = %tmp25
  br label %tmp29

tmp29:                                      ; preds = %ifthen19, %tmp27
  %152 = bitcast %record* %1 to i8*
  %153 = bitcast %record* %2 to i8*
  %154 = bitcast %record* %3 to i8*
  %155 = bitcast %record* %4 to i8*
  %156 = bitcast %record* %5 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %152, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %153, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %154, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %155, i8 0, i64 16, i1 false)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %156, i8 0, i64 16, i1 false)
  %157 = load %array addrspace(1)*, %array addrspace(1)* addrspace(1)* %27
  %158 = getelementptr inbounds %array, %array addrspace(1)* %157, i64 0, i32 1, i64 %conv13
  %159 = load i32, i32 addrspace(1)* %158
  %160 = getelementptr inbounds i8, i8 addrspace(1)* %arg0, i64 8
  %161 = bitcast i8 addrspace(1)* %160 to i32 addrspace(1)*
  %162 = load i32, i32 addrspace(1)* %161
  %icmpeq11 = icmp eq i32 %159, %162
  br i1 %icmpeq11, label %tmp31, label %tmp32

; CHECK:  tmp31:
; CHECK:  %token159 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @g5, i32 2, i32 0, i8* %arg3, i32 40) [ "struct-live"(%record* %17) ]
; CHECK:  %token161 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, %record addrspace(1)*)* @g10, i32 3, i32 0, i8 addrspace(1)* %186, i8 addrspace(1)* null, %record addrspace(1)* %187) [ "gc-live"(i8 addrspace(1)* %186), "struct-live"(%record* %17) ]
; CHECK:  %token163 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %188)
;
tmp31:                                    ; preds = %ifelse22, %tmp29
  %163 = call i8 addrspace(1)* @g5(i8* %arg3, i32 40)
  %164 = addrspacecast %record* %17 to %record addrspace(1)*
  call void @g10(i8 addrspace(1)* %163, i8 addrspace(1)* null, %record addrspace(1)* %164)
  call void @g3(i8 addrspace(1)* %163)
  unreachable

tmp32:                                     ; preds = %tmp29, %ifelse22
  %165 = phi i32 [ %179, %ifelse22 ], [ %159, %tmp29 ]
  %166 = phi i64 [ %conv18, %ifelse22 ], [ %conv13, %tmp29 ]
  %icmpeq12 = icmp eq i32 %165, 34
  br i1 %icmpeq12, label %tmp33, label %tmp34

tmp33:                                    ; preds = %tmp32
  %167 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %166, i64 3)
  %conv15 = extractvalue { i64, i1 } %167, 1
  br i1 %conv15, label %ifthen21, label %ifelse21

ifelse21:                                  ; preds = %tmp33
  %conv28 = extractvalue { i64, i1 } %167, 0
  %168 = load i64, i64 addrspace(1)* %135
  %icmpslt1 = icmp slt i64 %168, %conv28
  br i1 %icmpslt1, label %tmp34, label %body4

; CHECK:  ifthen21:
; CHECK:  %token165 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %5) ]
; CHECK:  %token167 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %193) [ "struct-live"(%record* %5) ]
; CHECK:  %token169 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %194)
;
ifthen21:                                     ; preds = %tmp33
  call void @g1(i8* %arg3, i64 0, i64 0)
  %169 = addrspacecast %record* %5 to %record addrspace(1)*
  %170 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %169)
  call void @g3(i8 addrspace(1)* %170)
  unreachable

body4:                                  ; preds = %ifelse21
  %171 = add nsw i64 %166, 1
  %172 = getelementptr inbounds %array, %array addrspace(1)* %157, i64 0, i32 1, i64 %171
  %173 = load i32, i32 addrspace(1)* %172
  %icmpeq13 = icmp eq i32 %173, 34
  br i1 %icmpeq13, label %body5, label %tmp34

body5:                                     ; preds = %body4
  %174 = add nsw i64 %166, 2
  %175 = getelementptr inbounds %array, %array addrspace(1)* %157, i64 0, i32 1, i64 %174
  %176 = load i32, i32 addrspace(1)* %175
  %icmpeq14 = icmp eq i32 %176, 34
  br i1 %icmpeq14, label %body6, label %tmp34

tmp34:                                     ; preds = %body5, %body4, %ifelse21, %tmp32
  %177 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %166, i64 1)
  %conv16 = extractvalue { i64, i1 } %177, 1
  br i1 %conv16, label %ifthen22, label %ifelse22

; CHECK:  ifelse22:
; CHECK:  %token171 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @g0, i32 0, i32 0) [ "gc-live"(%array addrspace(1)* %.0196, i8 addrspace(1)* %.9), "struct-live"(%record* %17, %record* %1, %record* %5) ]
;
ifelse22:                                  ; preds = %tmp34
  %conv18 = extractvalue { i64, i1 } %177, 0
  %178 = getelementptr inbounds %array, %array addrspace(1)* %157, i64 0, i32 1, i64 %conv18
  %179 = load i32, i32 addrspace(1)* %178
  %icmpeq15 = icmp eq i32 %179, %162
  call void @g0()
  br i1 %icmpeq15, label %tmp31, label %tmp32

; CHECK:  ifthen22:
; CHECK:  %token174 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8*, i64, i64)* @g1, i32 3, i32 0, i8* %arg3, i64 0, i64 0) [ "struct-live"(%record* %1) ]
; CHECK:  %token176 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*, %record addrspace(1)*)* @g2, i32 2, i32 0, i8 addrspace(1)* null, %record addrspace(1)* %206) [ "struct-live"(%record* %1) ]
; CHECK:  %token178 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %207)
;
ifthen22:                                     ; preds = %tmp34
  call void @g1(i8* %arg3, i64 0, i64 0)
  %180 = addrspacecast %record* %1 to %record addrspace(1)*
  %181 = call i8 addrspace(1)* @g2(i8 addrspace(1)* null, %record addrspace(1)* %180)
  call void @g3(i8 addrspace(1)* %181)
  unreachable

body6:                                               ; preds = %body5
  br label %end1

tmp26:                             ; preds = %tmp23
  br label %tmp30

tmp22:                           ; preds = %ifelse17
  %182 = add nsw i64 %arg2, 1
  br label %tmp30

tmp24:                           ; preds = %tmp21
  br label %tmp30

tmp28:                           ; preds = %ifthen20
  br label %tmp30

tmp30:                                    ; preds = %tmp27, %tmp26, %tmp22, %tmp24, %tmp28, %ifthen18
  %183 = phi i64 [ %140, %tmp27 ], [ %140, %tmp26 ], [ %182, %tmp22 ], [ %140, %tmp24 ], [ %140, %tmp28 ], [ %140, %ifthen18 ]
  %184 = bitcast %record* %0 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %184, i8 0, i64 16, i1 false)
  %185 = getelementptr inbounds i8, i8 addrspace(1)* %arg0, i64 8
  %186 = bitcast i8 addrspace(1)* %185 to i32 addrspace(1)*
  %187 = load i64, i64 addrspace(1)* %135
  %icmpsgt5 = icmp sgt i64 %187, %183
  br i1 %icmpsgt5, label %tmp35, label %tmp36

tmp35:                              ; preds = %tmp30
  %188 = load %array addrspace(1)*, %array addrspace(1)* addrspace(1)* %27
  br label %body7

body7:                                    ; preds = %body8, %tmp35
  %189 = phi i64 [ %183, %tmp35 ], [ %193, %body8 ]
  %190 = getelementptr inbounds %array, %array addrspace(1)* %188, i64 0, i32 1, i64 %189
  %191 = load i32, i32 addrspace(1)* %190
  switch i32 %191, label %ifthen23 [
    i32 34, label %tmp36
    i32 10, label %tmp36
  ]

ifthen23:                                       ; preds = %body7
  %192 = load i32, i32 addrspace(1)* %186
  %icmpeq16 = icmp eq i32 %191, %192
  br i1 %icmpeq16, label %tmp36, label %body8

body8:                                  ; preds = %ifthen23
  %193 = add i64 %189, 1
  %icmpeq17 = icmp eq i64 %193, %187
  br i1 %icmpeq17, label %tmp36, label %body7

tmp36:                                         ; preds = %body7, %body7, %ifthen23, %body8, %tmp30
  %194 = phi i64 [ -1, %tmp30 ], [ %189, %ifthen23 ], [ %189, %body7 ], [ %189, %body7 ], [ -1, %body8 ]
  br label %end1

end1:                                       ; preds = %tmp36, %body6
  %195 = phi i64 [ %174, %body6 ], [ %194, %tmp36 ]
  %icmpeq18 = icmp eq i64 %195, -1
  br i1 %icmpeq18, label %tmp37, label %tmp38

; CHECK:  tmp37:
; CHECK:  %token180 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @g5, i32 2, i32 0, i8* %arg3, i32 40) [ "struct-live"(%record* %17) ]
; CHECK:  %token182 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*, i8 addrspace(1)*, %record addrspace(1)*)* @g10, i32 3, i32 0, i8 addrspace(1)* %222, i8 addrspace(1)* null, %record addrspace(1)* %223) [ "gc-live"(i8 addrspace(1)* %222), "struct-live"(%record* %17) ]
; CHECK:  %token184 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)*)* @g3, i32 1, i32 0, i8 addrspace(1)* %224)
;
tmp37:                                      ; preds = %end1
  %196 = call i8 addrspace(1)* @g5(i8* %arg3, i32 40)
  %197 = addrspacecast %record* %17 to %record addrspace(1)*
  call void @g10(i8 addrspace(1)* %196, i8 addrspace(1)* null, %record addrspace(1)* %197)
  call void @g3(i8 addrspace(1)* %196)
  unreachable
}

attributes #0 = { "hasRcdParam" }
