; RUN: opt < %s -licm --cangjie-pipeline -S | FileCheck %s

define i32 @hoist_unmarked_i32(i32* %p, i1 %flag, i64 %n) {
; CHECK-LABEL: define i32 @hoist_unmarked_i32(
; CHECK: entry:
; CHECK-NEXT: [[V:%.*]] = load i32, i32* %p, align 4
; CHECK-NEXT: br label %loop
; CHECK: load:
; CHECK-NEXT: br label %cont
entry:
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %cont ]
  %acc = phi i32 [ 0, %entry ], [ %next, %cont ]
  br i1 %flag, label %load, label %cont

load:
  %v = load i32, i32* %p, align 4
  br label %cont

cont:
  %next = phi i32 [ %v, %load ], [ %acc, %loop ]
  %inc = add i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i32 %next
}

define i32 @do_not_hoist_unsafe_i32(i32* %p, i1 %flag, i64 %n) {
; CHECK-LABEL: define i32 @do_not_hoist_unsafe_i32(
; CHECK: entry:
; CHECK-NEXT: [[RAW:%.*]] = bitcast i32* %p to i8*
; CHECK-NEXT: [[UNSAFE_I8:%.*]] = call i8* @llvm.cj.unsafe.pointer.p0i8(i8* [[RAW]])
; CHECK-NEXT: [[UNSAFE:%.*]] = bitcast i8* [[UNSAFE_I8]] to i32*
; CHECK-NEXT: br label %loop
; CHECK: load:
; CHECK-NEXT: [[V:%.*]] = load i32, i32* [[UNSAFE]], align 4
; CHECK-NEXT: br label %cont
entry:
  %raw = bitcast i32* %p to i8*
  %unsafe.i8 = call i8* @llvm.cj.unsafe.pointer.p0i8(i8* %raw)
  %unsafe = bitcast i8* %unsafe.i8 to i32*
  br label %loop

loop:
  %i = phi i64 [ 0, %entry ], [ %inc, %cont ]
  %acc = phi i32 [ 0, %entry ], [ %next, %cont ]
  br i1 %flag, label %load, label %cont

load:
  %v = load i32, i32* %unsafe, align 4
  br label %cont

cont:
  %next = phi i32 [ %v, %load ], [ %acc, %loop ]
  %inc = add i64 %i, 1
  %done = icmp eq i64 %inc, %n
  br i1 %done, label %exit, label %loop

exit:
  ret i32 %next
}

declare i8* @llvm.cj.unsafe.pointer.p0i8(i8*)
