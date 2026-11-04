; RUN: opt '-passes=cj-devirtual-opt' --cangjie-pipeline -S < %s | FileCheck %s
;
; `Box(foo1)` where `foo1: (A) -> Unit` is checked at runtime against
; `(I) -> Unit`, `I` being an interface defined in this module. The nested
; comparison `I <: A` depends on the extension data of `A` and can not be
; decided here, so the runtime call must be preserved. This used to hit
; `report_fatal_error("The interface type is processed in subtype.!")` and
; abort opt.

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8**, i8*, i8* }

@Unit.ti = external global %TypeInfo

@"I.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 0, i16 0, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"A.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 0, i16 0, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }

@"(A)->Unit.ti" = linkonce_odr global %TypeInfo { i8* null, i8 19, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 2, i16 -32768, i32* null, i8* null, i8* bitcast ([2 x %TypeInfo*]* @"(A)->Unit.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"(A)->Unit.ti.typeArgs" = private unnamed_addr constant [2 x %TypeInfo*] [%TypeInfo* @Unit.ti, %TypeInfo* @"A.ti"]
@"(I)->Unit.ti" = linkonce_odr global %TypeInfo { i8* null, i8 19, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 2, i16 -32768, i32* null, i8* null, i8* bitcast ([2 x %TypeInfo*]* @"(I)->Unit.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"(I)->Unit.ti.typeArgs" = private unnamed_addr constant [2 x %TypeInfo*] [%TypeInfo* @Unit.ti, %TypeInfo* @"I.ti"]

@"default:$CN7default4foo1H1AE.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"Closure<(A)->Unit>.ti", i8** null, i8* null, i8* null }
@"Closure<(A)->Unit>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -125, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 1, i16 -32768, i32* null, i8* null, i8* bitcast ([1 x %TypeInfo*]* @"Closure<(A)->Unit>.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"Closure<(A)->Unit>.ti.typeArgs" = private unnamed_addr constant [1 x %TypeInfo*] [%TypeInfo* @"(A)->Unit.ti"]
@"Closure<(I)->Unit>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -125, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 1, i16 -32768, i32* null, i8* null, i8* bitcast ([1 x %TypeInfo*]* @"Closure<(I)->Unit>.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"Closure<(I)->Unit>.ti.typeArgs" = private unnamed_addr constant [1 x %TypeInfo*] [%TypeInfo* @"(I)->Unit.ti"]

; CHECK-LABEL: define i1 @test(
; CHECK: call i1 @CJ_MCC_IsSubType
define i1 @test() {
  %r = call i1 @CJ_MCC_IsSubType(i8* bitcast (%TypeInfo* @"default:$CN7default4foo1H1AE.ti" to i8*), i8* bitcast (%TypeInfo* @"Closure<(I)->Unit>.ti" to i8*))
  ret i1 %r
}

declare i1 @CJ_MCC_IsSubType(i8*, i8*)
