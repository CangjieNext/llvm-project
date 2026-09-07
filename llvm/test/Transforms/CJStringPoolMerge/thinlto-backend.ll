; RUN: opt -module-summary %s -o %t.bc
; RUN: llvm-lto2 run %t.bc -o %t.o -cangjie-pipeline -cj-string-pool-merge-verbose -r=%t.bc,use_lit,plx 2>&1 | FileCheck %s --check-prefix=VERBOSE
;
; The ThinLTO backend must run CJStringPoolMerge (mirroring the non-LTO
; per-module placement): private cjstring buffers are never imported, so
; each backend merges its own module's buffers. The verbose statistics are
; the observable: two single-string buffers ("bar" and "foo", 3 content
; bytes + 16-byte RawArray header each) become one 6-byte pool (+ header).
;
; VERBOSE: CJStringPoolMerge: buffers=2 merged=2 erased=2 bytes 38 -> 22
target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }

@"$const_cjstring_data.aaaa" = private constant { i8*, i64, [3 x i8] } { i8* null, i64 3, [3 x i8] c"foo" } #0
@"$const_cjstring_data.bbbb" = private constant { i8*, i64, [3 x i8] } { i8* null, i64 3, [3 x i8] c"bar" } #0

@"$const_cjstring.l1" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.aaaa" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1
@"$const_cjstring.l2" = private global %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.bbbb" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1

; Reference both literals so neither is stripped before the merge. Only the
; i32 length fields are read: synthetic modules without the full GC
; scaffolding of real cangjie bc make CJPartialEscapeAnalysis crash on
; GC-pointer flows (it does so even with cj-string-pool-merge disabled), so
; this test deliberately avoids them.
define i32 @use_lit() {
  %a = load i32, i32* getelementptr (%"record.std.core:String", %"record.std.core:String"* @"$const_cjstring.l1", i32 0, i32 2)
  %b = load i32, i32* getelementptr (%"record.std.core:String", %"record.std.core:String"* @"$const_cjstring.l2", i32 0, i32 2)
  %s = add i32 %a, %b
  ret i32 %s
}

attributes #0 = { "cjstring_deferred" }
attributes #1 = { "cjstring_literal" }

!llvm.module.flags = !{!0}
!0 = !{i32 1, !"CJBC", i32 1}
