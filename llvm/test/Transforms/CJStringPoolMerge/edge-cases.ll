; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
; RUN: opt -passes=cj-string-pool-merge -cj-string-pool-merge-verbose -disable-output %s 2>&1 | FileCheck %s --check-prefix=WARN
; RUN: opt -passes=cj-string-pool-merge -cj-string-pool-merge=false -S < %s | FileCheck %s --check-prefix=NOMERGE
;
; Edge cases around buffer collection and repointing:
;  - buffers with a direct instruction user, a non-cast/GEP constant user,
;    a non-literal global user, or a constant whose user is an instruction
;    are left untouched;
;  - an attribute-marked non-record global is skipped by repointLiteral, so
;    its buffer is kept (with a warning) instead of being erased;
;  - an empty buffer merges at base 0; a buffer with explicit alignment
;    propagates it to the merged pool; a buffer with no users is ignored;
;  - a mutable (non-constant, unmarked) String global is repointed but stays
;    non-constant;
;  - with -cj-string-pool-merge=false nothing is merged and the literal
;    records are still flipped back to constant.
; CHECK-NOT: $const_cjstring_data.aaaa
; CHECK-NOT: $const_cjstring_data.bbbb
; CHECK-NOT: $const_cjstring_data.empty
; CHECK-DAG: @"$const_cjstring_data.dead" = private constant
; CHECK-DAG: @"$const_cjstring_data.merged" = private constant { i8*, i64, [8 x i8] } { i8* getelementptr inbounds (%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 8, [8 x i8] c"abcabdzz" }, align 8
; CHECK-DAG: @"$const_cjstring.l_a" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }
; CHECK-DAG: @"$const_cjstring.l_b" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 3, i32 3 }
; CHECK-DAG: @"$const_cjstring.l_empty" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 0, i32 0 }
; CHECK-DAG: @mut_str = global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [8 x i8] }* @"$const_cjstring_data.merged" to i8*) to i8 addrspace(1)*), i32 3, i32 3 }
; CHECK-DAG: @"$const_cjstring_data.dupdup" = private constant { i8*, i64, [2 x i8] }
; CHECK-DAG: @agg = global [2 x i8*]
; CHECK-DAG: @sb_like = global %"record.std.core:StringBuilder"
; CHECK-DAG: @"$const_cjstring_data.inst" = private constant
; CHECK-DAG: @"$const_cjstring_data.badce" = private constant
; CHECK-DAG: @"$const_cjstring_data.nongv" = private constant
; CHECK-DAG: @"$const_cjstring_data.ceinst" = private constant
; CHECK-NOT: $const_cjstring_data.aaaa
; CHECK-NOT: $const_cjstring_data.bbbb
; CHECK-NOT: $const_cjstring_data.empty
target triple = "aarch64--linux-gnu"

%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }
; A hypothetical std.core struct whose name only *starts with* "String";
; the pass must match the String record by exact name, not by prefix.
%"record.std.core:StringBuilder" = type { i8 addrspace(1)*, i32, i32 }
%TypeInfo = type { i8 }
@"RawArray<UInt8>.ti" = external global %TypeInfo

; Packing: "abc"@0, "abd"@3, "zz"@6 (tie broken lexicographically, no
; suffix/prefix overlaps). The empty buffer contributes no bytes, base 0.
; Merged buffers are erased; only .dupdup (unrepointable user), the
; rejected buffers and the user-less .dead remain.
; NOMERGE-NOT: $const_cjstring_data.merged
@"$const_cjstring_data.aaaa" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"abc" }, align 8 #0
@"$const_cjstring_data.bbbb" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"abd" } #0
@"$const_cjstring_data.dupdup" = private constant { i8*, i64, [2 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 2, [2 x i8] c"zz" } #0
@"$const_cjstring_data.empty" = private constant { i8*, i64, [0 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 0, [0 x i8] zeroinitializer } #0

; A marked buffer with no users at all is ignored (not merged, not erased).
@"$const_cjstring_data.dead" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"ded" } #0

; Merged buffers are repointed and flipped back to constant.
@"$const_cjstring.l_a" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.aaaa" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1
@"$const_cjstring.l_b" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.bbbb" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1
@"$const_cjstring.l_empty" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [0 x i8] }* @"$const_cjstring_data.empty" to i8*) to i8 addrspace(1)*), i32 0, i32 0 } #1

; A mutable String global (record type, no attribute) is repointed like a
; literal but must stay non-constant.
; NOMERGE-DAG: @mut_str = global %"record.std.core:String"
; NOMERGE-DAG: @sb_like = global %"record.std.core:StringBuilder"
@mut_str = global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.bbbb" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }

; An attribute-marked global that is NOT a String record cannot be repointed
; by the record rewrite; its buffer is kept (with a warning) rather than
; erased. The aggregate below also references the same cast twice, exercising
; the de-duplication of the constant-user walk.
; WARN: CJStringPoolMerge: $const_cjstring_data.dupdup still has users after merging, kept (duplicate!)
@agg = global [2 x i8*] [ i8* bitcast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.dupdup" to i8*), i8* bitcast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.dupdup" to i8*) ] #1

; Attribute-marked but of a non-String-record type: must never be flipped to
; constant (a prefix match on the type name would wrongly accept it).
@sb_like = global %"record.std.core:StringBuilder" zeroinitializer #1

; Buffers with any of these users are not mergeable and stay untouched:
; a direct instruction user, a non-cast/GEP constant expression, a
; non-literal global user, and a constant expression used by an instruction.
@"$const_cjstring_data.inst" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"ins" } #0
@"$const_cjstring_data.badce" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"bad" } #0
@"$const_cjstring_data.nongv" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"oth" } #0
@"$const_cjstring_data.ceinst" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"cei" } #0
@arith_user = global i64 add (i64 ptrtoint ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.badce" to i64), i64 4)
@plain_user = global i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.nongv" to i8*)

define i8 @load_from_inst() {
; CHECK-LABEL: @load_from_inst(
  %p = getelementptr { i8*, i64, [3 x i8] }, { i8*, i64, [3 x i8] }* @"$const_cjstring_data.inst", i32 0, i32 2, i32 0
  %v = load i8, i8* %p
  ret i8 %v
}

define i8* @ret_ceinst() {
; CHECK-LABEL: @ret_ceinst(
  ret i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.ceinst" to i8*)
}

; With merging disabled, the literal records are still flipped back to
; constant and every original buffer stays. Non-record globals that merely
; carry the cjstring_literal attribute are not flipped in either mode.
; NOMERGE-DAG: @agg = global [2 x i8*]
; NOMERGE-DAG: @"$const_cjstring.l_a" = private constant %"record.std.core:String"
; NOMERGE-DAG: @"$const_cjstring.l_b" = private constant %"record.std.core:String"
; NOMERGE-DAG: @"$const_cjstring.l_empty" = private constant %"record.std.core:String"
; NOMERGE-DAG: @"$const_cjstring_data.aaaa" = private constant
; NOMERGE-DAG: @"$const_cjstring_data.bbbb" = private constant

attributes #0 = { "cjstring_deferred" }
attributes #1 = { "cjstring_literal" }
