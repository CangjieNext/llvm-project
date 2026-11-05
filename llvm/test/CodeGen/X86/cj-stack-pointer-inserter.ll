; RUN: llc -O0 --cangjie-pipeline -mattr=-avx -print-after=cangjie-stack-pointer-inserter %s -o - 2>&1 | FileCheck %s
;
; Regression test for the Cangjie stack pointer inserter: every GC statepoint
; that is live on a frame slot holding a reference must be rewritten with a
; non-zero number of stack pointers (the `STATEPOINT <num-stack-ptrs>, ...`
; field).  The pass used to recompute the live set of each statepoint by
; rescanning its basic block from the end (quadratic in the number of
; statepoints per block); it now walks each block once, backwards, and
; snapshots the live set at every statepoint.  Both statepoints below must
; still be rewritten, and the emitted stack map record for the function must
; still be produced.

target datalayout = "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-w64-windows-gnu"

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8**, i8*, i8* }
%Hdr = type { i8 addrspace(1)*, i64 }

@ti = global %TypeInfo zeroinitializer

declare token @llvm.cj.gc.statepoint(...)
declare i8 addrspace(1)* @llvm.cj.gc.result.p1i8(token)
declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32)
declare cangjiegccc void @CJ_MCC_HandleSafepoint()
declare void @sink(i8 addrspace(1)*)

define internal void @init_hdr(%Hdr* sret(%Hdr) %h, i8 addrspace(1)* %p) gc "cangjie" {
entry:
  %gep = getelementptr inbounds %Hdr, %Hdr* %h, i32 0, i32 0
  store i8 addrspace(1)* %p, i8 addrspace(1)** %gep, align 8
  %len = getelementptr inbounds %Hdr, %Hdr* %h, i32 0, i32 1
  store i64 2, i64* %len, align 8
  ret void
}

define void @sret_slot_live() gc "cangjie" {
entry:
  %h = alloca %Hdr, align 8
  %t1 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8*, i32)* @CJ_MCC_NewObject, i32 2, i32 0, i8* bitcast (%TypeInfo* @ti to i8*), i32 16)
  %a = call i8 addrspace(1)* @llvm.cj.gc.result.p1i8(token %t1)
  call void @init_hdr(%Hdr* sret(%Hdr) %h, i8 addrspace(1)* %a)
  %sp1 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @CJ_MCC_HandleSafepoint, i32 0, i32 0)
  %sp2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @CJ_MCC_HandleSafepoint, i32 0, i32 0)
  %gep = getelementptr inbounds %Hdr, %Hdr* %h, i32 0, i32 0
  %v = load i8 addrspace(1)*, i8 addrspace(1)** %gep, align 8
  call void @sink(i8 addrspace(1)* %v)
  ret void
}

; The pass ran on the function.
; CHECK: # *** IR Dump After Cangjie Stack Pointers Inserter (cangjie-stack-pointer-inserter) ***:

; Both statepoints that are dominated by the "sret slot holds a reference"
; fact must be rewritten with a non-zero stack pointer count.  The two
; statepoints are identical in the machine dump, so exactly two occurrences
; are expected.
; CHECK-COUNT-2: STATEPOINT {{[1-9][0-9]*}}, 0, 0, @CJ_Safepoint_Stub,

; The function still gets a stack map record in the .cjsm section.
; CHECK: .Lstack_map.sret_slot_live:
