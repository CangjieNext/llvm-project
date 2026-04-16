; RUN: opt -S -mergefunc < %s | FileCheck %s

declare void @sink(i64)

define void @cangjie_small_a(i64 %x) gc "cangjie" {
entry:
  ret void
}

define void @cangjie_small_b(i64 %x) gc "cangjie" {
entry:
  ret void
}

define void @cangjie_profitable_a(i64 %x) gc "cangjie" {
entry:
  %0 = add i64 %x, 1
  %1 = add i64 %0, 2
  %2 = add i64 %1, 3
  call void @sink(i64 %2)
  ret void
}

define void @cangjie_profitable_b(i64 %x) gc "cangjie" {
entry:
  %0 = add i64 %x, 1
  %1 = add i64 %0, 2
  %2 = add i64 %1, 3
  call void @sink(i64 %2)
  ret void
}

; CHECK-LABEL: @cangjie_small_a(
; CHECK-NOT: tail call void @cangjie_small_a(
; CHECK: ret void

; CHECK-LABEL: @cangjie_small_b(
; CHECK-NOT: tail call void @cangjie_small_a(
; CHECK: ret void

; CHECK-LABEL: @cangjie_profitable_a(
; CHECK: call void @sink(i64
; CHECK: ret void

; CHECK-LABEL: @cangjie_profitable_b(
; CHECK: tail call void @cangjie_profitable_a(
; CHECK-NEXT: ret void
