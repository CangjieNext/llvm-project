; RUN: opt -passes='mergefunc' -S < %s | FileCheck %s --check-prefix=MERGEFUNC
; RUN: opt -passes='mergefunc,param-mergefunc' -S < %s | FileCheck %s --check-prefix=PARAM

@GA = internal global i32 1
@GB = internal global i32 2
@SGA = internal global i8 addrspace(1)* null
@SGB = internal global i8 addrspace(1)* null

declare i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** nocapture)

define i32 @f0(i32 %x) {
entry:
  %p = load i32, i32* @GA, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

define i32 @f1(i32 %x) {
entry:
  %p = load i32, i32* @GB, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

define i8 addrspace(1)* @g0() {
entry:
  %v = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGA)
  ret i8 addrspace(1)* %v
}

define i8 addrspace(1)* @g1() {
entry:
  %v = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGB)
  ret i8 addrspace(1)* %v
}

; MERGEFUNC-LABEL: define i32 @f0(
; MERGEFUNC: load i32, i32* @GA
; MERGEFUNC-LABEL: define i32 @f1(
; MERGEFUNC: load i32, i32* @GB
; MERGEFUNC-LABEL: define i8 addrspace(1)* @g0(
; MERGEFUNC: call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGA)
; MERGEFUNC-LABEL: define i8 addrspace(1)* @g1(
; MERGEFUNC: call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGB)

; PARAM-LABEL: define i32 @f0(
; PARAM: tail call i32 @f0.Tpm(i32 %x, i32* @GA)
; PARAM-NEXT: ret i32

; PARAM-LABEL: define i32 @f1(
; PARAM: tail call i32 @f0.Tpm(i32 %x, i32* @GB)
; PARAM-NEXT: ret i32

; PARAM-LABEL: define internal i32 @f0.Tpm(
; PARAM: load i32, i32* %1
; PARAM: add i32 %0
; PARAM: ret i32

; PARAM-LABEL: define i8 addrspace(1)* @g0(
; PARAM: call i8 addrspace(1)* @g0.Tpm(i8 addrspace(1)** @SGA)
; PARAM-NEXT: ret i8 addrspace(1)*

; PARAM-LABEL: define i8 addrspace(1)* @g1(
; PARAM: call i8 addrspace(1)* @g0.Tpm(i8 addrspace(1)** @SGB)
; PARAM-NEXT: ret i8 addrspace(1)*

; PARAM-LABEL: define internal i8 addrspace(1)* @g0.Tpm(
; PARAM: call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** %0)
; PARAM: ret i8 addrspace(1)*
