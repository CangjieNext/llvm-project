; RUN: opt -passes='function(cj-gcinstr-restore)' -S %s -o - | FileCheck %s

declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

define void @vector_gc_store(i8 addrspace(1)* %obj, <2 x i8 addrspace(1)*> %vec) gc "cangjie" {
entry:
  %field = bitcast i8 addrspace(1)* %obj to <2 x i8 addrspace(1)*> addrspace(1)*
  store <2 x i8 addrspace(1)*> %vec, <2 x i8 addrspace(1)*> addrspace(1)* %field, align 8
  ret void
}

define <2 x i8 addrspace(1)*> @vector_gc_load(i8 addrspace(1)* %obj) gc "cangjie" {
entry:
  %field = bitcast i8 addrspace(1)* %obj to <2 x i8 addrspace(1)*> addrspace(1)*
  %vec = load <2 x i8 addrspace(1)*>, <2 x i8 addrspace(1)*> addrspace(1)* %field, align 8
  ret <2 x i8 addrspace(1)*> %vec
}

define <2 x i32> @preserve_non_gc_vector_add(i8 addrspace(1)* %obj, <2 x i8 addrspace(1)*> %vec, <2 x i32> %lhs, <2 x i32> %rhs) gc "cangjie" {
entry:
  %sum = add <2 x i32> %lhs, %rhs
  %field = bitcast i8 addrspace(1)* %obj to <2 x i8 addrspace(1)*> addrspace(1)*
  store <2 x i8 addrspace(1)*> %vec, <2 x i8 addrspace(1)*> addrspace(1)* %field, align 8
  ret <2 x i32> %sum
}

; CHECK-LABEL: define void @vector_gc_store(
; CHECK-NOT: store <2 x i8 addrspace(1)*>
; CHECK: %[[STORE_BASE:.*]] = bitcast <2 x i8 addrspace(1)*> addrspace(1)* %field to i8 addrspace(1)* addrspace(1)*
; CHECK: %[[STORE_E0:.*]] = extractelement <2 x i8 addrspace(1)*> %vec, i32 0
; CHECK: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %[[STORE_E0]], i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %[[STORE_BASE]])
; CHECK: %[[STORE_GEP:.*]] = getelementptr i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %[[STORE_BASE]], i32 1
; CHECK: %[[STORE_E1:.*]] = extractelement <2 x i8 addrspace(1)*> %vec, i32 1
; CHECK: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %[[STORE_E1]], i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %[[STORE_GEP]])

; CHECK-LABEL: define <2 x i8 addrspace(1)*> @vector_gc_load(
; CHECK-NOT: load <2 x i8 addrspace(1)*>
; CHECK: %[[LOAD_BASE:.*]] = bitcast <2 x i8 addrspace(1)*> addrspace(1)* %field to i8 addrspace(1)* addrspace(1)*
; CHECK: %[[LOAD0:.*]] = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %[[LOAD_BASE]])
; CHECK: %[[V0:.*]] = insertelement <2 x i8 addrspace(1)*> poison, i8 addrspace(1)* %[[LOAD0]], i32 0
; CHECK: %[[LOAD_GEP:.*]] = getelementptr i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %[[LOAD_BASE]], i32 1
; CHECK: %[[LOAD1:.*]] = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %[[LOAD_GEP]])
; CHECK: %[[V1:.*]] = insertelement <2 x i8 addrspace(1)*> %[[V0]], i8 addrspace(1)* %[[LOAD1]], i32 1
; CHECK: ret <2 x i8 addrspace(1)*> %[[V1]]

; CHECK-LABEL: define <2 x i32> @preserve_non_gc_vector_add(
; CHECK: %sum = add <2 x i32> %lhs, %rhs
; CHECK: %[[KEEP_E0:.*]] = extractelement <2 x i8 addrspace(1)*> %vec, i32 0
; CHECK: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %[[KEEP_E0]], i8 addrspace(1)* %obj
; CHECK: ret <2 x i32> %sum
