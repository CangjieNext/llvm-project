; RUN: opt -passes=cj-generic-intrinsic-opt --cangjie-pipeline -S < %s | FileCheck %s
;
; The `ti_load` load emitted by CJRuntimeLowering::replaceIsReference is
; normally only consumed by `icmp slt i8 %kind, 0`.  Earlier passes (GVN PRE,
; LoopRotate, SimplifyCFG, ...) may forward the loaded kind through a phi
; instead.  The pass used to report_fatal_error in that case; it must instead
; fold the load to the known constant kind.

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8**, i8*, i8* }

@"pkg:Number.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 -64, i16 1, i32 12, i8* null, i32 0, i8 1, i8 0, i16 -32766, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, i8** null, i8* null, i8* null } #0
@"pkg:Point.ti" = internal global %TypeInfo { i8* null, i8 22, i8 0, i16 0, i32 16, i8* null, i32 0, i8 8, i8 0, i16 0, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, i8** null, i8* null, i8* null } #0
@"std.core:Object.ti" = external global %TypeInfo #0

; CHECK-LABEL: define i1 @load_used_by_phi(
; CHECK-NOT: load i8
; CHECK: br i1 true, label %loop, label %exit
; CHECK: loop:
; CHECK-NEXT: %k = phi i8 [ -128, %entry ], [ -128, %latch ]
; CHECK-NEXT: %iv = phi i64
; CHECK-NEXT: %c = icmp slt i8 %k, 0
; CHECK-NOT: load i8
define i1 @load_used_by_phi(i64 %n) gc "cangjie" {
entry:
  %k0 = load i8, i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"pkg:Number.ti", i64 0, i32 1), align 8, !ti_load !0
  %c0 = icmp slt i8 %k0, 0
  br i1 %c0, label %loop, label %exit

loop:
  %k = phi i8 [ %k0, %entry ], [ %k1, %latch ]
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %c = icmp slt i8 %k, 0
  br i1 %c, label %ref, label %nonref

ref:
  br label %latch

nonref:
  br label %latch

latch:
  %k1 = load i8, i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"pkg:Number.ti", i64 0, i32 1), align 8, !ti_load !0
  %iv.next = add i64 %iv, 1
  %cond = icmp eq i64 %iv.next, %n
  br i1 %cond, label %exit, label %loop

exit:
  %r = phi i1 [ false, %entry ], [ true, %latch ]
  ret i1 %r
}

; CHECK-LABEL: define i1 @value_type_used_by_select(
; CHECK-NOT: load i8
; CHECK: %s = select i1 %p, i8 22, i8 22
; CHECK-NEXT: %c = icmp slt i8 %s, 0
; CHECK-NEXT: ret i1 %c
define i1 @value_type_used_by_select(i1 %p) gc "cangjie" {
  %k = load i8, i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"pkg:Point.ti", i64 0, i32 1), align 8, !ti_load !0
  %s = select i1 %p, i8 %k, i8 %k
  %c = icmp slt i8 %s, 0
  ret i1 %c
}

; CHECK-LABEL: define i1 @plain_icmp(
; CHECK-NOT: load i8
; CHECK-NOT: icmp
; CHECK: ret i1 true
define i1 @plain_icmp() gc "cangjie" {
  %k = load i8, i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"pkg:Number.ti", i64 0, i32 1), align 8, !ti_load !0
  %c = icmp slt i8 %k, 0
  ret i1 %c
}

; Declared-only TypeInfo: only the sign of the kind is known, so the canonical
; compare is folded but the load itself must be kept for the other user.
; CHECK-LABEL: define i8 @external_ti_keeps_load(
; CHECK: %k = load i8
; CHECK-NOT: icmp
; CHECK: %s = select i1 true, i8 %k, i8 0
; CHECK: ret i8 %s
define i8 @external_ti_keeps_load() gc "cangjie" {
  %k = load i8, i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"std.core:Object.ti", i64 0, i32 1), align 8, !ti_load !0
  %c = icmp slt i8 %k, 0
  %s = select i1 %c, i8 %k, i8 0
  ret i8 %s
}

attributes #0 = { "CFileKlass" }

!0 = !{}
