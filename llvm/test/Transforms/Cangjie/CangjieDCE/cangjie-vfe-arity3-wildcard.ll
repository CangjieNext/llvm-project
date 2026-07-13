; RUN: opt < %s -passes='globaldce' --cangjie-pipeline -S | FileCheck %s

; This test extends the VFE arity coverage with:
;   1. arity-3 flat encoding !{!"T.tt", a, b, c} (3 siblings)
;   2. the "$g" sentinel for an uninstantiated generic type parameter, which
;      must NOT crash the decoder and must resolve conservatively (no GV named
;      "$g" exists, so that dimension contributes nothing to the Path; the call
;      site resolves at the nearest enclosing instantiated type).
;
; Scenario:
;   interface J<A, B, C>            (TypeInfo "default:J.ti", internal -> VFE-safe)
;   class R  <: J<Int32, Int64, X>  (arity-3 instantiation; DEAD - no call site)
;   interface I<T>                  (TypeInfo "default:I.ti", internal -> VFE-safe)
;   class S  <: I<Wildcard>         (call site uses a "$g" sentinel -> Path [I_TT])
;
; Metadata:
;   !0 = !{!"default:J.ti", !"X.ti", !"Y.ti", !"Z.ti"}   (R's inheritedType, arity-3)
;   !1 = !{!"default:I.ti", !{!"$g"}}                  (call site objType with sentinel)
;
; Expectations:
;   - arity-3 R's J.ft is DEAD (zeroed): no call site reaches [J_TI, X_TI, Y_TI, Z_TI].
;   - The sentinel-bearing call site resolves to Path [I_TI] (sentinel dropped),
;     and the downward closure must cover any I<...> ExtensionDef registered under
;     that path. Here there is none, so nothing extra happens, but the decoder must
;     not assert/crash on the "$g" sentinel.

%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8*, %ExtensionDef**, i16 }
%ExtensionDef = type { i32, i8, i8, i16, i8*, i8*, i8*, i8* }
%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%Unit.Type = type {}

@"default:J.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null } #1
@"default:I.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null } #1
@"default:X.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null } #1
@"default:Y.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null } #1
@"default:Z.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null } #1

@"default:R.tt" = private global %TypeTemplate { i8* null, i8 -127, i8 0, i16 0, i16 3, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** getelementptr inbounds ([2 x %ExtensionDef*], [2 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 0), i16 -32768 } #2

; arity-3 ExtensionDef: R implements J<X, Y, Z>.
; inheritedType = !{!"J.ti", !"X.ti", !"Y.ti", !"Z.ti"} (NEW flat 4-operand node).
; DEAD: no call site reaches Path [J_TI, X_TI, Y_TI, Z_TI].
; CHECK: @"default:R_ed_default:J.ft" = private global [1 x i8*] zeroinitializer
@"default:R_ed_default:J.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @"_CN7default1RIJ3XYZ_E7func_JHv" to i8*)]
@"default:R_ed_default:J" = private global %ExtensionDef { i32 3, i8 1, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:R.tt" to i8*), i8* bitcast (%TypeInfo* @"default:J.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:R_ed_default:J.ft" to i8*) }, !inheritedType !0 #0

@NonExternalExtensionDefs = private global [2 x %ExtensionDef*] [%ExtensionDef* @"default:R_ed_default:J", %ExtensionDef* null]
@llvm.used = appending global [1 x i8*] [i8* bitcast ([2 x %ExtensionDef*]* @NonExternalExtensionDefs to i8*)]

declare i8* @llvm.cj.vfe.info(i8*, metadata, i64)

define internal void @"_CN7default1RIJ3XYZ_E7func_JHv"(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
  ret void
}

; Call site carrying a "$g" sentinel objType: !{!"I.ti", !{!"$g"}}.
; The decoder must flatten this to Path [I_TI] (sentinel names no GV -> dropped).
; No I<...> ExtensionDef exists in this module, so this resolves to nothing live,
; but the pass must not crash on the sentinel.
define void @use_I_wildcard(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %v, %TypeInfo* %ti.T) gc "cangjie" {
allocas:
  %1 = alloca %Unit.Type, align 8
  %2 = bitcast i8 addrspace(1)* %v to %TypeInfo* addrspace(1)*
  %ti = load %TypeInfo*, %TypeInfo* addrspace(1)* %2, align 8
  %3 = bitcast %TypeInfo* %ti to i8*
  %4 = bitcast %TypeInfo* %ti.T to i8*
  %5 = call i8* @CJ_MCC_GetMTable(i8* %3, i8* %4)
  %6 = bitcast i8* %5 to i8**
  %7 = load i8*, i8** %6, align 8
  %8 = call i8* @llvm.cj.vfe.info(i8* %7, metadata !1, i64 0)
  %9 = bitcast i8* %8 to void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)*
  call void %9(%Unit.Type* noalias sret(%Unit.Type) %1, i8 addrspace(1)* %v, %TypeInfo* %ti.T)
  ret void
}

declare i8* @CJ_MCC_GetMTable(i8*, i8*)

attributes #0 = { "CFileMTable" }
attributes #1 = { "CFileKlass" }
attributes #2 = { "cj_tt" }

; arity-3 flat multi-operand encoding -> Path [J_TI, X_TI, Y_TI, Z_TI].
!0 = !{!"default:J.ti", !"default:X.ti", !"default:Y.ti", !"default:Z.ti"}
; objType with a "$g" sentinel for an uninstantiated generic type parameter.
; Flattens to Path [I_TI] (the "$g" names no global, so it is dropped).
!1 = !{!"default:I.ti", !{!"$g"}}
