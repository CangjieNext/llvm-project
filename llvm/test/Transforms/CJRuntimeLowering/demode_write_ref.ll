; RUN: opt < %s -passes=cj-runtime-lowering -S | FileCheck %s

; llvm.cj.demode.write.ref is emitted for a store into a `demode` instance field. A demoded
; field always has mode `~local`, so the stored value is statically known to be on the heap,
; while the owning object may still live in a local region -- i.e. this explicitly creates a
; region -> heap edge. It must be lowered to a plain call to CJ_MCC_DemodeWriteRef.
;
; Same two properties as maybe_local_write_ref.ll:
;  1. The argument order (obj, field, value) must be preserved -- it matches the runtime ABI
;     and differs from llvm.cj.gcwrite.ref, which is (value, obj, field).
;  2. The lowering must happen in this pass rather than in CJBarrierLowering, whose fast path
;     would replace the barrier with a plain store when the GC phase is idle, silently dropping
;     the local-GC-root registration.
;
; It is kept separate from llvm.cj.maybe.local.write.ref so the runtime does not have to
; re-check whether the stored value is region-allocated.

declare void @llvm.cj.demode.write.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.maybe.local.write.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

define void @demode_write(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field) gc "cangjie" {
; CHECK-LABEL: define void @demode_write
; CHECK: call void @CJ_MCC_DemodeWriteRef(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
; CHECK-NOT: llvm.cj.demode.write.ref
entry:
  call void @llvm.cj.demode.write.ref(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
  ret void
}

; The two modal barriers must stay distinct runtime calls.
define void @both_modal_barriers(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field) gc "cangjie" {
; CHECK-LABEL: define void @both_modal_barriers
; CHECK: call void @CJ_MCC_MaybeLocalWriteRef
; CHECK: call void @CJ_MCC_DemodeWriteRef
entry:
  call void @llvm.cj.maybe.local.write.ref(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
  call void @llvm.cj.demode.write.ref(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
  ret void
}

; An ordinary reference write barrier is left untouched by this pass; it is lowered later, by
; CJBarrierLowering.
define void @ordinary_write(i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field, i8 addrspace(1)* %value) gc "cangjie" {
; CHECK-LABEL: define void @ordinary_write
; CHECK: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
entry:
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
  ret void
}

; The runtime function must be declared as a GC leaf: CJ_MCC_DemodeWriteRef is exported as a
; plain alias without a callee-saved-register stub, so it cannot act as a safepoint.
; CHECK: declare void @CJ_MCC_DemodeWriteRef(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*) #[[ATTR:[0-9]+]]
; CHECK: attributes #[[ATTR]] = {{{.*}}"gc-leaf-function"{{.*}}}
