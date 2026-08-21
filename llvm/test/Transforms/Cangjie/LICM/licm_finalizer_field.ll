; RUN: opt < %s -licm --cangjie-pipeline -S | FileCheck %s

; A finalizer object's field load must NOT be hoisted (or sunk) out of a loop.
; Hoisting removes the only SSA use of the finalizer object inside the loop,
; so GC liveness reports it dead and the GC runs its ~init() while the source
; program still references it.

%KlassInfo.0 = type { [0 x %KlassInfo.0*] }
@_ZN7default1AE.objKlass = weak_odr global %KlassInfo.0 { [0 x %KlassInfo.0*] zeroinitializer }

declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32)
declare void @use_obj(i8 addrspace(1)*)

; CHECK-LABEL: define void @finalizer_field_not_hoisted
; The load of the finalizer object field must stay inside the loop body.
; CHECK: br label %loop
; CHECK-NOT: %[[L:.*]] = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*
; CHECK: loop:
; CHECK: %[[L:.*]] = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*
; CHECK: call void @use_obj(i8 addrspace(1)* %[[L]])
define void @finalizer_field_not_hoisted() gc "cangjie" {
entry:
  %0 = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%KlassInfo.0* @_ZN7default1AE.objKlass to i8*), i32 16)
  %1 = getelementptr i8, i8 addrspace(1)* %0, i64 8
  %2 = bitcast i8 addrspace(1)* %1 to i8 addrspace(1)* addrspace(1)*
  br label %loop

loop:
  %3 = phi i64 [ 0, %entry ], [ %5, %loop ]
  %4 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %2, align 8
  call void @use_obj(i8 addrspace(1)* %4)
  %5 = add i64 %3, 1
  %6 = icmp slt i64 %5, 10
  br i1 %6, label %loop, label %exit

exit:
  ret void
}
