; RUN: opt < %s -cj-rewrite-statepoint -S 2>&1 | FileCheck %s
; RUN: opt < %s -passes=cj-rewrite-statepoint -S 2>&1 | FileCheck %s
; RUN: opt -enable-new-pm=0 -O2 -cj-rewrite-statepoint -S < %s 2>&1 | FileCheck %s
; RUN: opt < %s -cj-rewrite-statepoint -large-struct-size-threshold=1 -S 2>&1 | FileCheck %s

%LocalBox = type { i8 addrspace(1)* }
%Env = type { i8 addrspace(1)*, i64 }

declare void @g() #0 gc "cangjie"
declare void @use_env(i8*) #0
declare void @use_copy_dst(%CopyDst*) #0
declare void @use_ref(i8 addrspace(1)*) #0
declare void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture)
declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.gcwrite.struct.p0i8.i64(i8 addrspace(1)*, i8 addrspace(1)*, i8*, i64)
declare void @llvm.cj.copy.struct.field.p0i8.i64(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.memset.p0i8(i8*, i8, i64, i1)

%CopySrc = type { i8 addrspace(1)*, i8 addrspace(1)* }
%CopyDst = type { i8 addrspace(1)*, i8 addrspace(1)* }

; The LocalBox address is stored into a field of Env after the safepoint in
; the loop body.  The store does not dominate the safepoint, but the stored
; value reaches it on the next iteration.  The runtime cannot follow the
; stack address held by Env, so LocalBox has to be struct-live as well.
define void @loop_carried_stack_pointee(i8 addrspace(1)* %heap, i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  br label %loop

loop:
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-LABEL: define void @loop_carried_stack_pointee
; CHECK: loop:
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; Cangjie emits many lifetime.start markers with a zero size operand.  The
; inserted initialization must use the alloca size, not the marker size.
define void @zero_size_lifetime(i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %box.i8 = bitcast %LocalBox* %box to i8*
  call void @llvm.lifetime.start.p0i8(i64 0, i8* %box.i8)
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  br label %loop

loop:
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %box.value, align 8
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-LABEL: define void @zero_size_lifetime
; CHECK: %box = alloca %LocalBox
; CHECK: %[[BOXPTR:.*]] = bitcast %LocalBox* %box to i8*
; CHECK: call void @llvm.lifetime.start.p0i8(i64 0, i8* {{(nonnull )?}}%[[BOXPTR]])
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 %[[BOXPTR]], i8 0, i64 8, i1 false)
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; Each lifetime region needs its own zero initialization.
define void @multiple_lifetimes(i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %box.i8 = bitcast %LocalBox* %box to i8*
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  call void @llvm.lifetime.start.p0i8(i64 0, i8* %box.i8)
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  call void @llvm.lifetime.start.p0i8(i64 0, i8* %box.i8)
  br label %loop

loop:
  call void @g()
  call void @use_env(i8* %env.i8)
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %box.value, align 8
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-LABEL: define void @multiple_lifetimes
; CHECK: %[[BOXPTR2:.*]] = bitcast %LocalBox* %box to i8*
; CHECK: call void @llvm.lifetime.start.p0i8(i64 0, i8* {{(nonnull )?}}%[[BOXPTR2]])
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 %[[BOXPTR2]], i8 0, i64 8, i1 false)
; CHECK: call void @llvm.lifetime.start.p0i8(i64 0, i8* {{(nonnull )?}}%[[BOXPTR2]])
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 %[[BOXPTR2]], i8 0, i64 8, i1 false)

; Cangjie normally lowers a reference store to llvm.cj.gcwrite.ref.  That form
; must establish the same stack-field -> stack-object relationship as a plain
; store.
define void @gcwrite_stack_pointee(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %env.as1 = addrspacecast %Env* %env to i8 addrspace(1)*
  %env.field.as1 = getelementptr i8, i8 addrspace(1)* %env.as1, i64 0
  %env.field = bitcast i8 addrspace(1)* %env.field.as1 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %box.as1, i8 addrspace(1)* %env.as1, i8 addrspace(1)* addrspace(1)* %env.field)
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  ret void
}

; CHECK-LABEL: define void @gcwrite_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; llvm.cj.memset is also a definition of the destination field and must kill
; an earlier pointer stored there.
define void @cj_memset_kills_stack_pointee(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  %env.i8 = bitcast %Env* %env to i8*
  call void @llvm.cj.memset.p0i8(i8* %env.i8, i8 0, i64 8, i1 false)
  call void @g()
  call void @use_env(i8* %env.i8)
  ret void
}

; CHECK-LABEL: define void @cj_memset_kills_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box

; A whole-struct copy can put the stack address into the destination just like
; a reference store.  The copy has no SSA value for the stored address, so the
; source field has to be resolved as the copy's reaching definition.
define void @gcwrite_struct_stack_pointee(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %src = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %src.field = getelementptr inbounds %Env, %Env* %src, i64 0, i32 0
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %src.field, align 8
  %src.i8 = bitcast %Env* %src to i8*
  %env.as1 = addrspacecast %Env* %env to i8 addrspace(1)*
  call void @llvm.cj.gcwrite.struct.p0i8.i64(i8 addrspace(1)* %env.as1, i8 addrspace(1)* %env.as1, i8* %src.i8, i64 8)
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  ret void
}

; CHECK-LABEL: define void @gcwrite_struct_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

attributes #0 = { "record_mut" }

; A store on one branch does not dominate the safepoint at the join, but it
; reaches it on that branch, so LocalBox is still live there.
define void @branch_stack_pointee(i8 addrspace(1)* %heap, i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  br i1 %cond, label %capture, label %join

capture:
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  br label %join

join:
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  ret void
}

; CHECK-LABEL: define void @branch_stack_pointee
; CHECK: join:
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; A store in a block that cannot reach the safepoint must not make LocalBox
; live there.
define void @unreachable_stack_pointee(i8 addrspace(1)* %heap, i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  br i1 %cond, label %dead, label %live

dead:
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  br label %exit

live:
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  br label %exit

exit:
  ret void
}

; CHECK-LABEL: define void @unreachable_stack_pointee
; CHECK: live:
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box

; A store killed by a later store to the same field does not keep LocalBox
; live at the safepoint.
define void @killed_stack_pointee(i8 addrspace(1)* %heap, i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  br i1 %cond, label %capture, label %join

capture:
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  br label %join

join:
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  ret void
}

; CHECK-LABEL: define void @killed_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box

; A kill in a successor block also stops the store from reaching the
; safepoint.
define void @killed_in_successor_stack_pointee(i8 addrspace(1)* %heap, i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  br i1 %cond, label %capture, label %join

capture:
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  br label %kill

kill:
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  br label %join

join:
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  ret void
}

; CHECK-LABEL: define void @killed_in_successor_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%Env* %env
; CHECK-NOT: %LocalBox* %box

; On the first loop iteration the pointee is scanned before the loop-carried
; store executes.  It must therefore be zero-initialized.
define void @loop_carried_uninit_stack_pointee(i1 %cond) #0 gc "cangjie" {
entry:
  %env = alloca %Env, align 8
  %box = alloca %LocalBox, align 8
  %env.field = getelementptr inbounds %Env, %Env* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.field, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  br label %loop

loop:
  call void @g()
  %env.i8 = bitcast %Env* %env to i8*
  call void @use_env(i8* %env.i8)
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %box.value, align 8
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.field, align 8
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-LABEL: define void @loop_carried_uninit_stack_pointee
; CHECK: entry:
; CHECK: %box = alloca %LocalBox
; CHECK: call void @llvm.memset.p0i8.i64({{.*}}i8 0, i64 8, i1 false)
; CHECK: loop:
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_env{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; The copied source range starts at field 1, not at the source object base.  The
; destination field 0 maps to the source pointer's own offset 0, not to source
; offset 8 again, so the LocalBox stored in source field 1 must stay live.
define void @range_copy_nonzero_source_offset(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %src = alloca %CopySrc, align 8
  %dst = alloca %CopyDst, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %src.f0 = getelementptr inbounds %CopySrc, %CopySrc* %src, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %src.f0, align 8
  %src.f1 = getelementptr inbounds %CopySrc, %CopySrc* %src, i64 0, i32 1
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %src.f1, align 8
  %dst.f0 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f0, align 8
  %dst.f1 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 1
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f1, align 8
  %src.i8 = bitcast %CopySrc* %src to i8*
  %src.field1.i8 = getelementptr inbounds i8, i8* %src.i8, i64 8
  %dst.as1 = addrspacecast %CopyDst* %dst to i8 addrspace(1)*
  call void @llvm.cj.gcwrite.struct.p0i8.i64(i8 addrspace(1)* %dst.as1, i8 addrspace(1)* %dst.as1, i8* %src.field1.i8, i64 8)
  call void @g()
  call void @use_copy_dst(%CopyDst* %dst)
  ret void
}

; CHECK-LABEL: define void @range_copy_nonzero_source_offset
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; The source field is written through a variable index, so the analysis only
; knows that the source object may hold the stack address somewhere.  A later
; copy from a fixed source range must still preserve that pointee.
define void @range_copy_unknown_source_offset(i8 addrspace(1)* %heap, i64 %idx) #0 gc "cangjie" {
entry:
  %src = alloca %CopySrc, align 8
  %dst = alloca %CopyDst, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %src.i8 = bitcast %CopySrc* %src to i8*
  %src.dynamic = getelementptr inbounds i8, i8* %src.i8, i64 %idx
  %src.dynamic.ref = bitcast i8* %src.dynamic to i8 addrspace(1)**
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %src.dynamic.ref, align 8
  %dst.f0 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f0, align 8
  %dst.f1 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 1
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f1, align 8
  %dst.as1 = addrspacecast %CopyDst* %dst to i8 addrspace(1)*
  call void @llvm.cj.gcwrite.struct.p0i8.i64(i8 addrspace(1)* %dst.as1, i8 addrspace(1)* %dst.as1, i8* %src.i8, i64 8)
  call void @g()
  call void @use_copy_dst(%CopyDst* %dst)
  ret void
}

; CHECK-LABEL: define void @range_copy_unknown_source_offset
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; A range copy after the safepoint publishes its destination on the next loop
; iteration.  Even with an unknown source offset, the pointee must be live at
; the earlier safepoint.
define void @loop_carried_range_copy_unknown_source_offset(i8 addrspace(1)* %heap, i64 %idx, i1 %cond) #0 gc "cangjie" {
entry:
  %src = alloca %CopySrc, align 8
  %dst = alloca %CopyDst, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %src.i8 = bitcast %CopySrc* %src to i8*
  %src.dynamic = getelementptr inbounds i8, i8* %src.i8, i64 %idx
  %src.dynamic.ref = bitcast i8* %src.dynamic to i8 addrspace(1)**
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %src.dynamic.ref, align 8
  %dst.as1 = addrspacecast %CopyDst* %dst to i8 addrspace(1)*
  br label %loop

loop:
  call void @g()
  call void @use_copy_dst(%CopyDst* %dst)
  call void @llvm.cj.gcwrite.struct.p0i8.i64(i8 addrspace(1)* %dst.as1, i8 addrspace(1)* %dst.as1, i8* %src.i8, i64 8)
  br i1 %cond, label %loop, label %exit

exit:
  ret void
}

; CHECK-LABEL: define void @loop_carried_range_copy_unknown_source_offset
; CHECK: loop:
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@use_copy_dst{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; A partial copy into destination field 0 must not keep a stack object live
; merely because a different destination field is live.
define void @range_copy_uncovered_field(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %src = alloca %CopySrc, align 8
  %dst = alloca %CopyDst, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %src.f0 = getelementptr inbounds %CopySrc, %CopySrc* %src, i64 0, i32 0
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %src.f0, align 8
  %src.f1 = getelementptr inbounds %CopySrc, %CopySrc* %src, i64 0, i32 1
  store i8 addrspace(1)* null, i8 addrspace(1)** %src.f1, align 8
  %dst.f0 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f0, align 8
  %dst.f1 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 1
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f1, align 8
  %src.i8 = bitcast %CopySrc* %src to i8*
  %dst.as1 = addrspacecast %CopyDst* %dst to i8 addrspace(1)*
  call void @llvm.cj.gcwrite.struct.p0i8.i64(i8 addrspace(1)* %dst.as1, i8 addrspace(1)* %dst.as1, i8* %src.i8, i64 8)
  %live = load i8 addrspace(1)*, i8 addrspace(1)** %dst.f1, align 8
  call void @g()
  call void @use_ref(i8 addrspace(1)* %live)
  ret void
}

; CHECK-LABEL: define void @range_copy_uncovered_field
; CHECK-NOT: "struct-live"({{.*}}%LocalBox* %box
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}

; A whole-object liveness entry (-1) must still propagate a direct store to a
; field of that object.  The extra run line lowers the large-struct threshold
; to one so this small struct uses the whole-object representation.
%Env2 = type { i8 addrspace(1)*, i8 addrspace(1)* }
declare void @use_env2(%Env2*) #0

define void @whole_object_liveness_direct_store(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %Env2, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %env.f0 = getelementptr inbounds %Env2, %Env2* %env, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %env.f0, align 8
  %env.f1 = getelementptr inbounds %Env2, %Env2* %env, i64 0, i32 1
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %env.f1, align 8
  call void @g()
  call void @use_env2(%Env2* %env)
  ret void
}

; CHECK-LABEL: define void @whole_object_liveness_direct_store
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; An atomic reference write is another way to publish a stack object address.
; The runtime cannot follow that stack address, so the pointee must be live.
%AtomicEnv = type { i8 addrspace(1)* }
declare void @use_atomic_env(%AtomicEnv*) #0
declare void @llvm.cj.atomic.store(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*, i32)
declare i8 addrspace(1)* @llvm.cj.atomic.swap(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*, i32)
declare i1 @llvm.cj.atomic.compare.swap(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*, i32, i32)

define void @atomic_store_stack_pointee(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %AtomicEnv, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %env.as1 = addrspacecast %AtomicEnv* %env to i8 addrspace(1)*
  %env.f0.as1 = getelementptr inbounds %AtomicEnv, %AtomicEnv* %env, i64 0, i32 0
  %env.f0.as11 = addrspacecast i8 addrspace(1)** %env.f0.as1 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.atomic.store(i8 addrspace(1)* %box.as1, i8 addrspace(1)* %env.as1, i8 addrspace(1)* addrspace(1)* %env.f0.as11, i32 5)
  call void @g()
  call void @use_atomic_env(%AtomicEnv* %env)
  ret void
}

; CHECK-LABEL: define void @atomic_store_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

define void @atomic_swap_stack_pointee(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %AtomicEnv, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %env.as1 = addrspacecast %AtomicEnv* %env to i8 addrspace(1)*
  %env.f0.as1 = getelementptr inbounds %AtomicEnv, %AtomicEnv* %env, i64 0, i32 0
  %env.f0.as11 = addrspacecast i8 addrspace(1)** %env.f0.as1 to i8 addrspace(1)* addrspace(1)*
  %old = call i8 addrspace(1)* @llvm.cj.atomic.swap(i8 addrspace(1)* %box.as1, i8 addrspace(1)* %env.as1, i8 addrspace(1)* addrspace(1)* %env.f0.as11, i32 5)
  call void @g()
  call void @use_atomic_env(%AtomicEnv* %env)
  ret void
}

; CHECK-LABEL: define void @atomic_swap_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

define void @atomic_compare_swap_stack_pointee(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %env = alloca %AtomicEnv, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %env.as1 = addrspacecast %AtomicEnv* %env to i8 addrspace(1)*
  %env.f0.as1 = getelementptr inbounds %AtomicEnv, %AtomicEnv* %env, i64 0, i32 0
  %env.f0.as11 = addrspacecast i8 addrspace(1)** %env.f0.as1 to i8 addrspace(1)* addrspace(1)*
  %ok = call i1 @llvm.cj.atomic.compare.swap(i8 addrspace(1)* null, i8 addrspace(1)* %box.as1, i8 addrspace(1)* %env.as1, i8 addrspace(1)* addrspace(1)* %env.f0.as11, i32 5, i32 5)
  call void @g()
  call void @use_atomic_env(%AtomicEnv* %env)
  ret void
}

; CHECK-LABEL: define void @atomic_compare_swap_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})

; cj.copy.struct.field copies a field range just like array_copy.  Its source
; field can contain a stack object address, which the runtime cannot follow.
define void @copy_struct_field_stack_pointee(i8 addrspace(1)* %heap) #0 gc "cangjie" {
entry:
  %src = alloca %CopySrc, align 8
  %dst = alloca %CopyDst, align 8
  %box = alloca %LocalBox, align 8
  %box.value = getelementptr inbounds %LocalBox, %LocalBox* %box, i64 0, i32 0
  store i8 addrspace(1)* %heap, i8 addrspace(1)** %box.value, align 8
  %box.as1 = addrspacecast %LocalBox* %box to i8 addrspace(1)*
  %src.f0 = getelementptr inbounds %CopySrc, %CopySrc* %src, i64 0, i32 0
  store i8 addrspace(1)* %box.as1, i8 addrspace(1)** %src.f0, align 8
  %src.f1 = getelementptr inbounds %CopySrc, %CopySrc* %src, i64 0, i32 1
  store i8 addrspace(1)* null, i8 addrspace(1)** %src.f1, align 8
  %dst.f0 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 0
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f0, align 8
  %dst.f1 = getelementptr inbounds %CopyDst, %CopyDst* %dst, i64 0, i32 1
  store i8 addrspace(1)* null, i8 addrspace(1)** %dst.f1, align 8
  %src.as1 = addrspacecast %CopySrc* %src to i8 addrspace(1)*
  %dst.as1 = addrspacecast %CopyDst* %dst to i8 addrspace(1)*
  call void @llvm.cj.copy.struct.field.p0i8.i64(i8 addrspace(1)* %dst.as1, i8 addrspace(1)* %dst.as1, i8 addrspace(1)* %src.as1, i8 addrspace(1)* %src.as1, i64 8)
  call void @g()
  call void @use_copy_dst(%CopyDst* %dst)
  ret void
}

; CHECK-LABEL: define void @copy_struct_field_stack_pointee
; CHECK: call token (...) @llvm.cj.gc.statepoint({{.*}}@g{{.*}}"struct-live"({{.*}}%LocalBox* %box{{.*}})
