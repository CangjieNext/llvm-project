; RUN: split-file %s %t
; RUN: not llc --cangjie-pipeline %t/wrap.ll -o /dev/null 2>&1 | FileCheck %s
; RUN: not llc --cangjie-pipeline %t/large.ll -o /dev/null 2>&1 | FileCheck %s
; RUN: llc --cangjie-pipeline %t/small.ll -o /dev/null
;
; CHECK: exceeds cangjie max stacksize(2GB)

;--- wrap.ll
; 8 giant allocas of 2^61-8 bytes plus one 64-byte alloca: the true frame
; total is exactly 2^64 bytes, so MachineFrameInfo's uint64 stack size wraps
; to 0. The checked object-size sum must still reject it.
define void @wrap() gc "cangjie" {
entry:
  %a1 = alloca [288230376151711743 x i64], align 8
  %a2 = alloca [288230376151711743 x i64], align 8
  %a3 = alloca [288230376151711743 x i64], align 8
  %a4 = alloca [288230376151711743 x i64], align 8
  %a5 = alloca [288230376151711743 x i64], align 8
  %a6 = alloca [288230376151711743 x i64], align 8
  %a7 = alloca [288230376151711743 x i64], align 8
  %a8 = alloca [288230376151711743 x i64], align 8
  %pad = alloca [8 x i64], align 8
  ret void
}

;--- large.ll
; Two giant allocas whose sum (2^62-16 bytes) exceeds 2GB without wrapping:
; rejected through the fast path on the reported frame size.
define void @large() gc "cangjie" {
entry:
  %a1 = alloca [288230376151711743 x i64], align 8
  %a2 = alloca [288230376151711743 x i64], align 8
  ret void
}

;--- small.ll
; A normal small frame is accepted.
define i64 @small() gc "cangjie" {
entry:
  %a = alloca [16 x i64], align 8
  ret i64 0
}
