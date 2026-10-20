; RUN: opt < %s -passes=cj-runtime-lowering -S | FileCheck %s

; llvm.cj.maybe.local.write.generic is emitted for a generic (unspecified-layout) value
; written into an object that may live in a local region. It must be lowered to a plain
; call to CJ_MCC_MaybeLocalWriteGeneric with the argument order (value, obj, field, size)
; preserved, and the runtime function must be declared as a GC leaf (no safepoint stub),
; mirroring the ref-typed sibling CJ_MCC_MaybeLocalWriteRef.

declare void @llvm.cj.maybe.local.write.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i32)

define void @maybe_local_write_generic(i8 addrspace(1)* %obj, i8 addrspace(1)* %field, i8 addrspace(1)* %value, i32 %size) gc "cangjie" {
; CHECK-LABEL: define void @maybe_local_write_generic
; CHECK: call void @CJ_MCC_MaybeLocalWriteGeneric(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* %field, i32 %size)
; CHECK-NOT: llvm.cj.maybe.local.write.generic
entry:
  call void @llvm.cj.maybe.local.write.generic(i8 addrspace(1)* %value, i8 addrspace(1)* %obj, i8 addrspace(1)* %field, i32 %size)
  ret void
}

; CHECK: declare void @CJ_MCC_MaybeLocalWriteGeneric(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i32) #[[ATTR:[0-9]+]]
; CHECK: attributes #[[ATTR]] = {{{.*}}"gc-leaf-function"{{.*}}}
