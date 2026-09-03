; RUN: opt -passes=cj-string-pool-merge -S < %s | FileCheck %s
;
; CastChain handles whose users are all gcwrite String materializations:
; the handle is repointed to the merged pool BASE (valid header) and the
; paired (off:len) packed-i64 store at slot+8 is rewritten from off=0 to
; the buffer's pool offset -- restoring the legacy {pool, offset, length}
; shape. This keeps the String's header semantics intact (the stdlib's
; checked arrayGet reads the length field at base+8).

%TypeInfo = type { i8 }

@"RawArray<UInt8>.ti" = private constant %TypeInfo zeroinitializer

@"$const_cjstring_data.0" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"ABC" } #0
@"$const_cjstring_data.1" = private constant { i8*, i64, [3 x i8] } { i8* bitcast (%TypeInfo* @"RawArray<UInt8>.ti" to i8*), i64 3, [3 x i8] c"DEF" } #0

declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

define void @f(i8 addrspace(1)* %arr) gc "cangjie" {
entry:
  ; Element 0: myData = buffer 0, packed (off:len) = 0x300000000 at slot+8.
  %slot0 = getelementptr i8, i8 addrspace(1)* %arr, i64 16
  %slot0c = bitcast i8 addrspace(1)* %slot0 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.0" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* %arr, i8 addrspace(1)* addrspace(1)* %slot0c)
  %off0 = getelementptr i8, i8 addrspace(1)* %arr, i64 24
  %off0c = bitcast i8 addrspace(1)* %off0 to i64 addrspace(1)*
  store i64 12884901888, i64 addrspace(1)* %off0c, align 8
  ; Element 1: myData = buffer 1, packed (off:len) = 0x300000000 at slot+8.
  %slot1 = getelementptr i8, i8 addrspace(1)* %arr, i64 32
  %slot1c = bitcast i8 addrspace(1)* %slot1 to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [3 x i8] }* @"$const_cjstring_data.1" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* %arr, i8 addrspace(1)* addrspace(1)* %slot1c)
  %off1 = getelementptr i8, i8 addrspace(1)* %arr, i64 40
  %off1c = bitcast i8 addrspace(1)* %off1 to i64 addrspace(1)*
  store i64 12884901888, i64 addrspace(1)* %off1c, align 8
  ret void
}

; Merged pool "ABCDEF": "ABC" at data offset 0, "DEF" at data offset 3.
; CHECK: @"$const_cjstring_data.merged.0" = private constant { i8*, i64, [6 x i8] } { i8* {{[^,]*}}(%TypeInfo, %TypeInfo* @"RawArray<UInt8>.ti", i32 0, i32 0), i64 6, [6 x i8] c"ABCDEF" } #[[DATAATTR:[0-9]+]]
; Handles become cast chains of the pool base (valid header), and each
; paired offset store is rewritten: element 0 keeps off=0 (12884901888 =
; 0x300000000), element 1 gets off=3 (12884901891 = 0x300000003).
; CHECK: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* %arr, i8 addrspace(1)* addrspace(1)* %slot0c)
; CHECK: store i64 12884901888, i64 addrspace(1)* %off0c, align 8
; CHECK: call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* addrspacecast (i8* bitcast ({ i8*, i64, [6 x i8] }* @"$const_cjstring_data.merged.0" to i8*) to i8 addrspace(1)*), i8 addrspace(1)* %arr, i8 addrspace(1)* addrspace(1)* %slot1c)
; CHECK: store i64 12884901891, i64 addrspace(1)* %off1c, align 8
; Old buffers erased.
; CHECK-NOT: @"$const_cjstring_data.0"
; CHECK-NOT: @"$const_cjstring_data.1"
; CHECK: attributes #[[DATAATTR]] = { "cjstring_data" }

attributes #0 = { "cjstring_data" }
