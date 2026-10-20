; RUN: opt < %s -passes=cj-runtime-lowering -S | FileCheck %s
%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, i8*, i8*, i8* }

declare i8 addrspace(1)* @llvm.cj.malloc.object(i8*, i32)
declare i8 addrspace(1)* @llvm.cj.malloc.local.object(i8*, i32)

; NewFinalier cannot add "cj-malloc" attribute, because it may call a finalier method when destructor performing.

; A heap finalizer is owned by the GC, so it is neither registered nor
; unregistered as a local one.
define void @lower_new_finalier(i8* %ti, i8** %s) {
; CHECK-LABEL: @lower_new_finalier(
; CHECK:         %obj = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* %ti, i32 %{{[0-9]+}})
; CHECK-NOT:     @CJ_MCC_AddLocalFinalizer
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
; CHECK:         ret void
;
  %obj = call noalias i8 addrspace(1)* @llvm.cj.malloc.object(i8* %ti, i32 32), !MallocType !0
  ret void
}

; A LocalMode finalizer object is registered right after it is created. The
; matching CJ_MCC_RemoveLocalFinalizer is inserted later in the pipeline by
; cj-insert-remove-local-finalizer, so it is not expected here.
define void @lower_new_local_finalier(i8* %ti) {
; CHECK-LABEL: @lower_new_local_finalier(
; CHECK:         %obj = call noalias i8 addrspace(1)* @CJ_MCC_NewLocalObject(i8* %ti, i32 %{{[0-9]+}})
; CHECK-NEXT:    call void @CJ_MCC_AddLocalFinalizer(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
; CHECK-NOT:     @CJ_MCC_RemoveLocalFinalizer
;
  %obj = call noalias i8 addrspace(1)* @llvm.cj.malloc.local.object(i8* %ti, i32 32), !MallocType !0
  ret void
}

; CHECK:       declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32) #[[RT:[0-9]+]]
; CHECK:       attributes #[[RT]] = { "cj-runtime" }

!0 = !{!"HasFinalizer"}
