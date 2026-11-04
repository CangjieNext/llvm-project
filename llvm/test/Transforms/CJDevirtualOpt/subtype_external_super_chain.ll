; RUN: opt '-passes=cj-devirtual-opt' --cangjie-pipeline -S < %s | FileCheck %s
;
; `a is B` where `a` is an `A` instance: walking the super class chain of `A`
; reaches `std.core:Object`, which is declared in another module and has no
; initializer here, so `A <: B` can not be decided from the metadata visible in
; this module. The runtime call must be preserved instead of being folded.

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8**, i8*, i8* }

@"std.core:Object.ti" = external global %TypeInfo
@"default:A.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"std.core:Object.ti", i8** null, i8* null, i8* null }
@"default:B.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"std.core:Object.ti", i8** null, i8* null, i8* null }

; CHECK-LABEL: define i1 @test(
; CHECK: call i1 @CJ_MCC_IsSubType
define i1 @test() {
  %r = call i1 @CJ_MCC_IsSubType(i8* bitcast (%TypeInfo* @"default:A.ti" to i8*), i8* bitcast (%TypeInfo* @"default:B.ti" to i8*))
  ret i1 %r
}

declare i1 @CJ_MCC_IsSubType(i8*, i8*)
