; RUN: opt -passes=cj-exception-tail-outline -S < %s | FileCheck %s
;
; Everything that is not exactly the canonical throw tail is left alone: a
; throw of an object that was not allocated here has no tail to share, and a
; tail that is not self contained cannot be erased safely.

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, i8*, i8*, i8* }
%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }

@TI = external global %TypeInfo

declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32)
declare void @CJ_MCC_ThrowException(i8 addrspace(1)*)
declare void @_CExCtor(i8 addrspace(1)*, %TypeInfo*)
declare void @_CExCtorMsg(i8 addrspace(1)*, %"record.std.core:String"*, %TypeInfo*)
declare void @_CNat6Ctor0(i8 addrspace(1)*, %TypeInfo*)

; The constructor never reads the type info, so the frontend passes poison,
; and the allocation does not carry a usable one either. There is nothing left
; to give the helper, which dereferences the type info to read the object size.
; CHECK-LABEL: define void @poison_ti(
; CHECK:         %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* poison, i32 32)
; CHECK-NEXT:    call void @_CNat6Ctor0(i8 addrspace(1)* %obj, %TypeInfo* poison)
define void @poison_ti(i1 %c) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* poison, i32 32)
  call void @_CNat6Ctor0(i8 addrspace(1)* %obj, %TypeInfo* poison)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; The allocation's type info operand is not a type info, so it cannot stand in
; for the poisoned one.
; CHECK-LABEL: define void @poison_ti_raw_alloc(
; CHECK:         %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %raw, i32 32)
; CHECK-NEXT:    call void @_CNat6Ctor0(i8 addrspace(1)* %obj, %TypeInfo* poison)
define void @poison_ti_raw_alloc(i1 %c, i8* %raw) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %raw, i32 32)
  call void @_CNat6Ctor0(i8 addrspace(1)* %obj, %TypeInfo* poison)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; The factory result has a second user, so the call cannot be erased.
; CHECK-LABEL: define void @factory_extra_use(
; CHECK:         call void @_CNat6Sink(i8 addrspace(1)* %obj)
define void @factory_extra_use(i1 %c, %"record.std.core:String"* %msg) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @"rt$CreateOverflowException_msg"(%"record.std.core:String"* %msg)
  call void @_CNat6Sink(i8 addrspace(1)* %obj)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; A runtime factory that is not an exception factory: its result is not thrown
; as an object, so it is left alone.
; CHECK-LABEL: define void @not_an_exception_factory(
; CHECK:         %obj = call i8 addrspace(1)* @"rt$CreateSomething"
define void @not_an_exception_factory(i1 %c, %"record.std.core:String"* %msg) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @"rt$CreateSomething"(%"record.std.core:String"* %msg)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

; No NewObject in front of the throw: nothing to move out.
; CHECK-LABEL: define void @plain_throw(
; CHECK:         call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
define void @plain_throw(i8 addrspace(1)* %obj) gc "cangjie" {
entry:
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
}

; The allocation has a user outside the ctor/throw pair, so erasing it would
; break that user.
; CHECK-LABEL: define void @extra_use(
; CHECK:         call i8* @_CUses(i8 addrspace(1)* %obj)
define void @extra_use(i1 %c) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @TI to i8*), i32 32)
  call void @_CExCtor(i8 addrspace(1)* %obj, %TypeInfo* @TI)
  %keep = call i8* @_CUses(i8 addrspace(1)* %obj)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

declare i8* @_CUses(i8 addrspace(1)*)

; The throw is not the terminator of the cold path.
; CHECK-LABEL: define void @no_unreachable(
; CHECK:         call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
; CHECK-NEXT:    ret void
define void @no_unreachable(i1 %c) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @TI to i8*), i32 32)
  call void @_CExCtor(i8 addrspace(1)* %obj, %TypeInfo* @TI)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  ret void
r:
  ret void
}

; The trailing operand of the ctor is not a TypeInfo, so the helper ABI would
; not match; `Ex(obj, msg)` is the shape that hits this.
; CHECK-LABEL: define void @not_typeinfo(
; CHECK:         call void @_CMsgCtor(i8 addrspace(1)* %obj, i8* %msg)
define void @not_typeinfo(i1 %c, i8* %msg) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @TI to i8*), i32 32)
  call void @_CMsgCtor(i8 addrspace(1)* %obj, i8* %msg)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

declare void @_CMsgCtor(i8 addrspace(1)*, i8*)

declare void @_CNat6Sink(i8 addrspace(1)*)

declare i8 addrspace(1)* @"rt$CreateOverflowException_msg"(%"record.std.core:String"*)
declare i8 addrspace(1)* @"rt$CreateSomething"(%"record.std.core:String"*)

; Three arguments, but the middle one is not a `record.std.core:String`.
; CHECK-LABEL: define void @not_string(
; CHECK:         call void @_CMsgCtor2(i8 addrspace(1)* %obj, i8* %msg, %TypeInfo* @TI)
define void @not_string(i1 %c, i8* %msg) gc "cangjie" {
entry:
  br i1 %c, label %t, label %r
t:
  %obj = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @TI to i8*), i32 32)
  call void @_CMsgCtor2(i8 addrspace(1)* %obj, i8* %msg, %TypeInfo* @TI)
  call void @CJ_MCC_ThrowException(i8 addrspace(1)* %obj)
  unreachable
r:
  ret void
}

declare void @_CMsgCtor2(i8 addrspace(1)*, i8*, %TypeInfo*)

; CHECK-NOT: define internal void @"__cj$exception_tail_helper
