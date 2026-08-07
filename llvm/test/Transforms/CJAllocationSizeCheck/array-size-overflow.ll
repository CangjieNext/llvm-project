; RUN: split-file %s %t
; RUN: opt '-passes=default<O0>' -disable-output < %t/ordinary.ll
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/overflow.ll 2>&1 | FileCheck %s
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/nested.ll 2>&1 | FileCheck %s
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/struct.ll 2>&1 | FileCheck %s
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/array-of-struct.ll 2>&1 | FileCheck %s
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/dynamic.ll 2>&1 | FileCheck %s
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/no-gc.ll 2>&1 | FileCheck %s
;
; CHECK: The allocation type size exceeds the maximum representable size

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

;--- dynamic.ll
; A runtime-sized alloca is not part of the static frame, but its allocated
; type must still be representable.
define void @dynamic_count_huge_type(i64 %n) gc "cangjie" {
entry:
  %a = alloca [2305843009213693963 x i64], i64 %n
  ret void
}

;--- no-gc.ll
; A function that does not use the Cangjie GC (e.g. the synthesized
; 0_for_keeping_some_types, which keeps huge types alive with bare allocas)
; is checked too: an oversized allocation is a resource limit independent of
; the GC.
define private void @no_gc_huge_type() {
entry:
  %a = alloca [2305843009213693963 x i64], align 8
  ret void
}
