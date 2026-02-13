; RUN: opt -enable-new-pm=0 -global-merge-func -S %s | FileCheck %s
; RUN: opt -passes=global-merge-func -S %s | FileCheck %s

declare i32 @callee(i32)

@g1 = internal global i32 3, align 4
@g2 = internal global i32 7, align 4

define internal i32 @musttail_a(i32 %x) {
entry:
  %c = load i32, i32* @g1, align 4
  %sum = add i32 %x, %c
  %r = musttail call i32 @callee(i32 %sum)
  ret i32 %r
}

define internal i32 @musttail_b(i32 %x) {
entry:
  %c = load i32, i32* @g2, align 4
  %sum = add i32 %x, %c
  %r = musttail call i32 @callee(i32 %sum)
  ret i32 %r
}

; CHECK-NOT: .Tgm
; CHECK: define internal i32 @musttail_a(i32 %x)
; CHECK: musttail call i32 @callee(i32 %sum)
; CHECK: define internal i32 @musttail_b(i32 %x)
; CHECK: musttail call i32 @callee(i32 %sum)
