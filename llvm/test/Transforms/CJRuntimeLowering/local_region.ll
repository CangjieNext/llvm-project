; RUN: opt < %s -passes=cj-runtime-lowering -S | FileCheck %s

declare i1 @llvm.cj.start.local.region()
declare void @llvm.cj.end.local.region()
declare i8 addrspace(1)* @llvm.cj.malloc.local.object(i8*, i32)
declare i8 addrspace(1)* @llvm.cj.malloc.local.weakref.object(i8*, i32)
declare i8 addrspace(1)* @llvm.cj.alloca.local.generic(i8*, i32)
declare i8 addrspace(1)* @llvm.cj.malloc.local.array(i8*, i64)
declare i8 addrspace(1)* @llvm.cj.malloc.local.obj.array(i8*, i64)
declare i8 addrspace(1)* @llvm.cj.malloc.local.array8(i8*, i64)
declare i8 addrspace(1)* @llvm.cj.malloc.local.array16(i8*, i64)
declare i8 addrspace(1)* @llvm.cj.malloc.local.array32(i8*, i64)
declare i8 addrspace(1)* @llvm.cj.malloc.local.array64(i8*, i64)
declare i8 addrspace(1)* @llvm.cj.malloc.local.array.generic(i8*, i64)

; CHECK-LABEL: @lower_local_region(
; CHECK: call i1 @CJ_MCC_StartLocalRegion()
; CHECK: call void @CJ_MCC_EndLocalRegion()
define void @lower_local_region() {
  %1 = call i1 @llvm.cj.start.local.region()
  call void @llvm.cj.end.local.region()
  ret void
}

; CHECK-LABEL: @lower_local_objects(
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalObject(i8* %ti, i32 32)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalWeakRefObject(i8* %ti, i32 16)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalGenericObject(i8* %ti, i32 48)
define void @lower_local_objects(i8* %ti) {
  %1 = call i8 addrspace(1)* @llvm.cj.malloc.local.object(i8* %ti, i32 32)
  %2 = call i8 addrspace(1)* @llvm.cj.malloc.local.weakref.object(i8* %ti, i32 16)
  %3 = call i8 addrspace(1)* @llvm.cj.alloca.local.generic(i8* %ti, i32 48)
  ret void
}

; CHECK-LABEL: @lower_local_arrays(
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray(i8* %ti, i64 %n)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalObjArray(i8* %ti, i64 %n)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray8(i8* %ti, i64 %n)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray16(i8* %ti, i64 %n)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray32(i8* %ti, i64 %n)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray64(i8* %ti, i64 %n)
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalGenericArray(i8* %ti, i64 %n)
define void @lower_local_arrays(i8* %ti, i64 %n) {
  %1 = call i8 addrspace(1)* @llvm.cj.malloc.local.array(i8* %ti, i64 %n)
  %2 = call i8 addrspace(1)* @llvm.cj.malloc.local.obj.array(i8* %ti, i64 %n)
  %3 = call i8 addrspace(1)* @llvm.cj.malloc.local.array8(i8* %ti, i64 %n)
  %4 = call i8 addrspace(1)* @llvm.cj.malloc.local.array16(i8* %ti, i64 %n)
  %5 = call i8 addrspace(1)* @llvm.cj.malloc.local.array32(i8* %ti, i64 %n)
  %6 = call i8 addrspace(1)* @llvm.cj.malloc.local.array64(i8* %ti, i64 %n)
  %7 = call i8 addrspace(1)* @llvm.cj.malloc.local.array.generic(i8* %ti, i64 %n)
  ret void
}

; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalObject(i8*, i32) #[[LOCALMALLOC:[0-9]+]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalGenericObject(i8*, i32) #[[LOCALMALLOC]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray(i8*, i64) #[[LOCALMALLOC]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalObjArray(i8*, i64) #[[LOCALMALLOC]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray8(i8*, i64) #[[LOCALMALLOC]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray16(i8*, i64) #[[LOCALMALLOC]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray32(i8*, i64) #[[LOCALMALLOC]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalArray64(i8*, i64) #[[LOCALMALLOC]]
; CHECK: declare noalias i8 addrspace(1)* @CJ_MCC_NewLocalGenericArray(i8*, i64) #[[LOCALMALLOC]]
; CHECK: attributes #[[LOCALMALLOC]] = { argmemonly "cj-heapmalloc" "cj-runtime" }
; CHECK: declare i8 addrspace(1)* @CJ_MCC_NewLocalWeakRefObject(i8*, i32) #[[LOCALWEAK:[0-9]+]]
; CHECK: attributes #[[LOCALWEAK]] = { "cj-runtime" }
