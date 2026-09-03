; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; Orphan per-literal buffers (their literal wrappers were folded away by the
; per-module opt, leaving only direct ConstantExpr handles used by
; instructions) are merged by content. CastChain handles are MArray base
; pointers whose header must stay valid (the stdlib's checked arrayGet loads
; the length field at base+8), so they go to the nested-struct pool where
; every field is a verbatim buffer copy with a valid header; [0,2,off] GEP
; handles are raw data pointers and go to the byte pool.

%TypeInfo = type { i8 }

@"RawArray<UInt8>.ti" = private constant %TypeInfo zeroinitializer

@"$const_cjstring_data.0" = private constant { i8*, i64, [4 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 4, [4 x i8] c"ABCD" } #0
@"$const_cjstring_data.1" = private constant { i8*, i64, [2 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 2, [2 x i8] c"EF" } #0
@"$const_cjstring_data.2" = private constant { i8*, i64, [2 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 2, [2 x i8] c"GH" } #0
@"$const_cjstring_data.3" = private constant { i8*, i64, [4 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 4, [4 x i8] c"IJKL" } #0

declare void @use_ptr(i8 addrspace(1)*)
declare void @use_data(i8 addrspace(1)*)

define void @f() {
entry:
  ; CastChain handle on buffer 0.
  call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*))
  ; CastChain handle on buffer 3.
  call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.3" to i8*) to i8 addrspace(1)*))
  ; ConstGepIntoData handle on buffer 1: byte 1 of "EF".
  call void @use_data(i8 addrspace(1)* getelementptr inbounds ({ i8*, i64, [2 x i8] }, { i8*, i64, [2 x i8] } addrspace(1)* addrspacecast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.1" to { i8*, i64, [2 x i8] } addrspace(1)*), i32 0, i32 2, i32 1))
  ; ConstGepIntoData handle on buffer 2: byte 0 of "GH".
  call void @use_data(i8 addrspace(1)* getelementptr inbounds ({ i8*, i64, [2 x i8] }, { i8*, i64, [2 x i8] } addrspace(1)* addrspacecast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.2" to { i8*, i64, [2 x i8] } addrspace(1)*), i32 0, i32 2, i32 0))
  ret void
}

; The two ConstGepIntoData buffers merge into one byte pool "EFGH":
; "EF" at data offset 0, "GH" at data offset 2. Buffer bases = pool + 0 / + 2,
; data bytes at base + 16 + dataOff. (The byte pool is built first, hence
; merged.0; the i8-GEP at offset 0 folds away.)
; CHECK: @"$const_cjstring_data.merged.0" = private constant { i8*, i64, [4 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 4, [4 x i8] c"EFGH" } #[[DATAATTR:[0-9]+]]
; The CastChain buffers keep their own headers: they become fields of the
; nested pool (verbatim copies), and the handles are repointed to field bases
; (field 0 folds to the nested pool base itself).
; CHECK: @"$const_cjstring_data.nested.1" = private constant { { i8*, i64, [4 x i8] }, { i8*, i64, [4 x i8] } } { { i8*, i64, [4 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 4, [4 x i8] c"ABCD" }, { i8*, i64, [4 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 4, [4 x i8] c"IJKL" } } #[[DATAATTR]]
; CHECK: call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ { i8*, i64, [4 x i8] }, { i8*, i64, [4 x i8] } }* @"$const_cjstring_data.nested.1" to i8*) to i8 addrspace(1)*))
; CHECK: call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* getelementptr inbounds ({ { i8*, i64, [4 x i8] }, { i8*, i64, [4 x i8] } }, { { i8*, i64, [4 x i8] }, { i8*, i64, [4 x i8] } }* @"$const_cjstring_data.nested.1", i32 0, i32 1) to i8*) to i8 addrspace(1)*))
; CHECK: call void @use_data(i8 addrspace(1)* getelementptr inbounds ({ i8*, i64, [2 x i8] }, { i8*, i64, [2 x i8] } addrspace(1)* addrspacecast ({ i8*, i64, [2 x i8] }* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.merged.0" to { i8*, i64, [2 x i8] }*) to { i8*, i64, [2 x i8] } addrspace(1)*), i32 0, i32 2, i32 1))
; CHECK: call void @use_data(i8 addrspace(1)* getelementptr inbounds ({ i8*, i64, [2 x i8] }, { i8*, i64, [2 x i8] } addrspace(1)* addrspacecast ({ i8*, i64, [2 x i8] }* bitcast (i8* getelementptr inbounds (i8, i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.merged.0" to i8*), i64 2) to { i8*, i64, [2 x i8] }*) to { i8*, i64, [2 x i8] } addrspace(1)*), i32 0, i32 2, i32 0))
; Old buffers erased.
; CHECK-NOT: @"$const_cjstring_data.0"
; CHECK-NOT: @"$const_cjstring_data.1"
; CHECK-NOT: @"$const_cjstring_data.2"
; CHECK-NOT: @"$const_cjstring_data.3"
; CHECK: attributes #[[DATAATTR]] = { "cjstring_data" }

attributes #0 = { "cjstring_data" }
