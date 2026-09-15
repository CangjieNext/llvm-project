; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; All cjstring buffers emitted by CodeGen are RawArray<UInt8> over the same
; class info object, so one merged pool can carry that class info for every
; string. A deferred buffer whose class info differs cannot be repointed into
; such a pool (the pool would then report the wrong class for its strings), so
; the pass must skip it and leave it untouched: buffer kept, its literal still
; pointing at it and only flipped back to constant.
target triple = "aarch64--linux-gnu"

%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }
%TypeInfo = type { i8 }
@"RawArray<UInt8>.ti" = external global %TypeInfo

; Same class info as the first buffer -> merged into the pool.
@"$const_cjstring_data.main" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"abc" } #0
@"$const_cjstring.l_main" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.main" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1

; Different class info -> skipped by collectBuffers, must survive untouched.
@"$const_cjstring_data.odd" = private constant { i8*, i64, [3 x i8] } { i8* null, i64 3, [3 x i8] c"odd" } #0
@"$const_cjstring.l_odd" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.odd" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1

; CHECK-DAG: @"$const_cjstring_data.merged" = private constant { i8*, i64, [3 x i8] } { i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 3, [3 x i8] c"abc" }
; CHECK-DAG: @"$const_cjstring_data.odd" = private constant { i8*, i64, [3 x i8] } { i8* null, i64 3, [3 x i8] c"odd" }
; CHECK-DAG: @"$const_cjstring.l_main" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }
; CHECK-DAG: @"$const_cjstring.l_odd" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.odd" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }

attributes #0 = { "cjstring_deferred" }
attributes #1 = { "cjstring_literal" }
