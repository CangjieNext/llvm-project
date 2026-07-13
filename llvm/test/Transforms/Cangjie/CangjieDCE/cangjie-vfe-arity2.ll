; RUN: opt < %s -passes='globaldce' --cangjie-pipeline -S | FileCheck %s

; This test exercises the VFE metadata decoder with the new flat multi-operand
; (arity >= 2) encoding:
;   !{!"T.tt", arg0, arg1, ...}
;
; Scenario:
;   interface J<A, B> {}            (TypeInfo "default:J.ti", internal -> VFE-safe)
;   interface K {}                  (TypeInfo "default:K.ti", internal -> VFE-safe)
;   class P  <: J<Int32, Int64> {}  (arity-2 instantiation; DEAD - no call site)
;   class Q  <: K {}                (arity-0; LIVE - call site below reaches it)
;
; Metadata (NEW flat multi-operand encoding for the arity-2 ExtensionDef):
;   !0 = !{!"default:J.ti", !"Int32.ti", !"Int64.ti"}   (P's inheritedType, arity-2)
;   !1 = !{!"default:K.ti"}                             (Q's inheritedType, arity-0)
;
; The arity-2 P ExtensionDef's function table is never reached by any call site's
; up/down closure (the only call site is on interface K), so it gets zeroed.

%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8*, %ExtensionDef**, i16 }
%ExtensionDef = type { i32, i8, i8, i16, i8*, i8*, i8*, i8* }
%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%Unit.Type = type {}

@"default:J.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null } #1
@"default:K.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null } #1
@"Int32.ti" = external global %TypeInfo #3
@"Int64.ti" = external global %TypeInfo #3

; Target types (implementing classes' TypeTemplates). Only their existence as
; GV labels matters for VFE; contents stubbed.
@"default:P.tt" = private global %TypeTemplate { i8* null, i8 -127, i8 0, i16 0, i16 2, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** getelementptr inbounds ([3 x %ExtensionDef*], [3 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 0), i16 -32768 } #2
@"default:Q.tt" = private global %TypeTemplate { i8* null, i8 -127, i8 0, i16 0, i16 0, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** getelementptr inbounds ([3 x %ExtensionDef*], [3 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 1), i16 -32768 } #2

; arity-2 ExtensionDef: P implements J<Int32, Int64>.
; inheritedType = !{!"J.ti", !"Int32.ti", !"Int64.ti"} (NEW flat multi-operand encoding).
; DEAD: no call site's up/down closure reaches Path [J_TI, Int32_TI, Int64_TI].
; CHECK: @"default:P_ed_default:J.ft" = private global [1 x i8*] zeroinitializer
@"default:P_ed_default:J.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @"_CN7default1PIJ2X_E7func_JHv" to i8*)]
@"default:P_ed_default:J" = private global %ExtensionDef { i32 2, i8 1, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:P.tt" to i8*), i8* bitcast (%TypeInfo* @"default:J.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:P_ed_default:J.ft" to i8*) }, !inheritedType !0 #0

; arity-0 ExtensionDef: Q implements K.
; inheritedType = !{!"K.ti"} (legacy single-operand encoding, must still work).
; LIVE: the call site below has objType !1 = !{!"K.ti"} -> Path [K_TI], which
; reaches Q exactly.
; CHECK: @"default:Q_ed_default:K.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @_CN7default1QK_E7func_KHv to i8*)]
@"default:Q_ed_default:K.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @"_CN7default1QK_E7func_KHv" to i8*)]
@"default:Q_ed_default:K" = private global %ExtensionDef { i32 0, i8 1, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:Q.tt" to i8*), i8* bitcast (%TypeInfo* @"default:K.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:Q_ed_default:K.ft" to i8*) }, !inheritedType !1 #0

@NonExternalExtensionDefs = private global [3 x %ExtensionDef*] [%ExtensionDef* @"default:P_ed_default:J", %ExtensionDef* @"default:Q_ed_default:K", %ExtensionDef* null]
@llvm.used = appending global [1 x i8*] [i8* bitcast ([3 x %ExtensionDef*]* @NonExternalExtensionDefs to i8*)]

declare i8* @llvm.cj.vfe.info(i8*, metadata, i64)

define internal void @"_CN7default1PIJ2X_E7func_JHv"(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
  ret void
}

define internal void @"_CN7default1QK_E7func_KHv"(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
  ret void
}

; Call site on interface K: objType !1 = !{!"K.ti"} -> Path [K_TI]. Reaches Q.
define void @use_K(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %v, %TypeInfo* %ti.T) gc "cangjie" {
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
attributes #3 = { "CFileKlass" "NotModifiableClass" }

; arity-2 flat multi-operand encoding: operand(0) = template/leaf label,
; operand(1..] = type arguments (siblings). Decoder DFS-flattens to
; Path [J_TI, Int32_TI, Int64_TI].
!0 = !{!"default:J.ti", !"Int32.ti", !"Int64.ti"}
; arity-0 legacy encoding: single-operand node -> Path [K_TI]. Must still work.
!1 = !{!"default:K.ti"}
