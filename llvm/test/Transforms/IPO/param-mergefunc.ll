; RUN: opt -passes='mergefunc' -S < %s | FileCheck %s --check-prefix=MERGEFUNC
; RUN: opt -passes='mergefunc,param-mergefunc' -S < %s | FileCheck %s --check-prefix=PARAM

@GA = internal global i32 1
@GB = internal global i32 2
@SGA = internal global i8 addrspace(1)* null
@SGB = internal global i8 addrspace(1)* null
@ma0 = internal global i32 (i32)* @m0
@ma1 = internal global i32 (i32)* @m1

declare i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** nocapture)
declare i32 @__cj_personality_v0$(...)
declare i8* @cb0(i8*, i8*, i8*)
declare i8* @cb1(i8*, i8*, i8*)

define internal i32 @f0(i32 %x) {
entry:
  %p = load i32, i32* @GA, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

define internal i32 @f1(i32 %x) {
entry:
  %p = load i32, i32* @GB, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

define internal i8 addrspace(1)* @g0() {
entry:
  %v = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGA)
  ret i8 addrspace(1)* %v
}

define internal i8 addrspace(1)* @g1() {
entry:
  %v = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGB)
  ret i8 addrspace(1)* %v
}

define internal i32 @h0(i32 %x) gc "cangjie" personality i32 (...)* @__cj_personality_v0$ {
entry:
  %p = load i32, i32* @GA, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

define internal i32 @h1(i32 %x) gc "cangjie" personality i32 (...)* @__cj_personality_v0$ {
entry:
  %p = load i32, i32* @GB, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

define internal i8* @k0(i8* %p, i8* %q) {
entry:
  %r = tail call i8* @cb0(i8* null, i8* %p, i8* %q)
  ret i8* %r
}

define internal i8* @k1(i8* %p, i8* %q) {
entry:
  %r = tail call i8* @cb1(i8* null, i8* %p, i8* %q)
  ret i8* %r
}

define internal i32 @m0(i32 %x) {
entry:
  %p = load i32, i32* @GA, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

define internal i32 @m1(i32 %x) {
entry:
  %p = load i32, i32* @GB, align 4
  %s = add i32 %x, %p
  ret i32 %s
}

; MERGEFUNC-LABEL: define internal i32 @f0(
; MERGEFUNC: load i32, i32* @GA
; MERGEFUNC-LABEL: define internal i32 @f1(
; MERGEFUNC: load i32, i32* @GB
; MERGEFUNC-LABEL: define internal i8 addrspace(1)* @g0(
; MERGEFUNC: call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGA)
; MERGEFUNC-LABEL: define internal i8 addrspace(1)* @g1(
; MERGEFUNC: call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGB)

; PARAM-LABEL: define internal i32 @f0(
; PARAM: tail call i32 @f0.Tpm(i32 %x, i32* @GA)
; PARAM-NEXT: ret i32

; PARAM-LABEL: define internal i32 @f1(
; PARAM: tail call i32 @f0.Tpm(i32 %x, i32* @GB)
; PARAM-NEXT: ret i32

; PARAM-LABEL: define internal i32 @f0.Tpm(
; PARAM: load i32, i32* %1
; PARAM: add i32 %0
; PARAM: ret i32

; PARAM-LABEL: define internal i8 addrspace(1)* @g0(
; PARAM: call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGA)
; PARAM-NEXT: ret i8 addrspace(1)*

; PARAM-LABEL: define internal i8 addrspace(1)* @g1(
; PARAM: call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** @SGB)
; PARAM-NEXT: ret i8 addrspace(1)*

; PARAM-LABEL: define internal i32 @h0(
; PARAM: call i32 @h0.Tpm(i32 %x, i32* @GA)
; PARAM-NEXT: ret i32

; PARAM-LABEL: define internal i32 @h1(
; PARAM: call i32 @h0.Tpm(i32 %x, i32* @GB)
; PARAM-NEXT: ret i32

; PARAM-LABEL: define internal i32 @h0.Tpm(
; PARAM: load i32, i32* %1
; PARAM: add i32 %0
; PARAM: ret i32

; PARAM-LABEL: define internal i8* @k0.Tpm(
; PARAM: call i8* %2(i8* null, i8* %0, i8* %1)
; PARAM-NEXT: ret i8*

; PARAM-LABEL: define internal i8* @k0(
; PARAM: notail call i8* @k0.Tpm(i8* %p, i8* %q, i8* (i8*, i8*, i8*)* @cb0)
; PARAM-NEXT: ret i8*

; PARAM-LABEL: define internal i8* @k1(
; PARAM: notail call i8* @k0.Tpm(i8* %p, i8* %q, i8* (i8*, i8*, i8*)* @cb1)
; PARAM-NEXT: ret i8*

; PARAM-LABEL: define internal i32 @m0(
; PARAM: load i32, i32* @GA
; PARAM-NEXT: add i32 %x, %p
; PARAM-NEXT: ret i32 %s

; PARAM-LABEL: define internal i32 @m1(
; PARAM: load i32, i32* @GB
; PARAM-NEXT: add i32 %x, %p
; PARAM-NEXT: ret i32 %s
