; RUN: split-file %s %t
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/overflow.ll 2>&1 | FileCheck %s
; RUN: opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/under-limit.ll
; RUN: opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/dynamic.ll
;
; CHECK: stack frame exceeds cangjie max stacksize(2GB)

;--- overflow.ll
; Two allocas, each well below the per-type size limit, accumulate past the
; 2GB stack frame limit.
define void @frame_overflow() gc "cangjie" {
entry:
  %a = alloca [201326592 x i64], align 8
  %b = alloca [201326592 x i64], align 8
  ret void
}

;--- under-limit.ll
; A single 1GB alloca stays below the 2GB stack frame limit.
define void @frame_ok() gc "cangjie" {
entry:
  %a = alloca [134217728 x i64], align 8
  ret void
}

;--- dynamic.ll
; Runtime-sized allocas do not count toward the static frame size.
define void @dynamic_alloca(i64 %n) gc "cangjie" {
entry:
  %a = alloca i8, i64 %n, align 8
  ret void
}
