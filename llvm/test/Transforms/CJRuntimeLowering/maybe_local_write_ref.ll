; RUN: opt < %s -passes=cj-runtime-lowering -S | FileCheck %s

; llvm.cj.maybe.local.write.ref is emitted for a store into an instance field whose owning
; object may live in a local region (`this` inside `init(this @local?)`). It must be lowered
; to a plain call to CJ_MCC_MaybeLocalWriteRef.
;
; Two properties matter here:
;  1. The argument order (obj, field, value) must be preserved. It deliberately differs from
;     llvm.cj.gcwrite.ref, which is (value, obj, field), because it matches the runtime ABI.
;  2. The lowering must happen in this pass rather than in CJBarrierLowering. Although this is
;     a write barrier, it must additionally register the owning local object as a GC root
;     unconditionally, and CJBarrierLowering's fast path would replace the barrier with a plain
;     store whenever the GC phase is idle, silently dropping that registration.

declare void @llvm.cj.maybe.local.write.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

define void @maybe_local_write(i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field, i8 addrspace(1)* %value) gc "cangjie" {
; CHECK-LABEL: define void @maybe_local_write
; CHECK: call void @CJ_MCC_MaybeLocalWriteRef(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
; CHECK-NOT: llvm.cj.maybe.local.write.ref
entry:
  call void @llvm.cj.maybe.local.write.ref(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %field)
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

; The runtime function must be declared as a GC leaf: CJ_MCC_MaybeLocalWriteRef is exported as
; a plain alias without a callee-saved-register stub, so it cannot act as a safepoint.
; CHECK: declare void @CJ_MCC_MaybeLocalWriteRef(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*) #[[ATTR:[0-9]+]]
; CHECK: attributes #[[ATTR]] = {{{.*}}"gc-leaf-function"{{.*}}}
