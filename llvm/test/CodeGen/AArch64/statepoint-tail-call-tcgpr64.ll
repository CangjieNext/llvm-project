; RUN: llc -mtriple=arm64-apple-macosx11.0.0 -verify-machineinstrs -stop-after=prologepilog < %s | FileCheck %s --check-prefixes=MIR,MIR-SMALL
; RUN: llc -mtriple=arm64-apple-ios14.0.0 -verify-machineinstrs -stop-after=prologepilog < %s | FileCheck %s --check-prefixes=MIR,MIR-SMALL
; RUN: llc -mtriple=arm64-apple-ios14.0.0 -code-model=large -verify-machineinstrs -stop-after=prologepilog < %s | FileCheck %s --check-prefixes=MIR,MIR-LARGE
; RUN: llc -mtriple=arm64-apple-macosx11.0.0 -verify-machineinstrs < %s | FileCheck %s --check-prefixes=ASM,ASM-SMALL
; RUN: llc -mtriple=arm64-apple-ios14.0.0 -verify-machineinstrs < %s | FileCheck %s --check-prefixes=ASM,ASM-SMALL
; RUN: llc -mtriple=arm64-apple-ios14.0.0 -code-model=large -verify-machineinstrs < %s | FileCheck %s --check-prefixes=ASM,ASM-LARGE

; STATEPOINT_TAIL_CALL lowers an indirect call target to a register jump, and
; PEI inserts the callee-saved restores (x19-x28, fp, lr) right before the
; statepoint. If the tail-jump target lived in a callee-saved register, the
; epilogue would restore it to the caller's value before the jump. The target
; operand must therefore be constrained to tcGPR64 (x0-x18; x18 is reserved
; on Darwin), or to rtcGPR64 (x16/x17 only) under branch target enforcement.
;
; The {{\$x(1[0-7]|[0-9])}} patterns below accept only tcGPR64 registers at
; the target positions: the trailing ", " or end-of-line anchors make the
; alternation reject two-digit callee-saved registers such as $x19.

target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx11.0.0"

declare void @intervening()
declare void @ext_callee()

declare token @llvm.experimental.gc.statepoint.p0f_isVoidf(i64, i32, void ()*, i32, i32, ...)

; Indirect tail statepoint: the callee pointer is live across an intervening
; call, so an unconstrained register allocator would keep it in a callee-saved
; register. With the tcGPR64 constraint the callee pointer is spilled across
; the call and reloaded into a tcGPR64 register just before the tail jump,
; after the epilogue has restored the callee-saved registers.
define void @test_indirect_tail_statepoint(void ()* %fp) gc "statepoint-example" {
entry:
; MIR-LABEL: name: test_indirect_tail_statepoint
; MIR: STRXui $x0, $sp
; MIR: renamable {{\$x(1[0-7]|[0-9])}} = LDRXui $sp
; MIR: frame-destroy LDPX
; MIR: STATEPOINT_TAIL_CALL 0, 0, 0, killed renamable {{\$x(1[0-7]|[0-9])}},
; ASM-LABEL: test_indirect_tail_statepoint:
; ASM: str x0, [sp
; ASM: ldp x29, x30, [sp, #16]
; ASM: ldr {{x(1[0-7]|[0-9])}}, [sp
; ASM: br {{x(1[0-7]|[0-9])$}}
  call void @intervening()
  tail call token (i64, i32, void ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidf(i64 0, i32 0, void ()* elementtype(void ()) %fp, i32 0, i32 0, i32 0, i32 0)
  ret void
}

; Direct (symbol) tail statepoints use a pc-relative branch under the small
; code model and need no target register.
;
; Under -code-model=large, MachO references to external functions are loaded
; from the GOT into a register, so even a direct callee lowers to br xN. The
; GOT load result must also land in a tcGPR64 register, and the callee-saved
; restores must happen before the tail jump.
define void @test_direct_tail_statepoint() gc "statepoint-example" {
entry:
; MIR-LABEL: name: test_direct_tail_statepoint
; MIR-SMALL: STATEPOINT_TAIL_CALL 0, 0, 0, @ext_callee,
; MIR-LARGE: renamable {{\$x(1[0-7]|[0-9])}} = LOADgot target-flags(aarch64-got) @ext_callee
; MIR-LARGE: frame-destroy LDPXpost
; MIR-LARGE: STATEPOINT_TAIL_CALL 0, 0, 0, killed renamable {{\$x(1[0-7]|[0-9])}},
; ASM-LABEL: test_direct_tail_statepoint:
; ASM-SMALL: stp x29, x30, [sp, #-16]!
; ASM-SMALL: ldp x29, x30, [sp], #16
; ASM-SMALL: b _ext_callee
; ASM-LARGE: adrp {{x(1[0-7]|[0-9])}}, _ext_callee@GOTPAGE
; ASM-LARGE: ldr {{x(1[0-7]|[0-9])}}, [{{x(1[0-7]|[0-9])}}, _ext_callee@GOTPAGEOFF]
; ASM-LARGE: ldp x29, x30, [sp], #16
; ASM-LARGE: br {{x(1[0-7]|[0-9])$}}
  tail call token (i64, i32, void ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidf(i64 0, i32 0, void ()* elementtype(void ()) @ext_callee, i32 0, i32 0, i32 0, i32 0)
  ret void
}

; Under branch target enforcement an indirect branch may only land on a
; "bti c" landing pad through x16 or x17, so the tail-jump target must be
; constrained to rtcGPR64 instead of tcGPR64.
define void @test_bti_indirect_tail_statepoint(void ()* %fp) "branch-target-enforcement"="true" gc "statepoint-example" {
entry:
; MIR-LABEL: name: test_bti_indirect_tail_statepoint
; MIR: STATEPOINT_TAIL_CALL 0, 0, 0, killed renamable {{\$x1[67]}},
; ASM-LABEL: test_bti_indirect_tail_statepoint:
; ASM: hint #34
; ASM: ldr {{x1[67]}}, [sp
; ASM: br {{x1[67]$}}
  call void @intervening()
  tail call token (i64, i32, void ()*, i32, i32, ...) @llvm.experimental.gc.statepoint.p0f_isVoidf(i64 0, i32 0, void ()* elementtype(void ()) %fp, i32 0, i32 0, i32 0, i32 0)
  ret void
}
