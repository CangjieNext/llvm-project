; RUN: opt < %s -passes=cj-object-reuse-opt -S | FileCheck %s

; Regression test: a ctor's arg0 cast/PHI chain may end at a GlobalVariable
; or Argument (not an Instruction). isDefChainConsistOfCastOrPHI() must
; reject such candidates instead of dereferencing the nullptr that
; dyn_cast<Instruction>() yields (isa<> on null, observed as SIGSEGV with
; fault address 0x10).

@g.obj = global i8 0

@"std.core:Object.ti" = global { i8*, i8*, i8, i32, i32 } { i8* null, i8* null, i8 0, i32 1, i32 16 } #0

declare void @"_CNat6Object6<init>HA_ll"(i8 addrspace(1)*, { i8*, i8*, i8, i32, i32 }*)

define void @arg0_cast_of_global() gc "cangjie" {
; CHECK-LABEL: @arg0_cast_of_global(
; CHECK:         call void @"_CNat6Object6<init>HA_ll"(
; CHECK-NEXT:    ret void
  %p = addrspacecast i8* @g.obj to i8 addrspace(1)*
  call void @"_CNat6Object6<init>HA_ll"(i8 addrspace(1)* %p, { i8*, i8*, i8, i32, i32 }* @"std.core:Object.ti")
  ret void
}

define void @arg0_cast_of_argument(i8* %arg) gc "cangjie" {
; CHECK-LABEL: @arg0_cast_of_argument(
; CHECK:         call void @"_CNat6Object6<init>HA_ll"(
; CHECK-NEXT:    ret void
  %p = addrspacecast i8* %arg to i8 addrspace(1)*
  call void @"_CNat6Object6<init>HA_ll"(i8 addrspace(1)* %p, { i8*, i8*, i8, i32, i32 }* @"std.core:Object.ti")
  ret void
}

attributes #0 = { "NotModifiableClass" }
