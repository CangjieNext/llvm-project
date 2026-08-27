; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; Two per-literal buffer/literal pairs (one content a substring of the
; other, plus an empty string) are merged into a single shared pool with
; substring sharing; literals are repointed to {pool, offset, length} and
; the old buffers are erased.

%TypeInfo = type { i8 }
%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }

@"RawArray<UInt8>.ti" = private constant %TypeInfo zeroinitializer

@"$const_cjstring_data.0" = private constant { i8*, i64, [4 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 4, [4 x i8] c"ABCD" } #0
@"$const_cjstring.0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i32 0, i32 4 } #1

@"$const_cjstring_data.1" = private constant { i8*, i64, [2 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 2, [2 x i8] c"CD" } #0
@"$const_cjstring.1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i32 0, i32 2 } #1

@"$const_cjstring_data.2" = private constant { i8*, i64, [0 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 0, [0 x i8] zeroinitializer } #0
@"$const_cjstring.2" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [0 x i8] }* @"$const_cjstring_data.2" to i8*) to i8 addrspace(1)*), i32 0, i32 0 } #1

; CHECK-NOT: @"$const_cjstring_data.0"
; CHECK-NOT: @"$const_cjstring_data.1"
; CHECK-NOT: @"$const_cjstring_data.2"
; CHECK: @"$const_cjstring.0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i32 0, i32 4 } #[[LITATTR:[0-9]+]]
; CHECK: @"$const_cjstring.1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i32 2, i32 2 } #[[LITATTR]]
; CHECK: @"$const_cjstring.2" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i32 0, i32 0 } #[[LITATTR]]
; "ABCD" at offset 0, "CD" shares its tail at offset 2, "" at offset 0.
; CHECK: @"$const_cjstring_data.merged.0" = private constant { i8*, i64, [4 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 4, [4 x i8] c"ABCD" } #[[DATAATTR:[0-9]+]]
; CHECK: attributes #[[LITATTR]] = { "cjstring_literal" }
; CHECK: attributes #[[DATAATTR]] = { "cjstring_data" }

attributes #0 = { "cjstring_data" }
attributes #1 = { "cjstring_literal" }
