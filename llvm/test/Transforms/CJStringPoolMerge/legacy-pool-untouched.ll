; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; A legacy per-package pool (literals with offset != 0 / len != pool data
; length) in the same module MUST be left completely untouched: treating it
; as a per-literal buffer would rewrite its first literal to span the whole
; pool (silent string corruption). The two genuine per-literal pairs are
; still merged.

%TypeInfo = type { i8 }
%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }

@"RawArray<UInt8>.ti" = private constant %TypeInfo zeroinitializer

; Legacy pool: one 8-byte blob, two literals with non-zero offsets.
@"$const_cjstring_data.legacy" = private constant { i8*, i64, [8 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 8, [8 x i8] c"ABCDEFGH" } #0
@"$const_cjstring.legacy0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.legacy" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1
@"$const_cjstring.legacy1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.legacy" to i8*) to i8 addrspace(1)*), i32 3, i32 5 } #1

; Deferred per-literal pairs.
@"$const_cjstring_data.0" = private constant { i8*, i64, [2 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 2, [2 x i8] c"XY" } #0
@"$const_cjstring.0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i32 0, i32 2 } #1
@"$const_cjstring_data.1" = private constant { i8*, i64, [1 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 1, [1 x i8] c"Y" } #0
@"$const_cjstring.1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [1 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i32 0, i32 1 } #1

; Legacy pool and its literals byte-for-byte untouched.
; CHECK: @"$const_cjstring_data.legacy" = private constant { i8*, i64, [8 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 8, [8 x i8] c"ABCDEFGH" } #[[DATAATTR:[0-9]+]]
; CHECK: @"$const_cjstring.legacy0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.legacy" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #[[LITATTR:[0-9]+]]
; CHECK: @"$const_cjstring.legacy1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.legacy" to i8*) to i8 addrspace(1)*), i32 3, i32 5 } #[[LITATTR]]
; Old deferred buffers are erased (they would appear here if not).
; CHECK-NOT: @"$const_cjstring_data.0"
; CHECK-NOT: @"$const_cjstring_data.1"
; Deferred pairs merged into a new pool ("Y" shares "XY"'s tail at offset 1).
; CHECK: @"$const_cjstring.0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i32 0, i32 2 } #[[LITATTR]]
; CHECK: @"$const_cjstring.1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i32 1, i32 1 } #[[LITATTR]]
; CHECK: @"$const_cjstring_data.merged.0" = private constant { i8*, i64, [2 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 2, [2 x i8] c"XY" } #[[DATAATTR]]

attributes #0 = { "cjstring_data" }
attributes #1 = { "cjstring_literal" }
