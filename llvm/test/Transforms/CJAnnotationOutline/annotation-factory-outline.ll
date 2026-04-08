; RUN: opt -passes=cj-annotation-outline -S %s | FileCheck %s

%Arr = type { [32 x i8] }

declare i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** nocapture)
declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

@_CAO7default3fooE = internal global i8 addrspace(1)* null
@_CAO7default3barE = internal global i8 addrspace(1)* null
@_CAO7default3bazE = internal global i8 addrspace(1)* null
@_CAO7default3quxE = internal global i8 addrspace(1)* null

define void @_CAF7default3fooE(%Arr* sret(%Arr) %0) gc "cangjie" {
bb0:
  %1 = addrspacecast %Arr* %0 to %Arr addrspace(1)*
  %2 = bitcast %Arr addrspace(1)* %1 to i8 addrspace(1)*
  %3 = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @_CAO7default3fooE)
  %4 = getelementptr i8, i8 addrspace(1)* %2, i64 16
  %5 = bitcast i8 addrspace(1)* %4 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %3, i8 addrspace(1)* %2, i8 addrspace(1)* addrspace(1)* %5)
  %6 = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @_CAO7default3barE)
  %7 = getelementptr i8, i8 addrspace(1)* %2, i64 24
  %8 = bitcast i8 addrspace(1)* %7 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %6, i8 addrspace(1)* %2, i8 addrspace(1)* addrspace(1)* %8)
  ret void
}

define void @_CAF7default3barE(%Arr* sret(%Arr) %0) gc "cangjie" {
bb0:
  %1 = addrspacecast %Arr* %0 to %Arr addrspace(1)*
  %2 = bitcast %Arr addrspace(1)* %1 to i8 addrspace(1)*
  %3 = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @_CAO7default3bazE)
  %4 = getelementptr i8, i8 addrspace(1)* %2, i64 16
  %5 = bitcast i8 addrspace(1)* %4 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %3, i8 addrspace(1)* %2, i8 addrspace(1)* addrspace(1)* %5)
  %6 = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @_CAO7default3quxE)
  %7 = getelementptr i8, i8 addrspace(1)* %2, i64 24
  %8 = bitcast i8 addrspace(1)* %7 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %6, i8 addrspace(1)* %2, i8 addrspace(1)* addrspace(1)* %8)
  ret void
}

; Negative: same shape and slots as the foo/bar group, but the first store lands
; at a different offset, so it must not be merged with them.
define void @_CAF7default3bazE(%Arr* sret(%Arr) %0) gc "cangjie" {
bb0:
  %1 = addrspacecast %Arr* %0 to %Arr addrspace(1)*
  %2 = bitcast %Arr addrspace(1)* %1 to i8 addrspace(1)*
  %3 = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @_CAO7default3fooE)
  %4 = getelementptr i8, i8 addrspace(1)* %2, i64 8
  %5 = bitcast i8 addrspace(1)* %4 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %3, i8 addrspace(1)* %2, i8 addrspace(1)* addrspace(1)* %5)
  %6 = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @_CAO7default3barE)
  %7 = getelementptr i8, i8 addrspace(1)* %2, i64 24
  %8 = bitcast i8 addrspace(1)* %7 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %6, i8 addrspace(1)* %2, i8 addrspace(1)* addrspace(1)* %8)
  ret void
}

; CHECK-LABEL: define void @_CAF7default3fooE(%Arr* sret(%Arr) %0)
; CHECK: call void @_CAF7default3fooE_imp(%Arr* sret(%Arr) %0, i8 addrspace(1)** @_CAO7default3fooE, i8 addrspace(1)** @_CAO7default3barE)
; CHECK-NEXT: ret void

; CHECK-LABEL: define void @_CAF7default3barE(%Arr* sret(%Arr) %0)
; CHECK: call void @_CAF7default3fooE_imp(%Arr* sret(%Arr) %0, i8 addrspace(1)** @_CAO7default3bazE, i8 addrspace(1)** @_CAO7default3quxE)
; CHECK-NEXT: ret void

; CHECK-LABEL: define void @_CAF7default3bazE(%Arr* sret(%Arr) %0)
; CHECK: getelementptr i8, i8 addrspace(1)* {{.*}}, i64 8

; CHECK-LABEL: define internal void @_CAF7default3fooE_imp(%Arr* sret(%Arr) %0, i8 addrspace(1)** %slot0, i8 addrspace(1)** %slot1) local_unnamed_addr
; CHECK: %[[ARRAS1:.*]] = addrspacecast %Arr* %0 to %Arr addrspace(1)*
; CHECK-NEXT: %[[BASE:.*]] = bitcast %Arr addrspace(1)* %[[ARRAS1]] to i8 addrspace(1)*
; CHECK-NEXT: %[[VAL0:.*]] = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** %slot0)
; CHECK-NEXT: %[[GEP0:.*]] = getelementptr i8, i8 addrspace(1)* %[[BASE]], i64 16
; CHECK-NEXT: %[[PTR0:.*]] = bitcast i8 addrspace(1)* %[[GEP0]] to i8 addrspace(1)* addrspace(1)*
; CHECK-NEXT: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %[[VAL0]], i8 addrspace(1)* %[[BASE]], i8 addrspace(1)* addrspace(1)* %[[PTR0]])
; CHECK-NEXT: %[[VAL1:.*]] = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** %slot1)
; CHECK-NEXT: %[[GEP1:.*]] = getelementptr i8, i8 addrspace(1)* %[[BASE]], i64 24
; CHECK-NEXT: %[[PTR1:.*]] = bitcast i8 addrspace(1)* %[[GEP1]] to i8 addrspace(1)* addrspace(1)*
; CHECK-NEXT: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %[[VAL1]], i8 addrspace(1)* %[[BASE]], i8 addrspace(1)* addrspace(1)* %[[PTR1]])
; CHECK-NEXT: ret void
