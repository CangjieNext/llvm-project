; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; A buffer with BOTH a genuine per-literal wrapper AND a CastChain direct
; handle: the CastChain handle is an MArray base pointer whose header must
; stay valid (the stdlib's checked arrayGet loads the length field at
; base+8), so the buffer goes to the nested pool (each field a verbatim copy
; with a valid header); the wrapper is repointed to {field, 0, len} and the
; old buffer is erased -- no duplicated content left behind.

%TypeInfo = type { i8 }
%"record.std.core:String" = type { i8 addrspace(1)*, i32, i32 }

@"RawArray<UInt8>.ti" = private constant %TypeInfo zeroinitializer

@"$const_cjstring_data.0" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"abc" } #0
@"$const_cjstring.0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1

@"$const_cjstring_data.1" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"xyz" } #0

declare void @use_ptr(i8 addrspace(1)*)

define void @f() {
entry:
  ; CastChain handles on both buffers.
  call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*))
  call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*))
  ret void
}

; The wrapper is repointed to field 0 as {field, 0, len} (field 0 folds to
; the nested pool base itself).
; CHECK: @"$const_cjstring.0" = private constant %"record.std.core:String" { i8 addrspace(1)* addrspacecast (i8* bitcast ({ { i8*, i64, [3 x i8] }, { i8*, i64, [3 x i8] } }* @"$const_cjstring_data.nested.0" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #[[LITATTR:[0-9]+]]
; Both buffers become fields of the nested pool (verbatim, headers intact).
; CHECK: @"$const_cjstring_data.nested.0" = private constant { { i8*, i64, [3 x i8] }, { i8*, i64, [3 x i8] } } { { i8*, i64, [3 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 3, [3 x i8] c"abc" }, { i8*, i64, [3 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 3, [3 x i8] c"xyz" } } #[[DATAATTR:[0-9]+]]
; Handles repointed to field bases (valid headers).
; CHECK: call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ { i8*, i64, [3 x i8] }, { i8*, i64, [3 x i8] } }* @"$const_cjstring_data.nested.0" to i8*) to i8 addrspace(1)*))
; CHECK: call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* getelementptr inbounds ({ { i8*, i64, [3 x i8] }, { i8*, i64, [3 x i8] } }, { { i8*, i64, [3 x i8] }, { i8*, i64, [3 x i8] } }* @"$const_cjstring_data.nested.0", i32 0, i32 1) to i8*) to i8 addrspace(1)*))
; Old buffers erased (no duplicated content).
; CHECK-NOT: @"$const_cjstring_data.0"
; CHECK-NOT: @"$const_cjstring_data.1"
; CHECK: attributes #[[LITATTR]] = { "cjstring_literal" }
; CHECK: attributes #[[DATAATTR]] = { "cjstring_data" }

attributes #0 = { "cjstring_data" }
attributes #1 = { "cjstring_literal" }
