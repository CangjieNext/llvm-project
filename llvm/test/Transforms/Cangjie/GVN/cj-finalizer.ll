; RUN: opt -gvn --cangjie-pipeline -S < %s | FileCheck %s

%KlassInfo.0 = type { [0 x %KlassInfo.0*] }
@_ZN7default1AE.objKlass = weak_odr global %KlassInfo.0 { [0 x %KlassInfo.0*] zeroinitializer }
%Unit.Type = type { i8 }

declare i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8*, i32)
declare %Unit.Type @CJ_MCC_InvokeGC(i1)

define i64 @foo() {
entry:
  %0 = call noalias i8 addrspace(1)* @CJ_MCC_NewFinalizer(i8* bitcast (%KlassInfo.0* @_ZN7default1AE.objKlass to i8*), i32 16)
  %1 = getelementptr inbounds i8, i8 addrspace(1)* %0
  %2 = bitcast i8 addrspace(1)* %1 to i64 addrspace(1)*
  store i64 0, i64 addrspace(1)* %2, align 8
  %3 = tail call %Unit.Type @CJ_MCC_InvokeGC(i1 false)
  %4 = load i64, i64 addrspace(1)* %2, align 8
  ; CHECK-NOT: ret i64 0
  ret i64 %4
}