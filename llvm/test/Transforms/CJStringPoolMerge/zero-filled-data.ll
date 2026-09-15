; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; Constant uniquing folds an all-zero data array to zeroinitializer while
; keeping the element count, so CodeGen's buffers for "" and for a
; single-NUL-byte literal arrive as a 0-element and a 1-element
; [N x i8] zeroinitializer. Both carry real string bytes (zero of them, or one
; NUL) and must merge like any other deferred buffer; leaving the 1-element
; form behind would keep a standalone object and its class-info relocation.
target triple = "aarch64--linux-gnu"

%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }
%TypeInfo = type { i8 }
@"RawArray<UInt8>.ti" = external global %TypeInfo

; Packing order is longest-first, so "xy" lands first and the NUL byte follows:
;   "xy"(2) "\0"(1) ""(0)  ->  merged pool = "xy\0", bases 0, 2 and 0.
; CHECK-NOT: $const_cjstring_data.xy
; CHECK-NOT: $const_cjstring_data.nul
; CHECK-NOT: $const_cjstring_data.empty
@"$const_cjstring_data.xy" = private constant { i8*, i64, [2 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 2, [2 x i8] c"xy" } #0
@"$const_cjstring_data.nul" = private constant { i8*, i64, [1 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 1, [1 x i8] zeroinitializer } #0
@"$const_cjstring_data.empty" = private constant { i8*, i64, [0 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 0, [0 x i8] zeroinitializer } #0

; The literal records (non-constant, "cjstring_literal") are repointed and
; flipped back to constant.
; CHECK-DAG: @"$const_cjstring.l_xy" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 2 }
; CHECK-DAG: @"$const_cjstring.l_nul" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 2, i32 1 }
; CHECK-DAG: @"$const_cjstring.l_empty" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 0 }
@"$const_cjstring.l_xy" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.xy" to i8*) to i8 addrspace(1)*), i32 0, i32 2 } #1
@"$const_cjstring.l_nul" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [1 x i8] }* @"$const_cjstring_data.nul" to i8*) to i8 addrspace(1)*), i32 0, i32 1 } #1
@"$const_cjstring.l_empty" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [0 x i8] }* @"$const_cjstring_data.empty" to i8*) to i8 addrspace(1)*), i32 0, i32 0 } #1

; CHECK: @"$const_cjstring_data.merged" = private constant { i8*, i64, [3 x i8] } { i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 3, [3 x i8] c"xy\00" }

attributes #0 = { "cjstring_deferred" }
attributes #1 = { "cjstring_literal" }
