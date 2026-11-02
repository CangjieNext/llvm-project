; RUN: opt < %s -passes=cj-insert-remove-local-finalizer -S | FileCheck %s

; Unwinding out of a local finalizer's live range is just another way for it to
; die, and a landing pad shared by several invokes must still be unregistered
; only once.
;
; The landing pads here are `landingpad token`, which is what the Cangjie
; frontend emits and what CJRewriteStatepoint requires -- it uses the landing
; pad as the token operand of the exceptional gc.relocates it generates.

declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32)
declare void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)*)
declare void @use(i8 addrspace(1)*)
declare void @mayThrow()
declare void @cleanupAction()
declare i32 @personality_function()


; The object survives the invoke on the normal path but is dead on the unwind
; path, which never mentions it. Without a Remove in the landing pad the
; registration would leak whenever the call throws.

define void @live_across_invoke(i8* %ti) gc "cangjie" personality i32 ()* @personality_function {
; CHECK-LABEL: @live_across_invoke(
; CHECK:         invoke void @mayThrow()
; CHECK:       cont:
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
; CHECK:       lpad:
; CHECK-NEXT:    landingpad token
; CHECK-NEXT:      cleanup
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @cleanupAction()
;
entry:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  invoke void @mayThrow() to label %cont unwind label %lpad
cont:
  call void @use(i8 addrspace(1)* %obj)
  ret void
lpad:
  %l = landingpad token cleanup
  call void @cleanupAction()
  ret void
}


; Two invokes consume the object and share both successors. Every edge into
; each successor is dead, so a single Remove per successor is enough -- one per
; incoming edge would unregister the object twice.

define void @shared_landing_pad(i8* %ti, i1 %c) gc "cangjie" personality i32 ()* @personality_function {
; CHECK-LABEL: @shared_landing_pad(
; CHECK:       exit:
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
; CHECK:       lpad:
; CHECK-NEXT:    landingpad token
; CHECK-NEXT:      cleanup
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @cleanupAction()
;
entry:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  br i1 %c, label %b1, label %b2
b1:
  invoke void @use(i8 addrspace(1)* %obj) to label %exit unwind label %lpad
b2:
  invoke void @use(i8 addrspace(1)* %obj) to label %exit unwind label %lpad
exit:
  ret void
lpad:
  %l = landingpad token cleanup
  call void @cleanupAction()
  ret void
}


; Here the object only exists on the %b1 path, while both successors are also
; reached from %b2. The Remove cannot go into a shared successor -- %obj does
; not dominate it -- so both critical edges out of %b1 are split, the unwind one
; by cloning the landing pad.

define void @split_critical_edges(i8* %ti, i1 %c) gc "cangjie" personality i32 ()* @personality_function {
; CHECK-LABEL: @split_critical_edges(
; CHECK:       b1:
; CHECK:         call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    invoke void @use(i8 addrspace(1)* %obj)
; CHECK-NEXT:      to label %[[NORMAL:.*]] unwind label %[[UNWIND:.*]]
; CHECK:       [[NORMAL]]:
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    br label %exit
; CHECK:       b2:
; CHECK-NEXT:    invoke void @mayThrow()
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:       exit:
; CHECK-NEXT:    ret void
; CHECK:       [[UNWIND]]:
; CHECK-NEXT:    landingpad token
; CHECK-NEXT:      cleanup
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NOT:     call void @CJ_MCC_RemoveLocalFinalizer
;
entry:
  br i1 %c, label %b1, label %b2
b1:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 32)
  call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
  invoke void @use(i8 addrspace(1)* %obj) to label %exit unwind label %lpad
b2:
  invoke void @mayThrow() to label %exit unwind label %lpad
exit:
  ret void
lpad:
  %l = landingpad token cleanup
  call void @cleanupAction()
  ret void
}
