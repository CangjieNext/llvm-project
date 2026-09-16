;RUN: opt '-passes=cj-pea' -S < %s | FileCheck %s

; Regression test for issue #204: PEA finalizer stack allocation did not
; model the unwind semantics of a plain (non-nounwind) call. That edge is
; invisible in the CFG, so the promoted ~init was skipped when the call
; unwound, permanently leaking the GC-detached stack object's resource.
; The fix converts such calls to invokes unwinding to a catch-all pad that
; runs ~init and rethrows.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%BitMap = type { i32, [0 x i8] }
%ExtensionDef = type { i32, i8, i8, i16, i8*, i8*, i8*, i8* }
%"ObjLayout.default:W" = type { i64, i1 }

@"default:W.ti" = global %TypeInfo { i8* getelementptr inbounds ([10 x i8], [10 x i8]* @"default:W.name", i32 0, i32 0), i8 -128, i8 0, i16 2, i32 16, %BitMap* null, i32 0, i8 8, i8 0, i16 -32766, i32* getelementptr inbounds ([2 x i32], [2 x i32]* @"default:W.ti.offsets", i32 0, i32 0), i8* bitcast (void (i8 addrspace(1)*, %TypeInfo*)* @"_CN7default1W5~initHv" to i8*), i8* null, i8* bitcast ([2 x %TypeInfo*]* @"default:W.ti.fields" to i8*), %TypeInfo* @"std.core:Object.ti", %ExtensionDef** null, i8* inttoptr (i64 -9223372036854775808 to i8*), i8* null }, !RelatedType !0
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

; The may-throw call after the allocation becomes an invoke unwinding to a
; catch-all pad that runs ~init and rethrows.
define i64 @work_maythrow(i64 %i) gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
; CHECK-LABEL: @work_maythrow(
; CHECK:         invoke i64 @may_throw(
; CHECK-NEXT:    to label %{{.*}} unwind label %cj.finalizer.unwind
; Normal path: synchronous ~init before ret.
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
entry:
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

; A nounwind call cannot unwind: promotion happens without any EH pad.
define i64 @work_nounwind(i64 %i) gc "cangjie" personality i32 (...)* @"__cj_personality_v0$" {
; CHECK-LABEL: @work_nounwind(
; CHECK-NOT:     invoke
; CHECK-NOT:     cj.finalizer.unwind
; CHECK:         call void @"_CN7default1W5~initHv"(
; CHECK-NEXT:    ret i64
entry:
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

declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32) #0
declare i64 @may_throw(i64, i64)
declare i64 @no_throw(i64, i64) nounwind

attributes #0 = { "cj-runtime" }

!0 = !{!"ObjLayout.default:W"}
