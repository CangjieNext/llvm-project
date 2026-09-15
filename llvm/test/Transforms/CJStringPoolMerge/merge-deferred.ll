; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; Deferred-pooling buffers (marked with "cjstring_deferred") are merged into
; one shared pool; identical contents, substrings and suffix/prefix overlaps
; share storage. Literals are repointed to {merged, base, len} and flipped
; back to constant.
target triple = "aarch64--linux-gnu"

%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }
%TypeInfo = type { i8 }
@"RawArray<UInt8>.ti" = external global %TypeInfo

; Packing order is longest-first, ties lexicographic:
;   "hello"(5) "hello"(5) "lloWX"(5) "world"(5) "ell"(3)
; merged pool = "helloWXworld": hello@0, dup@0, lloWX@2 (overlap 3),
; world@7, ell@1 (substring).
; The original per-string buffers must be gone.
; CHECK-NOT: $const_cjstring_data.aaaa
; CHECK-NOT: $const_cjstring_data.bbbb
; CHECK-NOT: $const_cjstring_data.cccc
; CHECK-NOT: $const_cjstring_data.dddd
; CHECK-NOT: $const_cjstring_data.eeee
@"$const_cjstring_data.aaaa" = private constant { i8*, i64, [5 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 5, [5 x i8] c"hello" } #0
@"$const_cjstring_data.bbbb" = private constant { i8*, i64, [5 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 5, [5 x i8] c"hello" } #0
@"$const_cjstring_data.cccc" = private constant { i8*, i64, [5 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 5, [5 x i8] c"lloWX" } #0
@"$const_cjstring_data.dddd" = private constant { i8*, i64, [5 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 5, [5 x i8] c"world" } #0
@"$const_cjstring_data.eeee" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"ell" } #0

; The literal records (non-constant, "cjstring_literal") are repointed and
; flipped back to constant.
; CHECK-DAG: @"$const_cjstring.l_hello" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [12 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 5 }
; CHECK-DAG: @"$const_cjstring.l_dup" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [12 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 5 }
; CHECK-DAG: @"$const_cjstring.l_overlap" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [12 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 2, i32 5 }
; CHECK-DAG: @"$const_cjstring.l_world" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [12 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 7, i32 5 }
; CHECK-DAG: @"$const_cjstring.l_sub" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [12 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 1, i32 3 }
@"$const_cjstring.l_hello" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.aaaa" to i8*) to i8 addrspace(1)*), i32 0, i32 5 } #1
@"$const_cjstring.l_dup" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.bbbb" to i8*) to i8 addrspace(1)*), i32 0, i32 5 } #1
@"$const_cjstring.l_overlap" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.cccc" to i8*) to i8 addrspace(1)*), i32 0, i32 5 } #1
@"$const_cjstring.l_world" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.dddd" to i8*) to i8 addrspace(1)*), i32 0, i32 5 } #1
@"$const_cjstring.l_sub" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.eeee" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1

; CHECK: @"$const_cjstring_data.merged" = private constant { i8*, i64, [12 x i8] } { i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 12, [12 x i8] c"helloWXworld" }

attributes #0 = { "cjstring_deferred" }
attributes #1 = { "cjstring_literal" }
