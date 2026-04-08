; RUN: opt -passes=cj-annotation-outline -S %s | FileCheck %s

declare i8 addrspace(1)* @llvm.cj.malloc.object(i8*, i32)
declare void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)*, i8 addrspace(1)** nocapture)

@_CAO7default3fooE = internal global i8 addrspace(1)* null
@_CAO7default3barE = internal global i8 addrspace(1)* null
@_CAO7default3bazE = internal global i8 addrspace(1)* null
@_CAO7default3quxE = internal global i8 addrspace(1)* null

define internal fastcc void @_CAO7default3foo_iiHv() alwaysinline gc "cangjie" {
bb3:
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 16)
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*
  store i64 42, i64 addrspace(1)* %1, align 8
  %2 = getelementptr i64, i64 addrspace(1)* %1, i32 1
  store i64 7, i64 addrspace(1)* %2, align 8
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %0, i8 addrspace(1)** @_CAO7default3fooE)
  ret void
}

define internal fastcc void @_CAO7default3bar_iiHv() alwaysinline gc "cangjie" {
bb3:
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 16)
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*
  store i64 42, i64 addrspace(1)* %1, align 8
  %2 = getelementptr i64, i64 addrspace(1)* %1, i32 1
  store i64 7, i64 addrspace(1)* %2, align 8
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %0, i8 addrspace(1)** @_CAO7default3barE)
  ret void
}

; Negative: identical bodies, but they write different objects, so they must not
; be merged with each other or with the foo/bar group.
define internal void @_CAO7default3baz_iiHv() gc "cangjie" {
bb3:
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 16)
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*
  store i64 42, i64 addrspace(1)* %1, align 8
  %2 = getelementptr i64, i64 addrspace(1)* %1, i32 1
  store i64 7, i64 addrspace(1)* %2, align 8
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* null, i8 addrspace(1)** @_CAO7default3bazE)
  ret void
}

define internal void @_CAO7default3qux_iiHv() gc "cangjie" {
bb3:
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 16)
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*
  store i64 42, i64 addrspace(1)* %1, align 8
  %2 = getelementptr i64, i64 addrspace(1)* %1, i32 1
  store i64 7, i64 addrspace(1)* %2, align 8
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* inttoptr (i64 8 to i8 addrspace(1)*), i8 addrspace(1)** @_CAO7default3quxE)
  ret void
}

; CHECK-LABEL: define internal fastcc void @_CAO7default3foo_iiHv()
; CHECK: call fastcc void @_CAO7default3foo_iiHv_imp(i8 addrspace(1)** @_CAO7default3fooE)
; CHECK-NEXT: ret void

; CHECK-LABEL: define internal fastcc void @_CAO7default3bar_iiHv()
; CHECK: call fastcc void @_CAO7default3foo_iiHv_imp(i8 addrspace(1)** @_CAO7default3barE)
; CHECK-NEXT: ret void

; CHECK-LABEL: define internal void @_CAO7default3baz_iiHv()
; CHECK: call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* null, i8 addrspace(1)** @_CAO7default3bazE)

; CHECK-LABEL: define internal void @_CAO7default3qux_iiHv()
; CHECK: call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* inttoptr (i64 8 to i8 addrspace(1)*), i8 addrspace(1)** @_CAO7default3quxE)

; CHECK-LABEL: define internal fastcc void @_CAO7default3foo_iiHv_imp(i8 addrspace(1)** %slot) local_unnamed_addr
; CHECK: %[[OBJ:.*]] = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 16)
; CHECK-NEXT: %[[PAYLOAD:.*]] = bitcast i8 addrspace(1)* %[[OBJ]] to i64 addrspace(1)*
; CHECK-NEXT: store i64 42, i64 addrspace(1)* %[[PAYLOAD]], align 8
; CHECK-NEXT: %[[NEXT:.*]] = getelementptr i64, i64 addrspace(1)* %[[PAYLOAD]], i32 1
; CHECK-NEXT: store i64 7, i64 addrspace(1)* %[[NEXT]], align 8
; CHECK-NEXT: call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %[[OBJ]], i8 addrspace(1)** %slot)
; CHECK-NEXT: ret void
