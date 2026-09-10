; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; Legacy per-package pools (from prebuilt .bc compiled by an older compiler,
; sample taken verbatim from a daily SDK's std/libstd.binary.bc) carry only
; "cjstring_data" and have literals with nonzero start offsets. The pass must
; leave them completely untouched while still merging new deferred-pooling
; buffers (marked "cjstring_deferred") in the same module.
target triple = "aarch64--linux-gnu"

%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }
%TypeInfo = type { i8 }
@"RawArray<UInt8>.ti" = external global %TypeInfo

; ---- legacy sample from a daily SDK (old compiler), must stay unchanged ----
; CHECK: @"$const_cjstring_data.6U6RW-Y-Ln1" = private constant { i8*, i64, [233 x i8] } { i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 233, [233 x i8] c"subaddThe value of the step should not be zero.Overshift: Value of right operand is greater than or equal to the width of left operand!Overshift: Negative shift count!Buffer too small: need at least 1 byte.Buffer size() is too small." }
; CHECK-NEXT: @"$const_cjstring.kH46a81vom" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [233 x i8] }* @"$const_cjstring_data.6U6RW-Y-Ln1" to i8*) to i8 addrspace(1)*), i32 167, i32 39 }
; CHECK-NEXT: @"$const_cjstring.6q4oA0F0DNK" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [233 x i8] }* @"$const_cjstring_data.6U6RW-Y-Ln1" to i8*) to i8 addrspace(1)*), i32 206, i32 12 }
; CHECK-NEXT: @"$const_cjstring.3y-qu3ql2qi" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [233 x i8] }* @"$const_cjstring_data.6U6RW-Y-Ln1" to i8*) to i8 addrspace(1)*), i32 218, i32 15 }
@"$const_cjstring_data.6U6RW-Y-Ln1" = private constant { i8*, i64, [233 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 233, [233 x i8] c"subaddThe value of the step should not be zero.Overshift: Value of right operand is greater than or equal to the width of left operand!Overshift: Negative shift count!Buffer too small: need at least 1 byte.Buffer size() is too small." } #2
@"$const_cjstring.kH46a81vom" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [233 x i8] }* @"$const_cjstring_data.6U6RW-Y-Ln1" to i8*) to i8 addrspace(1)*), i32 167, i32 39 } #1
@"$const_cjstring.6q4oA0F0DNK" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [233 x i8] }* @"$const_cjstring_data.6U6RW-Y-Ln1" to i8*) to i8 addrspace(1)*), i32 206, i32 12 } #1
@"$const_cjstring.3y-qu3ql2qi" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [233 x i8] }* @"$const_cjstring_data.6U6RW-Y-Ln1" to i8*) to i8 addrspace(1)*), i32 218, i32 15 } #1

; ---- new deferred-pooling buffers in the same module, must merge ----
@"$const_cjstring_data.aaaa" = private constant { i8*, i64, [5 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 5, [5 x i8] c"Alpha" } #0
@"$const_cjstring_data.bbbb" = private constant { i8*, i64, [5 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 5, [5 x i8] c"Alpha" } #0
; CHECK-NOT: $const_cjstring_data.aaaa
; CHECK-NOT: $const_cjstring_data.bbbb
; CHECK-NEXT: @"$const_cjstring.l_new1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 5 }
; CHECK-NEXT: @"$const_cjstring.l_new2" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 5 }
@"$const_cjstring.l_new1" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.aaaa" to i8*) to i8 addrspace(1)*), i32 0, i32 5 } #1
@"$const_cjstring.l_new2" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [5 x i8] }* @"$const_cjstring_data.bbbb" to i8*) to i8 addrspace(1)*), i32 0, i32 5 } #1

; CHECK-NEXT: @"$const_cjstring_data.merged" = private constant { i8*, i64, [5 x i8] } { i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 5, [5 x i8] c"Alpha" }

attributes #0 = { "cjstring_deferred" }
attributes #1 = { "cjstring_literal" }
attributes #2 = { "cjstring_data" }
