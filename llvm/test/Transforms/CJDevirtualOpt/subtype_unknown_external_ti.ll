; RUN: opt '-passes=cj-devirtual-opt' --cangjie-pipeline -S < %s | FileCheck %s
;
; `Box(foo2)` where `foo2: (Any) -> Unit` is checked at runtime against
; `(Rune) -> Unit`. The check involves `Rune`/`Any` type infos which are
; declared in `std.core` and therefore have no initializer in this module, so
; the subtype relation can not be decided here and the runtime call must be
; preserved. Folding it to `false` used to make -O2 return 1 while -O0 returns
; 0 (the function parameter is contra-variant, so the runtime answers `true`).

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8**, i8*, i8* }

@Unit.ti = external global %TypeInfo
@Rune.ti = external global %TypeInfo
@"std.core:Any.ti" = external global %TypeInfo

@"(Rune)->Unit.ti" = linkonce_odr global %TypeInfo { i8* null, i8 19, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 2, i16 -32768, i32* null, i8* null, i8* bitcast ([2 x %TypeInfo*]* @"(Rune)->Unit.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"(Rune)->Unit.ti.typeArgs" = private unnamed_addr constant [2 x %TypeInfo*] [%TypeInfo* @Unit.ti, %TypeInfo* @Rune.ti]
@"(std.core:Any)->Unit.ti" = linkonce_odr global %TypeInfo { i8* null, i8 19, i8 0, i16 0, i32 8, i8* null, i32 0, i8 8, i8 2, i16 -32768, i32* null, i8* null, i8* bitcast ([2 x %TypeInfo*]* @"(std.core:Any)->Unit.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"(std.core:Any)->Unit.ti.typeArgs" = private unnamed_addr constant [2 x %TypeInfo*] [%TypeInfo* @Unit.ti, %TypeInfo* @"std.core:Any.ti"]

@"Closure<(Rune)->Unit>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -125, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 1, i16 -32768, i32* null, i8* null, i8* bitcast ([1 x %TypeInfo*]* @"Closure<(Rune)->Unit>.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"Closure<(Rune)->Unit>.ti.typeArgs" = private unnamed_addr constant [1 x %TypeInfo*] [%TypeInfo* @"(Rune)->Unit.ti"]
@"Closure<(std.core:Any)->Unit>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 -125, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 1, i16 -32768, i32* null, i8* null, i8* bitcast ([1 x %TypeInfo*]* @"Closure<(std.core:Any)->Unit>.ti.typeArgs" to i8*), i8* null, %TypeInfo* null, i8** null, i8* null, i8* null }
@"Closure<(std.core:Any)->Unit>.ti.typeArgs" = private unnamed_addr constant [1 x %TypeInfo*] [%TypeInfo* @"(std.core:Any)->Unit.ti"]

@"default:$CfN7default4foo2HCNat3AnyE.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 2, i32 16, i8* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"Closure<(std.core:Any)->Unit>.ti", i8** null, i8* null, i8* null }

; CHECK-LABEL: define i1 @test(
; CHECK: call i1 @CJ_MCC_IsSubType
define i1 @test() {
  %r = call i1 @CJ_MCC_IsSubType(i8* bitcast (%TypeInfo* @"default:$CfN7default4foo2HCNat3AnyE.ti" to i8*), i8* bitcast (%TypeInfo* @"Closure<(Rune)->Unit>.ti" to i8*))
  ret i1 %r
}

declare i1 @CJ_MCC_IsSubType(i8*, i8*)
