; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

target datalayout = "e-p:64:64-p1:64:64"
%box = type { i8*, i8 addrspace(1)* }
declare void @poll()
declare void @llvm.memset.p0i8.i64(i8*, i8, i64, i1)
declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)

; A header write leaves the GC field uninitialized on the unselected path.
; CHECK-LABEL: define {{.*}}@header_store(
; CHECK: %left = alloca %box
; CHECK-NEXT: [[LEFT:%.*]] = bitcast %box* %left to i8*
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 [[LEFT]], i8 0, i64 16, i1 false)
; CHECK: %right = alloca %box
; CHECK-NEXT: [[RIGHT:%.*]] = bitcast %box* %right to i8*
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 [[RIGHT]], i8 0, i64 16, i1 false)
define i8 addrspace(1)* @header_store(i1 %cond, i8* %tag, i8* %source, i8 addrspace(1)* %value) gc "cangjie" {
entry:
  %left = alloca %box, align 8
  %right = alloca %box, align 8
  %left.header = bitcast %box* %left to i8**
  store i8* %tag, i8** %left.header
  %right.header = bitcast %box* %right to i8**
  store i8* %tag, i8** %right.header
  call void @poll()
  br i1 %cond, label %set.left, label %set.right
set.left:
  %left.field = getelementptr %box, %box* %left, i32 0, i32 1
  store i8 addrspace(1)* %value, i8 addrspace(1)** %left.field
  br label %merge
set.right:
  %right.field = getelementptr %box, %box* %right, i32 0, i32 1
  store i8 addrspace(1)* %value, i8 addrspace(1)** %right.field
  br label %merge
merge:
  %chosen = phi %box* [ %left, %set.left ], [ %right, %set.right ]
  call void @poll()
  %field = getelementptr %box, %box* %chosen, i32 0, i32 1
  %result = load i8 addrspace(1)*, i8 addrspace(1)** %field
  ret i8 addrspace(1)* %result
}

; A header write leaves the GC field uninitialized on the unselected path.
; CHECK-LABEL: define {{.*}}@partial_memset(
; CHECK: %left = alloca %box
; CHECK-NEXT: [[LEFT:%.*]] = bitcast %box* %left to i8*
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 [[LEFT]], i8 0, i64 16, i1 false)
; CHECK: %right = alloca %box
; CHECK-NEXT: [[RIGHT:%.*]] = bitcast %box* %right to i8*
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 [[RIGHT]], i8 0, i64 16, i1 false)
define i8 addrspace(1)* @partial_memset(i1 %cond, i8* %tag, i8* %source, i8 addrspace(1)* %value) gc "cangjie" {
entry:
  %left = alloca %box, align 8
  %right = alloca %box, align 8
  %left.bytes = bitcast %box* %left to i8*
  call void @llvm.memset.p0i8.i64(i8* %left.bytes, i8 0, i64 8, i1 false)
  %right.bytes = bitcast %box* %right to i8*
  call void @llvm.memset.p0i8.i64(i8* %right.bytes, i8 0, i64 8, i1 false)
  call void @poll()
  br i1 %cond, label %set.left, label %set.right
set.left:
  %left.field = getelementptr %box, %box* %left, i32 0, i32 1
  store i8 addrspace(1)* %value, i8 addrspace(1)** %left.field
  br label %merge
set.right:
  %right.field = getelementptr %box, %box* %right, i32 0, i32 1
  store i8 addrspace(1)* %value, i8 addrspace(1)** %right.field
  br label %merge
merge:
  %chosen = phi %box* [ %left, %set.left ], [ %right, %set.right ]
  call void @poll()
  %field = getelementptr %box, %box* %chosen, i32 0, i32 1
  %result = load i8 addrspace(1)*, i8 addrspace(1)** %field
  ret i8 addrspace(1)* %result
}

; A header write leaves the GC field uninitialized on the unselected path.
; CHECK-LABEL: define {{.*}}@partial_memcpy(
; CHECK: %left = alloca %box
; CHECK-NEXT: [[LEFT:%.*]] = bitcast %box* %left to i8*
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 [[LEFT]], i8 0, i64 16, i1 false)
; CHECK: %right = alloca %box
; CHECK-NEXT: [[RIGHT:%.*]] = bitcast %box* %right to i8*
; CHECK-NEXT: call void @llvm.memset.p0i8.i64(i8* align 8 [[RIGHT]], i8 0, i64 16, i1 false)
define i8 addrspace(1)* @partial_memcpy(i1 %cond, i8* %tag, i8* %source, i8 addrspace(1)* %value) gc "cangjie" {
entry:
  %left = alloca %box, align 8
  %right = alloca %box, align 8
  %left.bytes = bitcast %box* %left to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %left.bytes, i8* %source, i64 8, i1 false)
  %right.bytes = bitcast %box* %right to i8*
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %right.bytes, i8* %source, i64 8, i1 false)
  call void @poll()
  br i1 %cond, label %set.left, label %set.right
set.left:
  %left.field = getelementptr %box, %box* %left, i32 0, i32 1
  store i8 addrspace(1)* %value, i8 addrspace(1)** %left.field
  br label %merge
set.right:
  %right.field = getelementptr %box, %box* %right, i32 0, i32 1
  store i8 addrspace(1)* %value, i8 addrspace(1)** %right.field
  br label %merge
merge:
  %chosen = phi %box* [ %left, %set.left ], [ %right, %set.right ]
  call void @poll()
  %field = getelementptr %box, %box* %chosen, i32 0, i32 1
  %result = load i8 addrspace(1)*, i8 addrspace(1)** %field
  ret i8 addrspace(1)* %result
}

; A whole-aggregate store already initializes every GC field.
; CHECK-LABEL: define {{.*}}@complete_store(
; CHECK-NOT: call void @llvm.memset
; CHECK: ret i8 addrspace(1)*
define i8 addrspace(1)* @complete_store(i1 %cond, %box %initial) gc "cangjie" {
entry:
  %left = alloca %box, align 8
  %right = alloca %box, align 8
  store %box %initial, %box* %left
  store %box %initial, %box* %right
  %chosen = select i1 %cond, %box* %left, %box* %right
  call void @poll()
  %field = getelementptr %box, %box* %chosen, i32 0, i32 1
  %result = load i8 addrspace(1)*, i8 addrspace(1)** %field
  ret i8 addrspace(1)* %result
}
