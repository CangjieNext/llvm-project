; RUN: opt -enable-new-pm=0 -global-merge-func -S %s | FileCheck %s
; RUN: opt -passes=global-merge-func -S %s | FileCheck %s

@g1 = internal global i32 3, align 4
@g2 = internal global i32 7, align 4

define i32 @ext_a(i32* %p) {
entry:
  %c = load i32, i32* @g1, align 4
  %v = load i32, i32* %p, align 4
  %sum = add i32 %v, %c
  ret i32 %sum
}

define i32 @ext_b(i32* %p) {
entry:
  %c = load i32, i32* @g2, align 4
  %v = load i32, i32* %p, align 4
  %sum = add i32 %v, %c
  ret i32 %sum
}

; CHECK-NOT: .Tgm
; CHECK: define i32 @ext_a(i32* %p)
; CHECK: define i32 @ext_b(i32* %p)
