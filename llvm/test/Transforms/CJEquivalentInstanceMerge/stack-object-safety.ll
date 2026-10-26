; RUN: opt --cangjie-pipeline -passes=cj-equivalent-instance-merge -S < %s | FileCheck %s

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8*, i8*, i8* }
%"ObjLayout.pkg:Temp<Int64>" = type { [40 x i8] }
%"ObjLayout.pkg:Temp<IntNative>" = type { [40 x i8] }
%"ObjLayout.pkg:Temp<Int32>" = type { [40 x i8] }
%"ObjLayout.pkg:Temp<Rune>" = type { [40 x i8] }

@OffsetsInt64 = private constant [3 x i32] [i32 0, i32 24, i32 32]
@OffsetsIntNative = private constant [3 x i32] [i32 0, i32 24, i32 32]

@"RawArray<Int64>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<IntNative>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<Int32>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<Rune>.ti" = linkonce_odr global %TypeInfo zeroinitializer
; Distinct offset globals with identical contents are layout-equivalent.
@"pkg:Temp<Int64>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 0, i8 0, i16 0, i32 0, i8* null, i32 0, i8 0, i8 0, i16 0, i32* getelementptr inbounds ([3 x i32], [3 x i32]* @OffsetsInt64, i32 0, i32 0), i8* null, i8* null, i8* null, %TypeInfo* null, i8* null, i8* null, i8* null }
@"pkg:Temp<IntNative>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 0, i8 0, i16 0, i32 0, i8* null, i32 0, i8 0, i8 0, i16 0, i32* getelementptr inbounds ([3 x i32], [3 x i32]* @OffsetsIntNative, i32 0, i32 0), i8* null, i8* null, i8* null, %TypeInfo* null, i8* null, i8* null, i8* null }
@"pkg:Temp<Int32>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Temp<Rune>.ti" = linkonce_odr global %TypeInfo zeroinitializer

declare i8* @CJ_MCC_NewArray64(i8*, i64, i64)
declare void @use(i8*)
declare void @llvm.memcpy.p0i8.p1i8.i64(i8* noalias nocapture writeonly, i8 addrspace(1)* noalias nocapture readonly, i64, i1 immarg)
declare void @llvm.cj.gcread.struct(i8* nocapture writeonly, i8 addrspace(1)* nocapture, i8 addrspace(1)* nocapture readonly, i64)

define internal i8 @read_payload(i8 addrspace(1)* nocapture %obj) {
  %ptr = getelementptr i8, i8 addrspace(1)* %obj, i64 8
  %value = load i8, i8 addrspace(1)* %ptr
  ret i8 %value
}

define internal void @copy_payload(i8* nocapture %dest, i8 addrspace(1)* nocapture %obj) {
  %src = getelementptr i8, i8 addrspace(1)* %obj, i64 8
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* %dest, i8 addrspace(1)* %src, i64 16, i1 false)
  ret void
}

define internal i8* @read_header(i8 addrspace(1)* nocapture %obj) {
  %ptr = bitcast i8 addrspace(1)* %obj to i8* addrspace(1)*
  %ti = load i8*, i8* addrspace(1)* %ptr
  ret i8* %ti
}

define internal void @copy_header(i8* nocapture %dest, i8 addrspace(1)* nocapture %obj) {
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* %dest, i8 addrspace(1)* %obj, i64 16, i1 false)
  ret void
}

; A recursive cycle is evaluated as part of the complete root analysis. Its
; header read must not be hidden by a cached nested success.
define internal void @recursive_header(i8 addrspace(1)* nocapture %obj, i1 %again) {
  br i1 %again, label %recurse, label %read
recurse:
  call void @recursive_header(i8 addrspace(1)* %obj, i1 false)
  ret void
read:
  %ptr = bitcast i8 addrspace(1)* %obj to i8* addrspace(1)*
  %ti = load i8*, i8* addrspace(1)* %ptr
  call void @use(i8* %ti)
  ret void
}

; The source operand of a GC struct copy can expose the header just like
; llvm.memcpy, even when both pointer operands are nocapture.
define internal void @read_header_gc(i8* nocapture %dest, i8 addrspace(1)* nocapture %obj) {
  call void @llvm.cj.gcread.struct(i8* %dest, i8 addrspace(1)* %obj, i8 addrspace(1)* %obj, i64 16)
  ret void
}

; select/phi/freeze can hide the header base from constant-offset tracking.
; Such pointers must be treated as possible header accesses, not as payload.
define internal i8* @read_select_header(i1 %cond, i8 addrspace(1)* nocapture %obj, i8 addrspace(1)* nocapture %other) {
  %selected = select i1 %cond, i8 addrspace(1)* %obj, i8 addrspace(1)* %other
  %ptr = bitcast i8 addrspace(1)* %selected to i8* addrspace(1)*
  %ti = load i8*, i8* addrspace(1)* %ptr
  ret i8* %ti
}

; Both helpers only touch payload. The IntNative body can be folded after its
; stack header and RawArray allocation are canonicalized to Int64.
define internal void @a_safe_int64() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }, !ea_val !0
  %dest = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 1, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int64>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %value = call i8 @read_payload(i8 addrspace(1)* %ref)
  %dest.ptr = bitcast [16 x i8]* %dest to i8*
  call void @copy_payload(i8* %dest.ptr, i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  ret void
}

define internal void @b_safe_intnative() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }, !ea_val !0
  %dest = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 1, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %value = call i8 @read_payload(i8 addrspace(1)* %ref)
  %dest.ptr = bitcast [16 x i8]* %dest to i8*
  call void @copy_payload(i8* %dest.ptr, i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  ret void
}

; nocapture does not make a header load unobservable. This pair must remain.
define internal void @c_header_int32() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int32>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int32>.ti" to i8*), i64 2, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int32>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int32>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int32>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %ti = call i8* @read_header(i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  call void @use(i8* %ti)
  ret void
}

define internal void @d_header_rune() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Rune>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Rune>.ti" to i8*), i64 2, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Rune>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Rune>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Rune>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %ti = call i8* @read_header(i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  call void @use(i8* %ti)
  ret void
}

; Copying header bytes to another buffer can expose TypeInfo just like a load.
define internal void @e_copy_int64() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }, !ea_val !0
  %dest = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 3, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int64>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %dest.ptr = bitcast [16 x i8]* %dest to i8*
  call void @copy_header(i8* %dest.ptr, i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  ret void
}

define internal void @f_copy_intnative() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }, !ea_val !0
  %dest = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 3, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %dest.ptr = bitcast [16 x i8]* %dest to i8*
  call void @copy_header(i8* %dest.ptr, i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  ret void
}

; Recursive helpers must not hide a header read in a cycle.
define internal void @g_recursive_int64() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 4, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int64>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  call void @recursive_header(i8 addrspace(1)* %ref, i1 true)
  call void @use(i8* %raw)
  ret void
}

define internal void @h_recursive_intnative() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 4, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  call void @recursive_header(i8 addrspace(1)* %ref, i1 true)
  call void @use(i8* %raw)
  ret void
}

define internal void @i_gc_int64() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }, !ea_val !0
  %dest = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 5, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int64>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %dest.ptr = bitcast [16 x i8]* %dest to i8*
  call void @read_header_gc(i8* %dest.ptr, i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  ret void
}

define internal void @j_gc_intnative() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }, !ea_val !0
  %dest = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 5, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %dest.ptr = bitcast [16 x i8]* %dest to i8*
  call void @read_header_gc(i8* %dest.ptr, i8 addrspace(1)* %ref)
  call void @use(i8* %raw)
  ret void
}

define internal void @k_select_int64() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }, !ea_val !0
  %other = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 6, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int64>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %other.base = bitcast [16 x i8]* %other to i8*
  %other.ref = addrspacecast i8* %other.base to i8 addrspace(1)*
  %ti = call i8* @read_select_header(i1 true, i8 addrspace(1)* %ref, i8 addrspace(1)* %other.ref)
  call void @use(i8* %raw)
  call void @use(i8* %ti)
  ret void
}

define internal void @l_select_intnative() {
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }, !ea_val !0
  %other = alloca [16 x i8]
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 6, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*), i8** %header
  %base = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8*
  %ref = addrspacecast i8* %base to i8 addrspace(1)*
  %other.base = bitcast [16 x i8]* %other to i8*
  %other.ref = addrspacecast i8* %other.base to i8 addrspace(1)*
  %ti = call i8* @read_select_header(i1 true, i8 addrspace(1)* %ref, i8 addrspace(1)* %other.ref)
  call void @use(i8* %raw)
  call void @use(i8* %ti)
  ret void
}

!0 = !{}

; CHECK: define internal void @a_safe_int64()
; CHECK-NOT: define internal void @b_safe_intnative()
; CHECK: define internal void @c_header_int32()
; CHECK: define internal void @d_header_rune()
; CHECK: store i8* bitcast (%TypeInfo* @"pkg:Temp<Rune>.ti" to i8*)
; CHECK: define internal void @e_copy_int64()
; CHECK: define internal void @f_copy_intnative()
; CHECK: store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*)
; CHECK: define internal void @g_recursive_int64()
; CHECK: define internal void @h_recursive_intnative()
; CHECK: store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*)
; CHECK: define internal void @i_gc_int64()
; CHECK: define internal void @j_gc_intnative()
; CHECK: store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*)
; CHECK: define internal void @k_select_int64()
; CHECK: define internal void @l_select_intnative()
; CHECK: store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*)
