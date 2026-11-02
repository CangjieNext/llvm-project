; RUN: opt < %s -passes=cj-insert-remove-local-finalizer -S | FileCheck %s

; Every CJ_MCC_AddLocalFinalizer must be paired with a
; CJ_MCC_RemoveLocalFinalizer at the end of the object's live range -- on every
; path, exactly once. These tests pin down the placement for ordinary control
; flow; the exception-handling shapes live in exceptions.ll.

%Cls = type { i8*, i64 }

declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32)
declare void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)*)
declare void @use(i8 addrspace(1)*)
declare void @use_cls(%Cls addrspace(1)*)
declare i1 @cond()


; A heap finalizer is never registered as a local one, so it must be left alone.

define void @heap_finalizer_untouched(i8* %ti) {
; CHECK-LABEL: @heap_finalizer_untouched(
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:         ret void
;
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @use(i8 addrspace(1)* %obj)
  ret void
}


; The registration itself is the only use: the object is dead as soon as it is
; registered, so the Remove follows immediately.

define void @dead_on_arrival(i8* %ti) {
; CHECK-LABEL: @dead_on_arrival(
; CHECK:         call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
;
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  ret void
}


; The object is only used on one arm of the branch. The other arm never touches
; it, but it is just as dead there, so it needs its own Remove.

define void @diamond(i8* %ti, i1 %c) {
; CHECK-LABEL: @diamond(
; CHECK:         call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:       then:
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK:       else:
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  br i1 %c, label %then, label %else
then:
  call void @use(i8 addrspace(1)* %obj)
  br label %exit
else:
  br label %exit
exit:
  ret void
}


; The object flows into phi nodes. It stays alive until the last use of the
; phis, and %merge is also reached from a path where the phis are *not* the
; object -- inserting on that incoming edge as well would remove it twice.

define void @phi_merge(i8* %ti, i1 %c, i8 addrspace(1)* %other) {
; CHECK-LABEL: @phi_merge(
; CHECK:       then:
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:       else:
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:       merge:
; CHECK:         call void @use(i8 addrspace(1)* %p)
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %q)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
;
entry:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  br i1 %c, label %then, label %else
then:
  br label %merge
else:
  br label %merge
merge:
  %p = phi i8 addrspace(1)* [ %obj, %then ], [ %other, %else ]
  %q = phi i8 addrspace(1)* [ %obj, %then ], [ %other, %else ]
  call void @use(i8 addrspace(1)* %p)
  call void @use(i8 addrspace(1)* %q)
  ret void
}


; Same, but the last phi of the block is not the one carrying the object. The
; call must not land between the phi nodes -- that is invalid IR.

define i32 @phi_not_last(i8* %ti, i1 %c, i8 addrspace(1)* %other, i32 %x) {
; CHECK-LABEL: @phi_not_last(
; CHECK:       merge:
; CHECK-NEXT:    %p = phi i8 addrspace(1)*
; CHECK-NEXT:    %n = phi i32
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %p)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret i32 %n
;
entry:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  br i1 %c, label %then, label %else
then:
  br label %merge
else:
  br label %merge
merge:
  %p = phi i8 addrspace(1)* [ %obj, %then ], [ %other, %else ]
  %n = phi i32 [ %x, %then ], [ 0, %else ]
  call void @use(i8 addrspace(1)* %p)
  ret i32 %n
}


; A select may yield the object, so the object is live until the select's
; result is dead.

define void @select_derived(i8* %ti, i1 %c, i8 addrspace(1)* %other) {
; CHECK-LABEL: @select_derived(
; CHECK:         %p = select i1 %c, i8 addrspace(1)* %obj, i8 addrspace(1)* %other
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %p)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
;
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  %p = select i1 %c, i8 addrspace(1)* %obj, i8 addrspace(1)* %other
  call void @use(i8 addrspace(1)* %p)
  ret void
}


; The object is normally cast to its class type before being used, so a use of
; the bitcast keeps the object alive.

define void @bitcast_derived(i8* %ti) {
; CHECK-LABEL: @bitcast_derived(
; CHECK:         %bc = bitcast i8 addrspace(1)* %obj to %Cls addrspace(1)*
; CHECK-NEXT:    call void @use_cls(%Cls addrspace(1)* %bc)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
;
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  %bc = bitcast i8 addrspace(1)* %obj to %Cls addrspace(1)*
  call void @use_cls(%Cls addrspace(1)* %bc)
  ret void
}


; Created before the loop and used inside it: the back edge keeps it live
; through the whole body, so the Remove belongs on the exit edge.

define void @loop_use(i8* %ti) {
; CHECK-LABEL: @loop_use(
; CHECK:       head:
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %obj)
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:       exit:
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
;
entry:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  br label %head
head:
  call void @use(i8 addrspace(1)* %obj)
  %c = call i1 @cond()
  br i1 %c, label %head, label %exit
exit:
  ret void
}


; Created *inside* the loop: a fresh object per iteration, each registered and
; unregistered within its own iteration.

define void @loop_new(i8* %ti) {
; CHECK-LABEL: @loop_new(
; CHECK:       head:
; CHECK:         call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK:       exit:
; CHECK-NEXT:    ret void
;
entry:
  br label %head
head:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  call void @use(i8 addrspace(1)* %obj)
  %c = call i1 @cond()
  br i1 %c, label %head, label %exit
exit:
  ret void
}


; Stored into a local variable and read back: the object is still alive after
; the store, so the Remove goes after the use of the loaded value.

define void @slot_roundtrip(i8* %ti) {
; CHECK-LABEL: @slot_roundtrip(
; CHECK:         store i8 addrspace(1)* %obj, i8 addrspace(1)** %slot
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:         %l = load i8 addrspace(1)*, i8 addrspace(1)** %slot
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %l)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
;
  %slot = alloca i8 addrspace(1)*
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  store i8 addrspace(1)* %obj, i8 addrspace(1)** %slot
  %l = load i8 addrspace(1)*, i8 addrspace(1)** %slot
  call void @use(i8 addrspace(1)* %l)
  ret void
}


; The load is fed by a store from either arm, so no single store defines it.
; Tracking the slot rather than an individual store keeps the object alive.

define void @slot_memory_phi(i8* %ti, i1 %c) {
; CHECK-LABEL: @slot_memory_phi(
; CHECK:       a:
; CHECK-NEXT:    store i8 addrspace(1)* %obj, i8 addrspace(1)** %slot
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:       b:
; CHECK-NEXT:    store i8 addrspace(1)* %obj, i8 addrspace(1)** %slot
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:       m:
; CHECK-NEXT:    %l = load i8 addrspace(1)*, i8 addrspace(1)** %slot
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %l)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
;
entry:
  %slot = alloca i8 addrspace(1)*
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  br i1 %c, label %a, label %b
a:
  store i8 addrspace(1)* %obj, i8 addrspace(1)** %slot
  br label %m
b:
  store i8 addrspace(1)* %obj, i8 addrspace(1)** %slot
  br label %m
m:
  %l = load i8 addrspace(1)*, i8 addrspace(1)** %slot
  call void @use(i8 addrspace(1)* %l)
  ret void
}


; Stored into a heap field: the object outlives this function through memory we
; cannot follow, so it is left registered rather than unregistered too early.

define void @escape_to_heap(i8* %ti, i8 addrspace(1)* addrspace(1)* %field) {
; CHECK-LABEL: @escape_to_heap(
; CHECK:         call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:         ret void
;
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  store i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field
  ret void
}
