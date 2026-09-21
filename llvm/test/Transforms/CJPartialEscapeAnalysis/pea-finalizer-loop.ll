; RUN: opt < %s '-passes=cj-pea' -S | FileCheck %s

; Stack-allocate a loop-local NewFinalizer object when ~init is synchronous
; at the unique latch. Mirrors the ObjC wrapper pattern:
;   for (...) { h = opaque_get(); s += opaque_call(h); opaque_release(h); }
; without inlining ~init, and without treating a post-loop invoke as a ban.

%BitMap = type { i32, [0 x i8] }
%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i32*, i8*, i8*, i8*, %TypeInfo*, i8*, i8* }
%ObjLayout.W = type { i8* }

@g = global i8 addrspace(1)* null

@"std.core$Object.ti" = external global %TypeInfo
@"default$W.name" = internal global [12 x i8] c"default$W\00\00\00", align 1
@"default$W.ti.offsets" = internal global [1 x i32] [i32 0]
@"default$W.ti.fields" = internal global [1 x %TypeInfo*] [%TypeInfo* @"std.core$Object.ti"]
@"default$W.fieldNames" = internal global [1 x i8*] [i8* getelementptr inbounds ([3 x i8], [3 x i8]* @"default$W.field.1.name", i32 0, i32 0)]
@"default$W.field.1.name" = internal global [3 x i8] c"h\00\00"

@W.ti = weak_odr global %TypeInfo {
  i8* getelementptr inbounds ([12 x i8], [12 x i8]* @"default$W.name", i32 0, i32 0),
  i8 -128, i8 0, i16 2, i32 16, %BitMap* null, i32 0, i8 8, i8 0,
  i32* getelementptr inbounds ([1 x i32], [1 x i32]* @"default$W.ti.offsets", i32 0, i32 0),
  i8* null,
  i8* bitcast (void (i8 addrspace(1)*, %TypeInfo*)* @w_dtor to i8*),
  i8* bitcast ([1 x %TypeInfo*]* @"default$W.ti.fields" to i8*),
  %TypeInfo* @"std.core$Object.ti", i8* null,
  i8* bitcast ([1 x i8*]* @"default$W.fieldNames" to i8*)
}, !RelatedType !0

@Throw.ti = weak_odr global %TypeInfo {
  i8* getelementptr inbounds ([12 x i8], [12 x i8]* @"default$W.name", i32 0, i32 0),
  i8 -128, i8 0, i16 2, i32 16, %BitMap* null, i32 0, i8 8, i8 0,
  i32* getelementptr inbounds ([1 x i32], [1 x i32]* @"default$W.ti.offsets", i32 0, i32 0),
  i8* null,
  i8* bitcast (void (i8 addrspace(1)*, %TypeInfo*)* @throwing_dtor to i8*),
  i8* bitcast ([1 x %TypeInfo*]* @"default$W.ti.fields" to i8*),
  %TypeInfo* @"std.core$Object.ti", i8* null,
  i8* bitcast ([1 x i8*]* @"default$W.fieldNames" to i8*)
}, !RelatedType !0

declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32)
declare i8* @opaque_get()
declare i64 @opaque_call(i8*, i64) nounwind
declare i64 @may_throw_call(i8*, i64)
declare void @opaque_release(i8*)
declare void @CJ_MCC_ThrowException(i8 addrspace(1)*)
declare void @may_throw()
declare i32 @__cj_personality_v0(...)
declare void @llvm.memset.p1i8.i64(i8 addrspace(1)* nocapture writeonly, i8, i64, i1 immarg)
; A defined, non-capturing callee: PEA proves no escape interprocedurally.
define i64 @use_obj(i8 addrspace(1)* nocapture %p) nounwind {
  ret i64 0
}
declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

; User-written ~init: calls a C release and has no nounwind attribute.
define void @w_dtor(i8 addrspace(1)* %this, %TypeInfo* %ti) gc "cangjie" {
  %p = getelementptr inbounds i8, i8 addrspace(1)* %this, i64 8
  %hslot = bitcast i8 addrspace(1)* %p to i8* addrspace(1)*
  %h = load i8*, i8* addrspace(1)* %hslot, align 8
  call void @opaque_release(i8* %h)
  ret void
}

define void @throwing_dtor(i8 addrspace(1)* %this, %TypeInfo* %ti) gc "cangjie" {
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %this)
  unreachable
}


; Allocation is in the loop body, not the latch — the Cangjie `for` shape.
; CHECK-LABEL: define i64 @loop_body_alloc(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: CJ_MCC_NewFinalizer
; CHECK: call void @w_dtor(
; CHECK-NEXT: br label %header
define i64 @loop_body_alloc() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %sum = phi i64 [ 0, %entry ], [ %add, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %handle = call i8* @opaque_get()
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  call void @llvm.memset.p1i8.i64(i8 addrspace(1)* %obj, i8 0, i64 16, i1 false)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %hslot = bitcast i8 addrspace(1)* %p to i8* addrspace(1)*
  store i8* %handle, i8* addrspace(1)* %hslot, align 8
  %h = load i8*, i8* addrspace(1)* %hslot, align 8
  %v = call i64 @opaque_call(i8* %h, i64 0)
  br label %latch

latch:
  %add = add i64 %sum, %v
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %sum
}

; Same as loop_body_alloc, plus a post-loop invoke/resume. The invoke must not
; poison the loop-local allocation.
; CHECK-LABEL: define i64 @loop_then_invoke(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: CJ_MCC_NewFinalizer
; CHECK: call void @w_dtor(
define i64 @loop_then_invoke() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %sum = phi i64 [ 0, %entry ], [ %add, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %handle = call i8* @opaque_get()
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %hslot = bitcast i8 addrspace(1)* %p to i8* addrspace(1)*
  store i8* %handle, i8* addrspace(1)* %hslot, align 8
  %h = load i8*, i8* addrspace(1)* %hslot, align 8
  %v = call i64 @opaque_call(i8* %h, i64 0)
  br label %latch

latch:
  %add = add i64 %sum, %v
  %next = add i64 %i, 1
  br label %header

exit:
  invoke void @may_throw() to label %ret unwind label %lpad

ret:
  ret i64 %sum

lpad:
  %lp = landingpad { i8*, i32 } cleanup
  resume { i8*, i32 } %lp
}

; Canonical single-latch allocation (CreateBB == Latch).
; CHECK-LABEL: define i64 @loop_latch_alloc(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: CJ_MCC_NewFinalizer
; CHECK: call void @w_dtor(
define i64 @loop_latch_alloc() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %sum = phi i64 [ 0, %entry ], [ %add, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %latch, label %exit

latch:
  %handle = call i8* @opaque_get()
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %hslot = bitcast i8 addrspace(1)* %p to i8* addrspace(1)*
  store i8* %handle, i8* addrspace(1)* %hslot, align 8
  %h = load i8*, i8* addrspace(1)* %hslot, align 8
  %v = call i64 @opaque_call(i8* %h, i64 0)
  %add = add i64 %sum, %v
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %sum
}

; PHI of the object across iterations must stay on the heap (#549 UAF class).
; CHECK-LABEL: define i64 @loop_phi(
; CHECK: CJ_MCC_NewFinalizer
; CHECK-NOT: call void @w_dtor(
define i64 @loop_phi() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %saved = phi i8 addrspace(1)* [ null, %entry ], [ %obj, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i64 addrspace(1)*
  store i64 1, i64 addrspace(1)* %slot, align 8
  br label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; Publishing the object pointer is not a stack-allocatable lifetime.
; CHECK-LABEL: define i64 @loop_store_escape(
; CHECK: CJ_MCC_NewFinalizer
; CHECK-NOT: call void @w_dtor(
define i64 @loop_store_escape() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  store i8 addrspace(1)* %obj, i8 addrspace(1)** @g
  br label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; Unreachable in the body skips the latch, so ~init would be missed.
; CHECK-LABEL: define i64 @loop_body_unreachable(
; CHECK: CJ_MCC_NewFinalizer
; CHECK-NOT: call void @w_dtor(
define i64 @loop_body_unreachable() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i64 addrspace(1)*
  store i64 1, i64 addrspace(1)* %slot, align 8
  %ov = icmp eq i64 %i, 3
  br i1 %ov, label %trap, label %latch

trap:
  unreachable

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; A ~init that may throw is still promoted: an uncaught exception escaping a
; finalizer is undefined behavior per the language spec, so the synchronous
; call at the latch is a legal (and deterministic) realization of that UB.
; CHECK-LABEL: define i64 @loop_throwing_dtor(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: CJ_MCC_NewFinalizer
; CHECK: call void @throwing_dtor(
; CHECK-NEXT: br label %header
define i64 @loop_throwing_dtor() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @Throw.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i64 addrspace(1)*
  store i64 1, i64 addrspace(1)* %slot, align 8
  br label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; A conditional break from the body makes the body a second exiting block:
; the break path skips the latch, so no destructor point can be proven.
; CHECK-LABEL: define i64 @loop_body_break(
; CHECK: CJ_MCC_NewFinalizer
; CHECK-NOT: call void @w_dtor(
define i64 @loop_body_break() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i64 addrspace(1)*
  store i64 1, i64 addrspace(1)* %slot, align 8
  %c = icmp eq i64 %i, 3
  br i1 %c, label %exit, label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; An in-loop invoke whose unwind edge bypasses the latch leaves a path from
; the allocation (and from the store use) that skips the destructor: reject.
; CHECK-LABEL: define i64 @loop_invoke_unwind_skip(
; CHECK: CJ_MCC_NewFinalizer
; CHECK-NOT: call void @w_dtor(
define i64 @loop_invoke_unwind_skip() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i64 addrspace(1)*
  store i64 1, i64 addrspace(1)* %slot, align 8
  invoke void @may_throw() to label %latch unwind label %lpad

latch:
  %next = add i64 %i, 1
  br label %header

lpad:
  %lp = landingpad { i8*, i32 } cleanup
  resume { i8*, i32 } %lp

exit:
  ret i64 %i
}

; A use inside a nested inner loop is still local to the outer loop: every
; path from the allocation and from the load reaches the outer latch, so
; promotion is legal and ~init runs once per outer iteration.
; CHECK-LABEL: define i64 @loop_nested_inner(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: CJ_MCC_NewFinalizer
; CHECK: call void @w_dtor(
; CHECK-NEXT: br label %header
define i64 @loop_nested_inner() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %sum = phi i64 [ 0, %entry ], [ %add, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i64 addrspace(1)*
  store i64 1, i64 addrspace(1)* %slot, align 8
  br label %iheader

iheader:
  %j = phi i64 [ 0, %body ], [ %jnext, %ilatch ]
  %vsum = phi i64 [ 0, %body ], [ %vadd, %ilatch ]
  %jcmp = icmp slt i64 %j, 4
  br i1 %jcmp, label %ibody, label %latch

ibody:
  %v = load i64, i64 addrspace(1)* %slot, align 8
  %vadd = add i64 %vsum, %v
  br label %ilatch

ilatch:
  %jnext = add i64 %j, 1
  br label %iheader

latch:
  %add = add i64 %sum, %vsum
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %sum
}

; A may-throw plain call in the loop body unwinds out of the function on an
; edge the CFG does not model, so the latch ~init would be skipped
; (issue #204). Converting such calls to invokes unwinding to a pad needs a
; personality fn, which this function lacks, so the gate (escape-analysis
; pre-flight) rejects the promotion. Real cjc output always carries
; __cj_personality_v0$; see loop_maythrow_call_personality.
; CHECK-LABEL: define i64 @loop_maythrow_call(
; CHECK: CJ_MCC_NewFinalizer
; CHECK-NOT: call void @w_dtor(
define i64 @loop_maythrow_call() gc "cangjie" {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %sum = phi i64 [ 0, %entry ], [ %add, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %latch, label %exit

latch:
  %handle = call i8* @opaque_get()
  ; 16 = 8-byte object head + 8-byte payload (ObjLayout {i64}).
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %hslot = bitcast i8 addrspace(1)* %p to i8* addrspace(1)*
  store i8* %handle, i8* addrspace(1)* %hslot, align 8
  %h = load i8*, i8* addrspace(1)* %hslot, align 8
  %v = call i64 @may_throw_call(i8* %h, i64 0)
  %add = add i64 %sum, %v
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %sum
}

; A GC reference field read via cj.gcread.ref yields an alias the walk
; cannot track (a self-referential field yields the object itself), and its
; uses are not constrained to precede ~init: reject, stay on the heap.
; CHECK-LABEL: define i64 @loop_gcread_ref(
; CHECK: CJ_MCC_NewFinalizer
; CHECK-NOT: call void @w_dtor(
define i64 @loop_gcread_ref() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i8 addrspace(1)* addrspace(1)*
  %f = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %slot)
  %v = call i64 @use_obj(i8 addrspace(1)* %f)
  br label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; A GC field write via cj.gcwrite.ref whose base is the object is a plain
; use that keeps the object alive until the write: promote.
; CHECK-LABEL: define i64 @loop_gcwrite_ref(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: CJ_MCC_NewFinalizer
; CHECK: call void @w_dtor(
define i64 @loop_gcwrite_ref() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %slot = bitcast i8 addrspace(1)* %p to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %obj, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %slot)
  br label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; An ordinary non-capturing call on the object is a plain use: promote when
; the latch destructor point post-dominates the call.
; CHECK-LABEL: define i64 @loop_call_use_obj(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: CJ_MCC_NewFinalizer
; CHECK: call void @w_dtor(
define i64 @loop_call_use_obj() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %body, label %exit

body:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  call void @llvm.memset.p1i8.i64(i8 addrspace(1)* %obj, i8 0, i64 16, i1 false)
  %v = call i64 @use_obj(i8 addrspace(1)* %obj)
  br label %latch

latch:
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %i
}

; Same as loop_maythrow_call, but with a personality fn: the in-loop
; may-throw call is converted to an invoke unwinding to a pad that runs the
; current iteration's ~init and rethrows, and the loop promotion proceeds.
; CHECK-LABEL: define i64 @loop_maythrow_call_personality(
; CHECK: alloca { %TypeInfo*, %ObjLayout.W }
; CHECK-NOT: call {{.*}}@CJ_MCC_NewFinalizer
; CHECK: invoke i64 @may_throw_call(
; CHECK: call void @w_dtor(
; CHECK: cj.finalizer.unwind:
; CHECK: call void @w_dtor(
; CHECK: call void @CJ_MCC_ThrowException(
define i64 @loop_maythrow_call_personality() gc "cangjie" personality i32 (...)* @__cj_personality_v0 {
entry:
  br label %header

header:
  %i = phi i64 [ 0, %entry ], [ %next, %latch ]
  %sum = phi i64 [ 0, %entry ], [ %add, %latch ]
  %cmp = icmp slt i64 %i, 10
  br i1 %cmp, label %latch, label %exit

latch:
  %handle = call i8* @opaque_get()
  ; 16 = 8-byte object head + 8-byte payload (ObjLayout {i64}).
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%TypeInfo* @W.ti to i8*), i32 16)
  %p = getelementptr inbounds i8, i8 addrspace(1)* %obj, i64 8
  %hslot = bitcast i8 addrspace(1)* %p to i8* addrspace(1)*
  store i8* %handle, i8* addrspace(1)* %hslot, align 8
  %h = load i8*, i8* addrspace(1)* %hslot, align 8
  %v = call i64 @may_throw_call(i8* %h, i64 0)
  %add = add i64 %sum, %v
  %next = add i64 %i, 1
  br label %header

exit:
  ret i64 %sum
}

!0 = !{!"ObjLayout.W"}
