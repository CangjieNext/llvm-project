; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; Orphan per-literal buffers (their literal wrappers were folded away by the
; per-module opt, leaving only direct ConstantExpr handles used by
; instructions) are merged by content; the handles are repointed by byte
; offset. Buffer 0 is reached via a cast chain (runtime AcquireRawData does
; +16 from the base), buffer 1 via a [0,2,off] GEP into the data array.

%TypeInfo = type { i8 }

@"RawArray<UInt8>.ti" = private constant %TypeInfo zeroinitializer

@"$const_cjstring_data.0" = private constant { i8*, i64, [4 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 4, [4 x i8] c"ABCD" } #0
@"$const_cjstring_data.1" = private constant { i8*, i64, [2 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 2, [2 x i8] c"EF" } #0

declare void @use_ptr(i8 addrspace(1)*)
declare void @use_data(i8 addrspace(1)*)

define void @f() {
entry:
  ; CastChain handle on buffer 0.
  call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [4 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*))
  ; ConstGepIntoData handle on buffer 1: byte 1 of "EF".
  call void @use_data(i8 addrspace(1)* getelementptr inbounds ({ i8*, i64, [2 x i8] }, { i8*, i64, [2 x i8] } addrspace(1)* addrspacecast ({ i8*, i64, [2 x i8] }* @"$const_cjstring_data.1" to { i8*, i64, [2 x i8] } addrspace(1)*), i32 0, i32 2, i32 1))
  ret void
}

; Merged pool "ABCDEF": "ABCD" at data offset 0, "EF" at data offset 4.
; CHECK: @"$const_cjstring_data.merged.0" = private constant { i8*, i64, [6 x i8] } {{.*}}, i64 6, [6 x i8] c"ABCDEF" } #[[DATAATTR:[0-9]+]]
; Buffer 0 CastChain: base = pool + 0 (the i8 GEP folds away at offset 0;
; the runtime's +16 lands on the pool's data start).
; CHECK: call void @use_ptr(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*))
; Buffer 1 GEP: buffer base = pool + 4, data byte = base + 16 + 1.
; CHECK: call void @use_data(i8 addrspace(1)* getelementptr inbounds ({ i8*, i64, [2 x i8] }, { i8*, i64, [2 x i8] } addrspace(1)* addrspacecast ({ i8*, i64, [2 x i8] }* bitcast (i8* getelementptr inbounds (i8, i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*), i64 4) to { i8*, i64, [2 x i8] }*) to { i8*, i64, [2 x i8] } addrspace(1)*), i32 0, i32 2, i32 1))
; Old buffers erased.
; CHECK-NOT: @"$const_cjstring_data.0"
; CHECK-NOT: @"$const_cjstring_data.1"
; CHECK: attributes #[[DATAATTR]] = { "cjstring_data" }

attributes #0 = { "cjstring_data" }
