; RUN: llc -verify-machineinstrs < %s | FileCheck %s

; A tail statepoint with an indirect callee whose address must survive an
; intervening call. Lowering the tail jump through a callee-saved register is
; unsafe: the epilogue restores that register right before the tail jump, so
; the jump target becomes garbage (see use_interface_def_imple_083 crash).
; Indirect callees must be lowered as a regular call + ret.

target datalayout = "e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-linux-gnu"

declare i8* @getfn()
declare i8* @clobber(i8*)
declare token @llvm.cj.gc.statepoint(...)

define void @test(i8 addrspace(1)** %sret) gc "statepoint-example" {
; CHECK-LABEL: test:
; CHECK:       callq	getfn@PLT
; CHECK-NEXT:  movq	%rax, %rbx
; CHECK-NEXT:  movq	%rax, %rdi
; CHECK-NEXT:  callq	clobber@PLT
; CHECK-NEXT:  movq	%r14, %rdi
; CHECK-NEXT:  callq	*%rbx
; CHECK-NOT:   TAILCALL
; CHECK:       retq
entry:
  %fn = call i8* @getfn()
  %f = bitcast i8* %fn to void (i8 addrspace(1)**)*
  %dummy = call i8* @clobber(i8* %fn)
  %tok = tail call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (i8 addrspace(1)**)* %f, i32 1, i32 0, i8 addrspace(1)** %sret)
  ret void
}
