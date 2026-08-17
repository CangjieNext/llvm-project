; RUN: split-file %s %t
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/member.ll 2>&1 | FileCheck %s
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/member-sum.ll 2>&1 | FileCheck %s
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/global-member.ll 2>&1 | FileCheck %s --check-prefix=GLOBAL
; RUN: opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/ok.ll
;
; CHECK: The allocation type size exceeds the maximum representable size
; GLOBAL: The value type size exceeds the maximum representable size in global variable

;--- member.ll
; A struct member that is itself an overflowing array (e.g. a struct with a
; huge VArray member) must be rejected: DataLayout's struct size would wrap
; silently and hide the member's overflow.
define void @struct_member_overflow() gc "cangjie" {
entry:
  %s = alloca { [2305843009213693963 x i64] }, align 8
  ret void
}

;--- member-sum.ll
; Two individually representable members (2^66 bits each) whose sum overflows
; uint64 bits (2^67) are rejected too.
define void @struct_member_sum_overflow() gc "cangjie" {
entry:
  %s = alloca { [1152921504606846976 x i64], [1152921504606846976 x i64] }, align 8
  ret void
}

;--- global-member.ll
; The same check applies to a global variable whose struct value type has an
; overflowing array member.
@s = global { [2305843009213693963 x i64] } zeroinitializer

;--- ok.ll
; A struct whose members are all representable is accepted.
define void @struct_ok() gc "cangjie" {
entry:
  %s = alloca { i64, [16 x i64] }, align 8
  ret void
}
