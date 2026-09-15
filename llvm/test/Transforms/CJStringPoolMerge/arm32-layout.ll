; RUN: opt -passes=cj-string-pool-merge -cj-string-pool-merge-verbose -S %s 2>&1 | FileCheck %s --check-prefix=AAPCS
; RUN: sed 's/i64:64/i64:32/' %s | opt -passes=cj-string-pool-merge -cj-string-pool-merge-verbose -S 2>&1 | FileCheck %s --check-prefix=ILP32
;
; arm is the only non-64-bit cangjie backend. The pass must merge there too,
; and the verbose byte counts must come from the target DataLayout rather than
; from a hard-coded 64-bit header:
;  - AAPCS arm32 keeps i64 8-aligned (ARMTargetMachine.cpp:164), so the
;    {i8*, i64, [N x i8]} buffer header is 16 and the counts match the LP64
;    numbers: 2 * (3 + 16) = 38 and 6 + 16 = 22;
;  - a 4-aligned i64 layout gives a 12-byte header, so the same two buffers
;    must report 2 * (3 + 12) = 30 and 6 + 12 = 18. If the header were ever
;    hard-coded to 16 this run would fail.
; AAPCS-DAG: CJStringPoolMerge: buffers=2 merged=2 erased=2 bytes 38 -> 22
; ILP32-DAG: CJStringPoolMerge: buffers=2 merged=2 erased=2 bytes 30 -> 18
; Both layouts merge the two buffers into one pool and repoint the literals
; (all DAG so the stdout/stderr interleaving of the 2>&1 pipe does not matter).
; AAPCS-DAG: @"$const_cjstring_data.merged" = private constant { i8*, i64, [6 x i8] } { i8* null, i64 6, [6 x i8] c"barfoo" }
; AAPCS-DAG: @"$const_cjstring.l1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 3, i32 3 }
; AAPCS-DAG: @"$const_cjstring.l2" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }
; ILP32-DAG: @"$const_cjstring_data.merged" = private constant { i8*, i64, [6 x i8] } { i8* null, i64 6, [6 x i8] c"barfoo" }
; ILP32-DAG: @"$const_cjstring.l1" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 3, i32 3 }
; ILP32-DAG: @"$const_cjstring.l2" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }
target datalayout = "e-m:e-p:32:32-Fi8-i64:64-v128:64:128-a:0:32-n32-S64"
target triple = "armv7-unknown-linux-gnueabihf"

%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }

@"$const_cjstring_data.aaaa" = private constant { i8*, i64, [3 x i8] } { i8* null, i64 3, [3 x i8] c"foo" } #0
@"$const_cjstring_data.bbbb" = private constant { i8*, i64, [3 x i8] } { i8* null, i64 3, [3 x i8] c"bar" } #0

@"$const_cjstring.l1" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.aaaa" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1
@"$const_cjstring.l2" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.bbbb" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1

define i32 @use_lit() {
  %a = load i32, i32* getelementptr (%"record.std.core:String", %"record.std.core:String"* @"$const_cjstring.l1", i32 0, i32 2)
  %b = load i32, i32* getelementptr (%"record.std.core:String", %"record.std.core:String"* @"$const_cjstring.l2", i32 0, i32 2)
  %s = add i32 %a, %b
  ret i32 %s
}

attributes #0 = { "cjstring_deferred" }
attributes #1 = { "cjstring_literal" }
