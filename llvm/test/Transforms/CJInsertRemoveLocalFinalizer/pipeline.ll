; Mirrors the real pass ordering: CJRuntimeLowering emits the registration, the
; optimizer runs, and only then does this pass pair it up. The point is that
; CJ_MCC_AddLocalFinalizer survives optimization and is still pairable, and that
; the pass copes with the optimized shapes (promoted slots, threaded branches)
; rather than the raw frontend ones.
;
; RUN: opt < %s -S \
; RUN:   -passes='cj-runtime-lowering,function(sroa,instcombine,simplifycfg),function(cj-insert-remove-local-finalizer)' \
; RUN:   | FileCheck %s

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, i8*, i8*, i8* }

declare i8 addrspace(1)* @llvm.cj.malloc.local.object(i8*, i32)
declare void @use(i8 addrspace(1)*)
declare void @mayThrow()
declare void @cleanupAction()
declare i32 @personality_function()


; The local variable slot is promoted by SROA, so by the time this pass runs the
; object is plain SSA and the whole diamond has collapsed: one registration, one
; unregistration.

define void @slot_promoted_away(i8* %ti, i1 %c) gc "cangjie" {
; CHECK-LABEL: @slot_promoted_away(
; CHECK:         call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @use(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
;
entry:
  %slot = alloca i8 addrspace(1)*
  %obj = call noalias i8 addrspace(1)* @llvm.cj.malloc.local.object(i8* %ti, i32 32), !MallocType !0
  store i8 addrspace(1)* %obj, i8 addrspace(1)** %slot
  br i1 %c, label %a, label %b
a:
  br label %m
b:
  br label %m
m:
  %l = load i8 addrspace(1)*, i8 addrspace(1)** %slot
  call void @use(i8 addrspace(1)* %l)
  ret void
}


; Optimization does not disturb the exception edges: the object is still
; unregistered on both the normal and the unwind path, once each.

define void @eh_survives_opt(i8* %ti) gc "cangjie" personality i32 ()* @personality_function {
; CHECK-LABEL: @eh_survives_opt(
; CHECK:         call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK:         invoke void @mayThrow()
; CHECK:         call void @use(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK:         landingpad token
; CHECK-NEXT:      cleanup
; CHECK-NEXT:    call void @CJ_MCC_RemoveLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    call void @cleanupAction()
; CHECK-NOT:     call void @CJ_MCC_RemoveLocalFinalizer
;
entry:
  %obj = call noalias i8 addrspace(1)* @llvm.cj.malloc.local.object(i8* %ti, i32 32), !MallocType !0
  invoke void @mayThrow() to label %cont unwind label %lpad
cont:
  call void @use(i8 addrspace(1)* %obj)
  ret void
lpad:
  %l = landingpad token cleanup
  call void @cleanupAction()
  ret void
}

!0 = !{!"HasFinalizer"}
