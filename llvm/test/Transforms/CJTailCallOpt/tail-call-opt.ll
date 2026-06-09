; RUN: opt -verify-each -cj-tail-call-opt -S < %s | FileCheck %s
; RUN: opt -verify-each -passes=cj-tail-call-opt -S < %s | FileCheck %s

%Unit.Type = type {}
%NonEmpty = type { i64 }

declare void @void_callee() gc "cangjie"
declare i64 @i64_callee(i64) gc "cangjie"
declare void @unit_callee(%Unit.Type* noalias sret(%Unit.Type)) gc "cangjie"
declare void @unit_and_ptr_callee(%Unit.Type* noalias sret(%Unit.Type), i8*) gc "cangjie"
declare void @non_empty_callee(%NonEmpty* noalias sret(%NonEmpty)) gc "cangjie"
declare void @ptr_callee(i8*) gc "cangjie"

define void @mark_void_tail() gc "cangjie" {
; CHECK-LABEL: define void @mark_void_tail(
; CHECK: tail call void @void_callee()
; CHECK-NEXT: ret void
entry:
  call void @void_callee()
  ret void
}

define i64 @mark_return_value(i64 %x) gc "cangjie" {
; CHECK-LABEL: define i64 @mark_return_value(
; CHECK: %r = tail call i64 @i64_callee(i64 %x)
; CHECK-NEXT: ret i64 %r
entry:
  %r = call i64 @i64_callee(i64 %x)
  ret i64 %r
}

define void @forward_unit_sret(%Unit.Type* noalias sret(%Unit.Type) %out) gc "cangjie" {
; CHECK-LABEL: define void @forward_unit_sret(
; CHECK: tail call void @unit_callee(%Unit.Type* noalias sret(%Unit.Type) %out)
; CHECK-NEXT: ret void
entry:
  %tmp = alloca %Unit.Type, align 8
  call void @unit_callee(%Unit.Type* noalias sret(%Unit.Type) %tmp)
  ret void
}

define void @keep_notail() gc "cangjie" {
; CHECK-LABEL: define void @keep_notail(
; CHECK: notail call void @void_callee()
; CHECK-NEXT: ret void
entry:
  notail call void @void_callee()
  ret void
}

define void @skip_non_cangjie() {
; CHECK-LABEL: define void @skip_non_cangjie(
; CHECK: call void @void_callee()
; CHECK-NEXT: ret void
entry:
  call void @void_callee()
  ret void
}

define void @skip_non_empty_sret(%NonEmpty* noalias sret(%NonEmpty) %out) gc "cangjie" {
; CHECK-LABEL: define void @skip_non_empty_sret(
; CHECK: %tmp = alloca %NonEmpty
; CHECK-NEXT: call void @non_empty_callee(%NonEmpty* noalias sret(%NonEmpty) %tmp)
; CHECK-NEXT: ret void
entry:
  %tmp = alloca %NonEmpty, align 8
  call void @non_empty_callee(%NonEmpty* noalias sret(%NonEmpty) %tmp)
  ret void
}

define void @skip_local_alloca_arg() gc "cangjie" {
; CHECK-LABEL: define void @skip_local_alloca_arg(
; CHECK: %slot = alloca i8
; CHECK-NEXT: call void @ptr_callee(i8* %slot)
; CHECK-NEXT: ret void
entry:
  %slot = alloca i8, align 1
  call void @ptr_callee(i8* %slot)
  ret void
}

define void @skip_nonzero_local_alloca_not_passed() gc "cangjie" {
; CHECK-LABEL: define void @skip_nonzero_local_alloca_not_passed(
; CHECK: %slot = alloca i8
; CHECK-NEXT: call void @void_callee()
; CHECK-NEXT: ret void
entry:
  %slot = alloca i8, align 1
  call void @void_callee()
  ret void
}

define void @skip_unit_sret_with_other_alloca_arg(%Unit.Type* noalias sret(%Unit.Type) %out) gc "cangjie" {
; CHECK-LABEL: define void @skip_unit_sret_with_other_alloca_arg(
; CHECK: %tmp = alloca %Unit.Type
; CHECK-NEXT: %slot = alloca i8
; CHECK-NEXT: call void @unit_and_ptr_callee(%Unit.Type* noalias sret(%Unit.Type) %tmp, i8* %slot)
; CHECK-NEXT: ret void
entry:
  %tmp = alloca %Unit.Type, align 8
  %slot = alloca i8, align 1
  call void @unit_and_ptr_callee(%Unit.Type* noalias sret(%Unit.Type) %tmp, i8* %slot)
  ret void
}

define void @skip_disable_tail_calls() #0 gc "cangjie" {
; CHECK-LABEL: define void @skip_disable_tail_calls(
; CHECK: call void @void_callee()
; CHECK-NEXT: ret void
entry:
  call void @void_callee()
  ret void
}

attributes #0 = { "disable-tail-calls"="true" }
