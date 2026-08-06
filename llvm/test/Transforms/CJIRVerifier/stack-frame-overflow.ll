; RUN: split-file %s %t
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/overflow.ll 2>&1 | FileCheck %s
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/count-multiply-wraps.ll 2>&1 | FileCheck %s
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/wide-count.ll 2>&1 | FileCheck %s
; RUN: opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/no-overflow.ll
; RUN: opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/dynamic.ll
;
; CHECK: stack frame size overflow

;--- overflow.ll
; Each alloca is representable on its own (2^63 bits), but their cumulative
; static frame size wraps uint64.
define void @frame_overflow() gc "cangjie" {
entry:
  %a = alloca [144115188075855872 x i64], align 8
  %b = alloca [144115188075855872 x i64], align 8
  ret void
}

;--- count-multiply-wraps.ll
; The type size (64 bits) times the constant count (2^61 + 1) wraps uint64;
; the backend's unchecked multiplication would see a tiny stack object.
define void @constant_count_multiply_wraps() gc "cangjie" {
entry:
  %a = alloca i64, i64 2305843009213693953
  ret void
}

;--- wide-count.ll
; An element count that does not fit 64 bits saturates the checked multiply
; and is rejected.
define void @wide_constant_count() gc "cangjie" {
entry:
  %a = alloca i8, i128 36893488147419103232
  ret void
}

;--- no-overflow.ll
; A frame whose size is representable flows downstream, even past the 2GB
; language limit: that limit is enforced by the backend stacksize guards,
; not by this pass.
define void @large_but_representable() gc "cangjie" {
entry:
  %a = alloca [134217728 x i64], align 8
  %b = alloca [134217728 x i64], align 8
  ret void
}

;--- dynamic.ll
; Runtime-sized allocas do not count toward the static frame size.
define void @dynamic_alloca(i64 %n) gc "cangjie" {
entry:
  %a = alloca i8, i64 %n, align 8
  ret void
}
