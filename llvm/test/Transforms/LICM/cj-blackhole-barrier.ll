; RUN: opt < %s -passes=licm -S | FileCheck %s

; llvm.cj.blackhole has default (may-read/may-write) memory effects, so a
; loop-invariant load must NOT be hoisted above it. This is what makes
; std.runtime.blackBox effective against LICM in @Bench measurement loops.

declare i8* @llvm.cj.blackhole(i8*)

define i64 @blackhole_blocks_licm(i64* %p, i64 %n) {
entry:
  br label %loop

; CHECK-LABEL: @blackhole_blocks_licm(
; CHECK: loop:
; CHECK-NEXT: %i = phi
; CHECK-NEXT: %v = load i64, i64* %p
loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %loop ]
  %v = load i64, i64* %p, align 8
  %p8 = bitcast i64* %p to i8*
; CHECK-NEXT: call i8* @llvm.cj.blackhole
  %bh = call i8* @llvm.cj.blackhole(i8* %p8)
  %inc = add i64 %i, 1
  %cmp = icmp ult i64 %inc, %n
  br i1 %cmp, label %loop, label %exit

exit:
  %r = add i64 %v, %i
  ret i64 %r
}

; Contrast case: without the blackhole call the same load leaves the loop
; (sunk to the exit block, executed only once).
define i64 @no_blackhole_load_leaves_loop(i64* dereferenceable(8) %p, i64 %n) {
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %loop ]
  %v = load i64, i64* %p, align 8
  %inc = add i64 %i, 1
  %cmp = icmp ult i64 %inc, %n
  br i1 %cmp, label %loop, label %exit

; CHECK-LABEL: @no_blackhole_load_leaves_loop(
; CHECK: exit:
; CHECK-NEXT: %i.lcssa = phi
; CHECK-NEXT: load i64, i64* %p
exit:
  %r = add i64 %v, %i
  ret i64 %r
}
