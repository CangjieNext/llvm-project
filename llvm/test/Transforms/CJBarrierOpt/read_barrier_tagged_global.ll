; RUN: opt -enable-new-pm=false --cj-barrier-opt -S < %s | FileCheck %s
; RUN: opt -passes=cj-barrier-opt -S < %s | FileCheck %s

%record = type { i8 addrspace(1)* }

@Global0 = internal global %record zeroinitializer

declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

define i8 addrspace(1)* @foo() gc "cangjie" {
entry:
  %0 = call i8 addrspace(1)* @llvm.cj.gcread.ref(
    i8 addrspace(1)* null,
    i8 addrspace(1)* addrspace(1)* getelementptr (
      %record,
      %record addrspace(1)* inttoptr (
        i64 or (
          i64 ptrtoint (%record addrspace(1)* addrspacecast (%record* @Global0 to %record addrspace(1)*) to i64),
          i64 -9223372036854775808
        ) to %record addrspace(1)*
      ),
      i64 0,
      i32 0
    )
  )
  ret i8 addrspace(1)* %0
}

; CHECK-LABEL: @foo(
; CHECK: call i8 addrspace(1)* @llvm.cj.gcread.ref
; CHECK-NOT: load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*
