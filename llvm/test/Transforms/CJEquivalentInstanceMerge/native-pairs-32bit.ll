; RUN: opt --cangjie-pipeline -passes=cj-equivalent-instance-merge -S < %s | FileCheck %s

; Native-width pairs are not equivalent on a 32-bit target, even if a malformed
; or simplified module happens to give their TypeInfo globals equal fields.
target datalayout = "e-m:e-p:32:32-i64:64-n8:16:32-S128"
target triple = "i386-unknown-linux-gnu"

%TypeInfo = type { i8*, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8, i8 }
@Int64.ti = external global %TypeInfo
@IntNative.ti = external global %TypeInfo
@UInt64.ti = external global %TypeInfo
@UIntNative.ti = external global %TypeInfo
@"RawArray<Int64>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<IntNative>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<UInt64>.ti" = linkonce_odr global %TypeInfo zeroinitializer
@"RawArray<UIntNative>.ti" = linkonce_odr global %TypeInfo zeroinitializer

declare i8* @CJ_MCC_NewArray64(i8*, i64, i64)
declare void @use(i8*)

define internal void @a_int64_32bit() {
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*), i64 1, i64 0)
  call void @use(i8* %raw)
  ret void
}

define internal void @b_intnative_32bit() {
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*), i64 1, i64 0)
  call void @use(i8* %raw)
  ret void
}

define internal void @c_uint64_32bit() {
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UInt64>.ti" to i8*), i64 2, i64 0)
  call void @use(i8* %raw)
  ret void
}

define internal void @d_uintnative_32bit() {
  %raw = call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UIntNative>.ti" to i8*), i64 2, i64 0)
  call void @use(i8* %raw)
  ret void
}

; CHECK: define internal void @a_int64_32bit()
; CHECK: call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<Int64>.ti" to i8*)
; CHECK: define internal void @b_intnative_32bit()
; CHECK: call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<IntNative>.ti" to i8*)
; CHECK: define internal void @c_uint64_32bit()
; CHECK: call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UInt64>.ti" to i8*)
; CHECK: define internal void @d_uintnative_32bit()
; CHECK: call i8* @CJ_MCC_NewArray64(i8* bitcast (%TypeInfo* @"RawArray<UIntNative>.ti" to i8*)
