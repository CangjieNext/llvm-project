; RUN: opt --cangjie-pipeline -passes=cj-specific-opt -S < %s | FileCheck %s
;
; On non-ELF, non-Windows platforms (Mach-O) the historical
; generateAliasForStaticGI path is preserved: operands of the
; StaticGenericTI array are replaced by private aliases, so the 4-byte
; relative offsets in the __cjstatic_gi table stay link-time constants.

target triple = "arm64-apple-macosx13"

@X.ti = linkonce_odr global i32 0
@Y.ti = external global i32

@StaticGenericTIs = private global [2 x ptr] [ptr @X.ti, ptr @Y.ti] #0
@llvm.used = appending global [1 x ptr] [ptr @StaticGenericTIs], section "llvm.metadata"

attributes #0 = { "CFileStaticGenericTI" }

; weak/linkonce operand gets a private alias; external declaration is kept
; as-is in the array (regression #553: element must not be dropped).
; CHECK: @StaticGenericTIs = private global [2 x ptr] [ptr @alias_X.ti, ptr @Y.ti]
; CHECK: @alias_X.ti = private alias i32, ptr @X.ti
; CHECK-NOT: @alias_Y.ti
