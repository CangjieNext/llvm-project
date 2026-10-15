; RUN: opt -passes=cj-exception-tail-outline -S < %s | FileCheck %s
;
; Both of these raise an error on a path where the stack is already exhausted,
; or an implicit check failed, so they cannot afford the extra frame an
; outlined tail adds. MergeFunctions' exception outline skips them for the same
; reason. @control has exactly the same body and is outlined, so the test also
; shows the guard is what keeps the two runtime functions untouched.

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, i8*, i8*, i8* }

@TI = external global %TypeInfo

declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32)
declare void @CJ_MCC_ThrowException(i8 addrspace(1)*)
declare void @_CExCtor(i8 addrspace(1)*, %TypeInfo*)

; CHECK-LABEL: define void @"rt$ThrowStackOverflowError"(
; CHECK:         %obj = call i8 addrspace(1)* @CJ_MCC_NewObject
; CHECK-NEXT:    call void @_CExCtor(i8 addrspace(1)* %obj, %TypeInfo* @TI)
; CHECK-NEXT:    call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
; CHECK-NEXT:    unreachable
define void @"rt$ThrowStackOverflowError"(i1 %c) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @TI to i8*), i32 88)
  call void @_CExCtor(i8 addrspace(1)* %obj, %TypeInfo* @TI)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; CHECK-LABEL: define void @"rt$ThrowImplicitException"(
; CHECK:         %obj = call i8 addrspace(1)* @CJ_MCC_NewObject
; CHECK-NEXT:    call void @_CExCtor(i8 addrspace(1)* %obj, %TypeInfo* @TI)
; CHECK-NEXT:    call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
; CHECK-NEXT:    unreachable
define void @"rt$ThrowImplicitException"(i1 %c) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @TI to i8*), i32 56)
  call void @_CExCtor(i8 addrspace(1)* %obj, %TypeInfo* @TI)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; CHECK-LABEL: define void @control(
; CHECK:         call void @__cj_exception_tail_helper0(%TypeInfo* @TI, void (i8 addrspace(1)*, %TypeInfo*)* @_CExCtor)
; CHECK-NEXT:    unreachable
define void @control(i1 %c) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @TI to i8*), i32 56)
  call void @_CExCtor(i8 addrspace(1)* %obj, %TypeInfo* @TI)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}
