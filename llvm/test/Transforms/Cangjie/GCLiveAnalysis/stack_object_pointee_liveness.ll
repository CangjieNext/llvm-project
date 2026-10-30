; RUN: opt < %s -cj-rewrite-statepoint -S 2>&1 | FileCheck %s
; RUN: opt < %s -passes=cj-rewrite-statepoint -S 2>&1 | FileCheck %s

%LocalBox = type { i8 addrspace(1)* }
%Env = type { i8 addrspace(1)*, i64 }

declare void @g() #0 gc "cangjie"
declare void @use_env(i8*) #0

; LocalBox is captured only after the safepoint.  Its value field is already
; live, so LocalBox must be struct-live at the earlier safepoint as well;
; otherwise a GC there can move the heap object while LocalBox keeps a stale
; reference.
define void @safepoint_before_capture(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  call void @g()
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  ret void
}

; CHECK-LABEL: define void @safepoint_before_capture
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK: %LocalBox* %box

declare void @drop_arg(i8 addrspace(1)*) gc "cangjie"

; A Cangjie-to-Cangjie call is tail-call eligible.  The callee has its own
; safepoints for its incoming arguments, so the call argument must not be added
; to gc-live here.
define void @cangjie_call_argument_is_live(i8 addrspace(1)* %obj) gc "cangjie" {
entry:
  call void @drop_arg(i8 addrspace(1)* %obj)
  ret void
}

; CHECK-LABEL: define void @cangjie_call_argument_is_live
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@drop_arg{{.*}}i8 addrspace(1)* %obj)
; CHECK-NOT: "gc-live"(i8 addrspace(1)* %obj)

attributes #0 = { "record_mut" }
