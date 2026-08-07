; RUN: split-file %s %t
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/overflow.ll 2>&1 | FileCheck %s --check-prefix=OVERFLOW
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/nested.ll 2>&1 | FileCheck %s --check-prefix=OVERFLOW
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/struct.ll 2>&1 | FileCheck %s --check-prefix=OVERFLOW
; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/mangled.ll 2>&1 | FileCheck %s --check-prefix=MANGLED
; RUN: opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/ok.ll
; RUN: opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %t/declaration.ll
;
; A global variable is diagnosed by its own (demangled) name, not by a
; function name, because it has no enclosing function.
; OVERFLOW: The value type size exceeds the maximum representable size in global variable
; MANGLED: The value type size exceeds the maximum representable size in global variable default::hugeGlobal

;--- overflow.ll
; A global whose value type size overflows uint64 bits is rejected; the
; assembler would otherwise emit only the wrapped byte count.
@huge_global = global [2305843009213693963 x i64] zeroinitializer

define void @foo() gc "cangjie" {
entry:
  ret void
}

;--- nested.ll
; Recursive checking also catches overflow inside a nested array.
@huge_nested = global [1 x [2305843009213693963 x i64]] zeroinitializer

define void @foo() gc "cangjie" {
entry:
  ret void
}

;--- struct.ll
%struct = type { [2305843009213693963 x i64] }

@huge_struct = global %struct zeroinitializer

define void @foo() gc "cangjie" {
entry:
  ret void
}

;--- mangled.ll
; The diagnostic demangles a Cangjie-mangled global variable name.
@"_CN7default10hugeGlobalEl" = global [2305843009213693963 x i64] zeroinitializer

define void @foo() gc "cangjie" {
entry:
  ret void
}

;--- ok.ll
; A representable global is accepted.
@ok_global = global [100 x i64] zeroinitializer
@ok_scalar = global i64 42

define void @foo() gc "cangjie" {
entry:
  ret void
}

;--- declaration.ll
; A global declaration (no initializer) is not checked here: its definition,
; and thus its real extent, lives in another module.
@ext_huge = external global [2305843009213693963 x i64]

define void @foo() gc "cangjie" {
entry:
  ret void
}
