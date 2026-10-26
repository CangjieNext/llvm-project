; RUN: opt --cangjie-pipeline -passes=cj-equivalent-instance-merge -S < %s | FileCheck %s
; RUN: opt --cangjie-pipeline -passes=cj-equivalent-instance-merge -cj-equivalent-instance-merge=false -S < %s | FileCheck %s --check-prefix=OFF

; The simplified TypeInfo has the same 18 fields as the Cangjie ABI. All
; candidates use zeroinitializer except the deliberately mismatched Bad pair.
%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, i8*, i8*, i8* }
%"ObjLayout.pkg:Temp<Int64>" = type { [8 x i8] }
%"ObjLayout.pkg:Temp<IntNative>" = type { [8 x i8] }
%"ObjLayout.pkg:Temp<Int32>" = type { [8 x i8] }
%"ObjLayout.pkg:Temp<Rune>" = type { [8 x i8] }
%"ObjLayout.pkg:Temp<UInt64>" = type { [8 x i8] }
%"ObjLayout.pkg:Temp<UIntNative>" = type { [8 x i8] }
%"ObjLayout.pkg:Unsafe<Int32>" = type { [8 x i8] }
%"ObjLayout.pkg:Unsafe<Rune>" = type { [8 x i8] }
%"ObjLayout.pkg:Bad<UInt64>" = type { [8 x i8] }
%"ObjLayout.pkg:Bad<UIntNative>" = type { [8 x i8] }

@Int64.ti = external global %TypeInfo
@IntNative.ti = external global %TypeInfo
@Int32.ti = external global %TypeInfo
@Rune.ti = external global %TypeInfo
@UInt64.ti = external global %TypeInfo
@UIntNative.ti = external global %TypeInfo

@"RawArray<Int64>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<IntNative>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<Int32>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<Rune>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<UInt64>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<UIntNative>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Temp<Int64>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Temp<IntNative>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Temp<Int32>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Temp<Rune>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Temp<UInt64>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Temp<UIntNative>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Unsafe<Int32>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"pkg:Unsafe<Rune>.ti" = linkonce_odr global %TypeInfo zeroinitializer

; Different offsets contents must never be canonicalized.
@BadOffsets64 = private constant [3 x i32] [i32 0, i32 24, i32 32]
@BadOffsetsNative = private constant [3 x i32] [i32 0, i32 24, i32 64]
@"pkg:Bad<UInt64>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 0, i8 0, i16 0, i32 0, i8* null, i32 0, i8 0, i8 0, i16 0, i32* getelementptr inbounds ([3 x i32], [3 x i32]* @BadOffsets64, i32 0, i32 0), i8* null, i8* null, i8* null, %TypeInfo* null, i8* null, i8* null, i8* null }
@"pkg:Bad<UIntNative>.ti" = linkonce_odr global %TypeInfo { i8* null, i8 0, i8 0, i16 0, i32 0, i8* null, i32 0, i8 0, i8 0, i16 0, i32* getelementptr inbounds ([3 x i32], [3 x i32]* @BadOffsetsNative, i32 0, i32 0), i8* null, i8* null, i8* null, %TypeInfo* null, i8* null, i8* null, i8* null }

; These represent reflection/type-registration records. They must retain the
; original primitive and composite TypeInfo pointers, not canonical partners.
@ReflectionTypes = global [6 x %TypeInfo*] [%TypeInfo* @IntNative.ti, %TypeInfo* @UIntNative.ti, %TypeInfo* @Rune.ti, %TypeInfo* @"RawArray<IntNative>.ti", %TypeInfo* @"RawArray<UIntNative>.ti", %TypeInfo* @"RawArray<Rune>.ti"]
@ThunkMetadata = global void()* @h_intnative_thunk

declare i8* @CJ_MCC_NewArray64(i8*, i64, i64)
declare void @use(i8*)

; Group 1: IntNative -> Int64.
define internal void @a_int64_impl() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 1, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int64>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

define internal void @b_intnative_impl() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 1, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

; Group 2: Rune -> Int32.
define internal void @c_int32_impl() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int32>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int32>.ti" to i8*), i64 2, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int32>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int32>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

define internal void @d_rune_impl() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Rune>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Rune>.ti" to i8*), i64 2, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Rune>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Rune>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

; Group 3: UIntNative -> UInt64.
define internal void @e_uint64_impl() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<UInt64>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UInt64>.ti" to i8*), i64 3, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<UInt64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<UInt64>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

define internal void @f_uintnative_impl() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<UIntNative>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UIntNative>.ti" to i8*), i64 3, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<UIntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<UIntNative>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

; An address-taken entry must survive as a forwarding thunk with its original
; name and signature, because ThunkMetadata still refers to it.
define internal void @g_int64_thunk() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 4, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<Int64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<Int64>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

define internal void @h_intnative_thunk() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 4, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Temp<IntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

; A header load can feed a type test or reflection. Its presence disables the
; stack rewrite, so the Rune and Int32 bodies remain distinct.
define internal void @i_int32_unsafe() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Unsafe<Int32>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int32>.ti" to i8*), i64 5, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Unsafe<Int32>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Unsafe<Int32>.ti" to i8*), i8** %header
  %ti = load i8*, i8** %header
  call void @use(i8* %raw)
  call void @use(i8* %ti)
  ret void
}

define internal void @j_rune_unsafe() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Unsafe<Rune>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Rune>.ti" to i8*), i64 5, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Unsafe<Rune>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Unsafe<Rune>.ti" to i8*), i8** %header
  %ti = load i8*, i8** %header
  call void @use(i8* %raw)
  call void @use(i8* %ti)
  ret void
}

; Name equivalence alone is insufficient when runtime layouts differ.
define internal void @k_uint64_bad() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Bad<UInt64>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UInt64>.ti" to i8*), i64 6, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Bad<UInt64>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Bad<UInt64>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

define internal void @l_uintnative_bad() {
entry:
  %obj = alloca { %TypeInfo*, %"ObjLayout.pkg:Bad<UIntNative>" }, !ea_val !0
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UIntNative>.ti" to i8*), i64 6, i64 0)
  %header = bitcast { %TypeInfo*, %"ObjLayout.pkg:Bad<UIntNative>" }* %obj to i8**
  store i8* bitcast (%TypeInfo* @"pkg:Bad<UIntNative>.ti" to i8*), i8** %header
  call void @use(i8* %raw)
  ret void
}

define void @call_direct() {
entry:
  call void @a_int64_impl()
  call void @b_intnative_impl()
  call void @c_int32_impl()
  call void @d_rune_impl()
  call void @e_uint64_impl()
  call void @f_uintnative_impl()
  call void @g_int64_thunk()
  call void @h_intnative_thunk()
  call void @i_int32_unsafe()
  call void @j_rune_unsafe()
  call void @k_uint64_bad()
  call void @l_uintnative_bad()
  ret void
}

!0 = !{}

; CHECK: @ReflectionTypes = global [6 x %TypeInfo*] [%TypeInfo* @IntNative.ti, %TypeInfo* @UIntNative.ti, %TypeInfo* @Rune.ti, %TypeInfo* @"RawArray<IntNative>.ti", %TypeInfo* @"RawArray<UIntNative>.ti", %TypeInfo* @"RawArray<Rune>.ti"]
; CHECK: @ThunkMetadata = global void ()* @h_intnative_thunk
; CHECK: define internal void @a_int64_impl()
; CHECK-NOT: define internal void @b_intnative_impl()
; CHECK: define internal void @c_int32_impl()
; CHECK-NOT: define internal void @d_rune_impl()
; CHECK: define internal void @e_uint64_impl()
; CHECK-NOT: define internal void @f_uintnative_impl()
; CHECK: define internal void @g_int64_thunk()
; CHECK: define internal void @i_int32_unsafe()
; CHECK: define internal void @j_rune_unsafe()
; CHECK: store i8* bitcast (%TypeInfo* @"pkg:Unsafe<Rune>.ti" to i8*)
; CHECK: define internal void @k_uint64_bad()
; CHECK: define internal void @l_uintnative_bad()
; CHECK: store i8* bitcast (%TypeInfo* @"pkg:Bad<UIntNative>.ti" to i8*)
; CHECK: define void @call_direct()
; CHECK: call void @a_int64_impl()
; CHECK-NOT: call void @b_intnative_impl()
; CHECK: call void @c_int32_impl()
; CHECK-NOT: call void @d_rune_impl()
; CHECK: call void @e_uint64_impl()
; CHECK-NOT: call void @f_uintnative_impl()
; CHECK: define internal void @h_intnative_thunk() {
; CHECK-NEXT: call void @g_int64_thunk()
; CHECK-NEXT: ret void

; OFF: define internal void @b_intnative_impl()
; OFF: store i8* bitcast (%TypeInfo* @"pkg:Temp<IntNative>.ti" to i8*)
; OFF: define internal void @d_rune_impl()
; OFF: define internal void @f_uintnative_impl()
