; RUN: opt < %s -passes=cj-object-reuse-opt -S | FileCheck %s

; Regression test for https://gitcode.com/Cangjie/llvm-project/issues/188
; For a generic class (e.g. Array<T>) the CIT_SIZE field (operand #4) of the
; type-info global is a ConstantExpr, not a ConstantInt. collectNoEscapeValue()
; must skip such candidates instead of asserting in getUniqueInteger().

@g.size.holder = global i8 0

@"std.core:Array<std.core:Object>.ti" = global { i8*, i8*, i8, i32, i32 } { i8* null, i8* null, i8 0, i32 1, i32 ptrtoint (i8* @g.size.holder to i32) } #0

declare void @"_CNat5ArrayIG_E6<init>HA0_G_ll"(i8 addrspace(1)*, { i8*, i8*, i8, i32, i32 }*)

define void @test_generic_ti_size_is_constexpr() gc "cangjie" {
; CHECK-LABEL: @test_generic_ti_size_is_constexpr(
; CHECK:         call void @"_CNat5ArrayIG_E6<init>HA0_G_ll"(
; CHECK-NEXT:    ret void
  %obj = alloca i8, align 8
  %obj.gc = addrspacecast i8* %obj to i8 addrspace(1)*
  call void @"_CNat5ArrayIG_E6<init>HA0_G_ll"(i8 addrspace(1)* %obj.gc, { i8*, i8*, i8, i32, i32 }* @"std.core:Array<std.core:Object>.ti")
  ret void
}

attributes #0 = { "NotModifiableClass" }
