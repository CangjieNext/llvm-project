; RUN: split-file %s %t
; RUN: opt '-passes=default<O0>' -disable-output < %t/ordinary.ll
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/overflow.ll 2>&1 | FileCheck %s
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/nested.ll 2>&1 | FileCheck %s
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/struct.ll 2>&1 | FileCheck %s
; RUN: not --crash opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/array-of-struct.ll 2>&1 | FileCheck %s
;
; CHECK: allocated type is too large

;--- ordinary.ll
@element = external global [2305843009213693963 x i64]

; A huge array type is accepted by an ordinary LLVM pipeline when no layout
; query is needed.
define void @ordinary_pipeline() {
entry:
  %element = getelementptr [2305843009213693963 x i64], ptr @element, i64 0, i64 0
  ret void
}

;--- overflow.ll
; The Cangjie pipeline rejects an alloca whose allocated type size overflows.
define void @array_size_overflow() gc "cangjie" {
entry:
  %array = alloca [2305843009213693963 x i64], align 8
  ret void
}

;--- nested.ll
; Recursive checking also catches overflow inside a nested array.
define void @nested_array_size_overflow() gc "cangjie" {
entry:
  %array = alloca [1 x [2305843009213693963 x i64]], align 8
  ret void
}

;--- struct.ll
%struct = type { [2305843009213693963 x i64] }

define void @struct_size_overflow() gc "cangjie" {
entry:
  %value = alloca %struct, align 8
  ret void
}

;--- array-of-struct.ll
%element = type { [2305843009213693963 x i64] }

define void @array_of_struct_size_overflow() gc "cangjie" {
entry:
  %array = alloca [1 x %element], align 8
  ret void
}
