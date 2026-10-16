; RUN: opt -passes=cj-exception-tail-outline -S < %s | FileCheck %s
;
; The runtime builds some exceptions in a factory that returns the object
; instead of throwing it:
;   %obj = call @rt$Create...Exception_msg(msg)
;   call @CJ_MCC_ThrowException(%obj)
;   unreachable
; There is no NewObject/ctor pair to share here, but the throw of the returned
; object is a cold tail of its own. One helper per factory signature is shared
; by every site, so the two factories below use the same one.

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, i8*, i8*, i8* }
%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }

declare void @CJ_MCC_ThrowException(i8 addrspace(1)*)
declare i8 addrspace(1)* @"rt$CreateOverflowException_msg"(%"record.std.core:String"*)
declare i8 addrspace(1)* @"rt$CreateArithmeticException_msg"(%"record.std.core:String"*)

; CHECK-LABEL: define void @factoryOverflow(
; CHECK:         call void @"__cj$exception_tail_helper_factory"(%"record.std.core:String"* %msg, i8 addrspace(1)* (%"record.std.core:String"*)* @"rt$CreateOverflowException_msg") #[[CALL:[0-9]+]]
; CHECK-NEXT:    unreachable
define void @factoryOverflow(i1 %c, %"record.std.core:String"* %msg) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @"rt$CreateOverflowException_msg"(%"record.std.core:String"* %msg)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; A different factory with the same signature lands on the same helper.
; CHECK-LABEL: define void @factoryArithmetic(
; CHECK:         call void @"__cj$exception_tail_helper_factory"(%"record.std.core:String"* %msg, i8 addrspace(1)* (%"record.std.core:String"*)* @"rt$CreateArithmeticException_msg") #[[CALL]]
; CHECK-NEXT:    unreachable
define void @factoryArithmetic(i1 %c, %"record.std.core:String"* %msg) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @"rt$CreateArithmeticException_msg"(%"record.std.core:String"* %msg)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; CHECK:       define internal void @"__cj$exception_tail_helper_factory"(%"record.std.core:String"* %0, i8 addrspace(1)* (%"record.std.core:String"*)* %factory) #[[HELPER:[0-9]+]] gc "cangjie"
; CHECK:         %obj = call i8 addrspace(1)* %factory(%"record.std.core:String"* %0)
; CHECK-NEXT:    call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
; CHECK-NEXT:    unreachable
; CHECK:       attributes #[[HELPER]] = { cold minsize noinline noreturn optsize }
; CHECK:       attributes #[[CALL]] = { noreturn }
