; RUN: opt < %s -passes=tailcallelim -verify-dom-info -S | FileCheck %s
; RUN: opt < %s -passes=tailcallelim -verify-dom-info --cangjie-pipeline -S | FileCheck %s

; CHECK: tail call void @callee0()
; CHECK: notail call void @callee1()
; CHECK: notail call void @callee2()
; CHECK-NEXT: ret void

define void @foo1(i32 %a) {
entry:
  %tobool = icmp eq i32 %a, 0
  br i1 %tobool, label %if.else, label %if.then

if.then:
  call void @callee0()
  br label %if.end

if.else:
  notail call void @callee1()
  br label %if.end

if.end:
  ret void
}

; A notail call immediately followed by ret is still a tail-call candidate
; shape for TCE (e.g. the CJ_MCC_EndLocalRegion emitted before a function
; return); TCE must keep the notail attribute and must not mark it as tail.
define void @foo2() {
entry:
  notail call void @callee2()
  ret void
}

declare void @callee0()
declare void @callee1()
declare void @callee2()
