; RUN: opt '-passes=cj-devirtual-opt' --cangjie-pipeline -S < %s | FileCheck %s
;
; `Box(foo1)` where `foo1: (Any) -> A` is checked at runtime against
; `(Any) -> B`. The return types `A`/`B` are declared in another module, so the
; return type comparison can not be decided here and the runtime call must be
; preserved. Reading those declarations as if they had an initializer used to
; crash the pass.

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8**, i8*, i8* }

@A.ti = external global %TypeInfo
@B.ti = external global %TypeInfo
@"std.core:Any.ti" = external global %TypeInfo

@"(std.core:Any)->A.ti" = linkonce_odr global %TypeInfo { i8* null, i8 19, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 2, i16 -32768, i32* null, i8* null, i8* bitcast ([2 x %TypeInfo*]* @"(std.core:Any)->A.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"(std.core:Any)->A.ti.typeArgs" = private unnamed_addr constant [2 x %TypeInfo*] [%TypeInfo* @A.ti, %TypeInfo* @"std.core:Any.ti"]
@"(std.core:Any)->B.ti" = linkonce_odr global %TypeInfo { i8* null, i8 19, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 2, i16 -32768, i32* null, i8* null, i8* bitcast ([2 x %TypeInfo*]* @"(std.core:Any)->B.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"(std.core:Any)->B.ti.typeArgs" = private unnamed_addr constant [2 x %TypeInfo*] [%TypeInfo* @B.ti, %TypeInfo* @"std.core:Any.ti"]

@"default:$CfN7default4foo1HCNat3AnyE.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"Closure<(std.core:Any)->A>.ti", i8** null, i8* null, i8* null }
@"Closure<(std.core:Any)->A>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -125, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 1, i16 -32768, i32* null, i8* null, i8* bitcast ([1 x %TypeInfo*]* @"Closure<(std.core:Any)->A>.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"Closure<(std.core:Any)->A>.ti.typeArgs" = private unnamed_addr constant [1 x %TypeInfo*] [%TypeInfo* @"(std.core:Any)->A.ti"]
@"Closure<(std.core:Any)->B>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -125, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 1, i16 -32768, i32* null, i8* null, i8* bitcast ([1 x %TypeInfo*]* @"Closure<(std.core:Any)->B>.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"Closure<(std.core:Any)->B>.ti.typeArgs" = private unnamed_addr constant [1 x %TypeInfo*] [%TypeInfo* @"(std.core:Any)->B.ti"]

; CHECK-LABEL: define i1 @test(
; CHECK: call i1 @CJ_MCC_IsSubType
define i1 @test() {
  %r = call i1 @CJ_MCC_IsSubType(i8* bitcast (%TypeInfo* @"default:$CfN7default4foo1HCNat3AnyE.ti" to i8*), i8* bitcast (%TypeInfo* @"Closure<(std.core:Any)->B>.ti" to i8*))
  ret i1 %r
}

declare i1 @CJ_MCC_IsSubType(i8*, i8*)
