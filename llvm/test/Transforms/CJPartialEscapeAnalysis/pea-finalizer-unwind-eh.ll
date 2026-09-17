; RUN: opt < %s '-passes=cj-pea' -S | FileCheck %s

; Issue #204: a may-throw plain call while a promoted finalizer is live
; unwinds on an edge the CFG does not model, skipping ~init. Convert those
; calls to invokes that land in a catch-all pad running ~init and rethrowing.
; ~init is not required to be nounwind: a throwing finalizer is UB per spec.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%BitMap = type { i32, [0 x i8] }
%ExtensionDef = type { i32, i8, i8, i16, i8*, i8*, i8*, i8* }
%"ObjLayout.default:W" = type { i64, i1 }

; Field 11 (CIT_GENERIC_FROM) holds the class finalizer pointer.
@"default:W.ti" = global %TypeInfo { i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"default:W.name", i32 0, i32 0), i8 -128, i8 0, i16 2, i32 16, %BitMap* null, i32 0, i8 8, i8 0, i16 -32766, i32* getelementptr inbounds ([2 x i32], [2 x i32]* @"default:W.ti.offsets", i32 0, i32 0), i8* bitcast (void (i8 addrspace(1)*, %TypeInfo*)* @"_CN7default1W5~initHv" to i8*), i8* null, i8* bitcast ([2 x %TypeInfo*]* @"default:W.ti.fields" to i8*), %TypeInfo* @"std.core:Object.ti", %ExtensionDef** null, i8* inttoptr (i64 -9223372036854775808 to i8*), i8* null }, !RelatedType !0
@"default:Throw.ti" = global %TypeInfo { i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"default:W.name", i32 0, i32 0), i8 -128, i8 0, i16 2, i32 16, %BitMap* null, i32 0, i8 8, i8 0, i16 -32766, i32* getelementptr inbounds ([2 x i32], [2 x i32]* @"default:W.ti.offsets", i32 0, i32 0), i8* bitcast (void (i8 addrspace(1)*, %TypeInfo*)* @throwing_dtor to i8*), i8* null, i8* bitcast ([2 x %TypeInfo*]* @"default:W.ti.fields" to i8*), %TypeInfo* @"std.core:Object.ti", %ExtensionDef** null, i8* inttoptr (i64 -9223372036854775808 to i8*), i8* null }, !RelatedType !0
@"default:W.name" = internal global [10 x i8] c"default:W\00", align 1
@"default:W.ti.offsets" = internal global [2 x i32] [i32 0, i32 0]
@"default:W.ti.fields" = internal global [2 x %TypeInfo*] [%TypeInfo* @"std.core:Object.ti", %TypeInfo* @"std.core:Object.ti"]
@"std.core:Object.ti" = external global %TypeInfo

define private i32 @"__cj_personality_v0$"(...) {
entry:
  ret i32 0
}

define void @"_CN7default1W5~initHv"(i8 addrspace(1)* %this, %TypeInfo* %outerTI) nounwind gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
  ret void
}

define void @throwing_dtor(i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %this)
  unreachable
}

declare void @CJ_MCC_ThrowException(i8 addrspace(1)*)

; CHECK-LABEL: @work_maythrow(
; CHECK:         invoke i64 @may_throw(
; CHECK-NEXT:    to label %{{.*}} unwind label %cj.finalizer.unwind
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK:         ret i64
; CHECK:       cj.finalizer.unwind:
; CHECK-NEXT:    %finalizer.lp = landingpad token
; CHECK-NEXT:    catch i8* null
; CHECK:         call i8* @CJ_MCC_GetExceptionWrapper()
; CHECK:         call i8 addrspace(1)* @CJ_MCC_PostThrowException(
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK:         call void @CJ_MCC_ThrowException(
; CHECK-NEXT:    unreachable
define i64 @work_maythrow(i64 %i) gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
entry:
  ; 24 = 8-byte object head + 16-byte payload (ObjLayout {i64, i1}).
  %w = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:W.ti" to i8*), i32 24)
  %0 = bitcast i8 addrspace(1)* %w to i8* addrspace(1)*
  %ti.payload = getelementptr i8*, i8* addrspace(1)* %0, i32 1
  %1 = bitcast i8* addrspace(1)* %ti.payload to %"ObjLayout.default:W" addrspace(1)*
  %2 = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %1, i32 0, i32 0
  store i64 %i, i64 addrspace(1)* %2, align 8
  %3 = load i64, i64 addrspace(1)* %2, align 8
  %r = call i64 @may_throw(i64 %3, i64 %i)
  ret i64 %r
}

; CHECK-LABEL: @work_nounwind(
; CHECK-NOT:     invoke
; CHECK-NOT:     cj.finalizer.unwind
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK-NEXT:    ret i64
define i64 @work_nounwind(i64 %i) gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
entry:
  ; 24 = 8-byte object head + 16-byte payload (ObjLayout {i64, i1}).
  %w = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:W.ti" to i8*), i32 24)
  %0 = bitcast i8 addrspace(1)* %w to i8* addrspace(1)*
  %ti.payload = getelementptr i8*, i8* addrspace(1)* %0, i32 1
  %1 = bitcast i8* addrspace(1)* %ti.payload to %"ObjLayout.default:W" addrspace(1)*
  %2 = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %1, i32 0, i32 0
  store i64 %i, i64 addrspace(1)* %2, align 8
  %3 = load i64, i64 addrspace(1)* %2, align 8
  %r = call i64 @no_throw(i64 %3, i64 %i)
  ret i64 %r
}

; A ~init that may throw is still promoted: throwing from a finalizer is UB.
; CHECK-LABEL: @work_throwing_dtor(
; CHECK:         alloca { %TypeInfo*, %"ObjLayout.default:W" }
; CHECK-NOT:     CJ_MCC_NewFinalizer
; CHECK:         call void @throwing_dtor(
; CHECK-NEXT:    ret i64
define i64 @work_throwing_dtor(i64 %i) gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
entry:
  ; 24 = 8-byte object head + 16-byte payload (ObjLayout {i64, i1}).
  %w = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:Throw.ti" to i8*), i32 24)
  %0 = bitcast i8 addrspace(1)* %w to i8* addrspace(1)*
  %ti.payload = getelementptr i8*, i8* addrspace(1)* %0, i32 1
  %1 = bitcast i8* addrspace(1)* %ti.payload to %"ObjLayout.default:W" addrspace(1)*
  %2 = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %1, i32 0, i32 0
  store i64 %i, i64 addrspace(1)* %2, align 8
  ret i64 %i
}

declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32) #0
declare i64 @may_throw(i64, i64)
declare i64 @no_throw(i64, i64) nounwind

attributes #0 = { "cj-runtime" }

!0 = !{!"ObjLayout.default:W"}

@"default:U.ti" = external global %TypeInfo
@"default:V.ti" = global %TypeInfo { i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"default:V.name", i32 0, i32 0), i8 -128, i8 0, i16 2, i32 16, %BitMap* null, i32 0, i8 8, i8 0, i16 -32766, i32* getelementptr inbounds ([2 x i32], [2 x i32]* @"default:W.ti.offsets", i32 0, i32 0), i8* null, i8* null, i8* bitcast ([2 x %TypeInfo*]* @"default:W.ti.fields" to i8*), %TypeInfo* @"std.core:Object.ti", %ExtensionDef** null, i8* inttoptr (i64 -9223372036854775808 to i8*), i8* null }, !RelatedType !0
@"default:V.name" = internal global [10 x i8] c"default:V\00", align 1

declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32) #0

; CHECK-LABEL: @two_finalizers(
; CHECK-NOT:     call {{.*}}@CJ_MCC_NewFinalizer(
; CHECK:         invoke {{.*}}@CJ_MCC_NewObject(
; CHECK:         invoke i64 @may_throw(
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK:         ret i64
; CHECK:       cj.finalizer.unwind:
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK-NOT:     call void @"_CN7default1W5~initHv"(
; CHECK:         call void @CJ_MCC_ThrowException(
; CHECK:       cj.finalizer.unwind{{[0-9]*}}:
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK:         call void @CJ_MCC_ThrowException(
define i64 @two_finalizers(i64 %i) gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
entry:
  ; 24 = 8-byte object head + 16-byte payload (ObjLayout {i64, i1}).
  %a = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:W.ti" to i8*), i32 24)
  %a0 = bitcast i8 addrspace(1)* %a to i8* addrspace(1)*
  %a.ti = getelementptr i8*, i8* addrspace(1)* %a0, i32 1
  %a.layout = bitcast i8* addrspace(1)* %a.ti to %"ObjLayout.default:W" addrspace(1)*
  %a.f = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %a.layout, i32 0, i32 0
  store i64 %i, i64 addrspace(1)* %a.f, align 8
  %o = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @"default:U.ti" to i8*), i32 16)
  %b = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:W.ti" to i8*), i32 24)
  %b0 = bitcast i8 addrspace(1)* %b to i8* addrspace(1)*
  %b.ti = getelementptr i8*, i8* addrspace(1)* %b0, i32 1
  %b.layout = bitcast i8* addrspace(1)* %b.ti to %"ObjLayout.default:W" addrspace(1)*
  %b.f = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %b.layout, i32 0, i32 0
  store i64 %i, i64 addrspace(1)* %b.f, align 8
  %av = load i64, i64 addrspace(1)* %a.f, align 8
  %r = call i64 @may_throw(i64 %av, i64 %i)
  %bv = load i64, i64 addrspace(1)* %b.f, align 8
  %s = add i64 %r, %bv
  ret i64 %s
}

; CHECK-LABEL: @no_personality_reject(
; CHECK-NOT:     invoke
; CHECK-NOT:     cj.finalizer.unwind
; CHECK:         @CJ_MCC_NewFinalizer(
; CHECK-NOT:     call void @"_CN7default1W5~initHv"(
define i64 @no_personality_reject(i64 %i) gc "cangjie" {
entry:
  ; 24 = 8-byte object head + 16-byte payload (ObjLayout {i64, i1}).
  %w = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:W.ti" to i8*), i32 24)
  %0 = bitcast i8 addrspace(1)* %w to i8* addrspace(1)*
  %ti.payload = getelementptr i8*, i8* addrspace(1)* %0, i32 1
  %1 = bitcast i8* addrspace(1)* %ti.payload to %"ObjLayout.default:W" addrspace(1)*
  %2 = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %1, i32 0, i32 0
  store i64 %i, i64 addrspace(1)* %2, align 8
  %3 = load i64, i64 addrspace(1)* %2, align 8
  %r = call i64 @may_throw(i64 %3, i64 %i)
  ret i64 %r
}

; CHECK-LABEL: @musttail_hazard(
; CHECK:         @CJ_MCC_NewFinalizer(
; CHECK-NOT:     invoke
; CHECK-NOT:     call void @"_CN7default1W5~initHv"(
; CHECK:         musttail call i64 @may_throw1(
define i64 @musttail_hazard(i64 %i) gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
entry:
  ; 24 = 8-byte object head + 16-byte payload (ObjLayout {i64, i1}).
  %w = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:W.ti" to i8*), i32 24)
  %0 = bitcast i8 addrspace(1)* %w to i8* addrspace(1)*
  %ti.payload = getelementptr i8*, i8* addrspace(1)* %0, i32 1
  %1 = bitcast i8* addrspace(1)* %ti.payload to %"ObjLayout.default:W" addrspace(1)*
  %2 = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %1, i32 0, i32 0
  store i64 %i, i64 addrspace(1)* %2, align 8
  %3 = load i64, i64 addrspace(1)* %2, align 8
  %r = musttail call i64 @may_throw1(i64 %3)
  ret i64 %r
}

declare i64 @may_throw1(i64)

; Gate rejects W (no personality), so the dependent V stays on the heap too.
; CHECK-LABEL: @dependent_escape(
; CHECK:         @CJ_MCC_NewFinalizer(
; CHECK:         @CJ_MCC_NewObject(
; CHECK-NOT:     alloca
define i64 @dependent_escape(i64 %i) gc "cangjie" {
entry:
  ; 24 = 8-byte object head + 16-byte payload (ObjLayout {i64, i1}).
  %w = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @"default:W.ti" to i8*), i32 24)
  ; 16 = 8-byte object head + 8-byte payload (ObjLayout {i64}).
  %d = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @"default:V.ti" to i8*), i32 16)
  %w0 = bitcast i8 addrspace(1)* %w to i8* addrspace(1)*
  %w.ti = getelementptr i8*, i8* addrspace(1)* %w0, i32 1
  %w.layout = bitcast i8* addrspace(1)* %w.ti to %"ObjLayout.default:W" addrspace(1)*
  %w.f = getelementptr inbounds %"ObjLayout.default:W", %"ObjLayout.default:W" addrspace(1)* %w.layout, i32 0, i32 0
  %w.fref = bitcast i64 addrspace(1)* %w.f to i8 addrspace(1)* addrspace(1)*
  store i8 addrspace(1)* %d, i8 addrspace(1)* addrspace(1)* %w.fref, align 8
  %r = call i64 @may_throw(i64 %i, i64 %i)
  ret i64 %r
}
