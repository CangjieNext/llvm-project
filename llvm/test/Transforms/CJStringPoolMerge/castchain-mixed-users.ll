; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; CastChain handles with mixed user kinds, all byte-pool repointable:
;  - plain-store String materialization (paired offset store fixup)
;  - raw-data accessor calls (mid-pool repoint, +16 lands on data)
;  - a named String record global ({handle, 0, len} -> {pool, poolOff, len})
;  - gcwrite with a variable-index slot (structural slot+8 pairing)

%TypeInfo = type { i8 }
%StringRec = type { i8 addrspace(1)*, i32, i32 }
%Layout = type { i8 addrspace(1)*, [4 x %StringRec] }

@"RawArray<UInt8>.ti" = private constant %TypeInfo zeroinitializer

@"$const_cjstring_data.0" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"ABC" } #0
@"$const_cjstring_data.1" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"DEF" } #0

; Literal wrapper for buffer 0 (pure indirection).
@"$const_cjstring.0" = private constant %StringRec { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i32 0, i32 3 } #1

; Named String constant referencing buffer 1: {handle, off=0, len=3}.
@MYSTR = constant %StringRec { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }

declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8* @CJ_MCC_AcquireRawData(i8 addrspace(1)*, i8*)
declare void @CJ_MCC_ReleaseRawData(i8 addrspace(1)*, i8*, i32)

; Raw-data accessor users of buffer 1: repointed mid-pool (pool base + 3).
define i8* @raw() gc "cangjie" {
entry:
  %p = call i8* @CJ_MCC_AcquireRawData(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i8* null)
  call void @CJ_MCC_ReleaseRawData(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i8* %p, i32 0)
  ret i8* %p
}

; Plain-store materialization of buffer 1 at slot+0, packed store at slot+8.
define void @plainstore(i8 addrspace(1)* %arr) gc "cangjie" {
entry:
  %slot = getelementptr i8, i8 addrspace(1)* %arr, i64 48
  %slotc = bitcast i8 addrspace(1)* %slot to i8 addrspace(1)* addrspace(1)*
  store i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* addrspace(1)* %slotc, align 8
  %offp = getelementptr i8, i8 addrspace(1)* %arr, i64 56
  %offc = bitcast i8 addrspace(1)* %offp to i64 addrspace(1)*
  store i64 12884901888, i64 addrspace(1)* %offc, align 8
  ret void
}

; Variable-index gcwrite materialization of buffer 1: slot and its packed
; store share the same variable element index, last field 0 vs 1 (8 bytes).
define void @varidx(%Layout addrspace(1)* %arr, i64 %i) gc "cangjie" {
entry:
  %slot = getelementptr %Layout, %Layout addrspace(1)* %arr, i64 0, i32 1, i64 %i, i32 0
  %arrc = bitcast %Layout addrspace(1)* %arr to i8 addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* %arrc, i8 addrspace(1)* addrspace(1)* %slot)
  %pk = getelementptr %Layout, %Layout addrspace(1)* %arr, i64 0, i32 1, i64 %i, i32 1
  %pkc = bitcast i32 addrspace(1)* %pk to i64 addrspace(1)*
  store i64 12884901888, i64 addrspace(1)* %pkc, align 8
  ret void
}

; Merged pool "ABCDEF": "ABC" at data offset 0, "DEF" at data offset 3.
; CHECK: @"$const_cjstring.0" = private constant %StringRec { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i32 0, i32 3 }
; Named global record: {poolBase, off=3, len=3}.
; CHECK: @MYSTR = constant %StringRec { i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i32 3, i32 3 }
; CHECK: @"$const_cjstring_data.merged.0" = private constant { i8*, i64, [6 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 6, [6 x i8] c"ABCDEF" } #[[DATAATTR:[0-9]+]]
; Raw-data accessors: mid-pool handle (pool base + offset 3, +16 -> "DEF").
; CHECK: call i8* @CJ_MCC_AcquireRawData(i8 addrspace(1)* addrspacecast (i8* getelementptr inbounds (i8, i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*), i64 3) to i8 addrspace(1)*), i8* null)
; CHECK: call void @CJ_MCC_ReleaseRawData(i8 addrspace(1)* addrspacecast (i8* getelementptr inbounds (i8, i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*), i64 3) to i8 addrspace(1)*), i8* %p, i32 0)
; Plain store: pool base handle, packed store rewritten 0x300000000 ->
; 0x300000003 (off=3).
; CHECK: store i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* addrspace(1)* %slotc, align 8
; CHECK: store i64 12884901891, i64 addrspace(1)* %offc, align 8
; Variable-index gcwrite: pool base handle, same offset fixup.
; CHECK: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* %arrc, i8 addrspace(1)* addrspace(1)* %slot)
; CHECK: store i64 12884901891, i64 addrspace(1)* %pkc, align 8
; Old buffers erased.
; CHECK-NOT: @"$const_cjstring_data.0"
; CHECK-NOT: @"$const_cjstring_data.1"
; CHECK: attributes #[[DATAATTR]] = { "cjstring_data" }

attributes #0 = { "cjstring_data" }
attributes #1 = { "cjstring_literal" }
