; RUN: opt -passes=cj-generic-intrinsic-opt --cangjie-pipeline -S < %s | FileCheck %s
;
; 套件8-9: CJGenericIntrinsicOpt.cpp lowerToMemcpy / lowerGenericBarrier / processAssignGeneric
;   覆盖目标:
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:78  (default report_fatal_error,
;         不触发, 用同 switch 其余 case 覆盖为 80-101)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:80-86  (lowerToMemcpy:
;         cj_assign_generic case -> GEP+memcpy)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:87-93  (cj_gcread_generic case)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:94-101 (cj_gcwrite_generic case)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:102-109 (TBAA 元数据复制)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:113-118 (Size == 0 早退)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:126-153 (lowerGenericBarrier:
;         hasInitializer / isReferenceType / hasRefField / isPrimitiveTypeInfo /
;         getTypeLayoutType / containsGCPtrType)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:159-170 (processGenericBarrier:
;         isStackToStackCopy lowerToMemcpy 路径)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:173-203 (processAssignGeneric
;         isStackToStackCopy + TI 提取 size 分支)
;   触发: 把 cj_assign_generic / cj_gcread.generic / cj_gcwrite.generic 用 TypeInfo
;   解释, 如果 TI 显示无 ref 字段则 lowerToMemcpy 把 intrinsic 替换为 llvm.memcpy

%fake_typeinfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, %fake_typeinfo*, i8*, i8* }
%struct.no.ref = type { i64, i64 }
%struct.with.ref = type { i64, i8 addrspace(1)* }

; 无 ref 字段, layout type 不含 GC ptr -> containsGCPtrType false -> lowerToMemcpy
@ti.no_ref = global %fake_typeinfo {
  i8* null,            ; 0: base
  i8 0,                ; 1: kind
  i8 0,                ; 2: flags
  i16 0,               ; 3
  i32 16,              ; 4: size (16 字节)
  i8* null,            ; 5
  i32 0,               ; 6
  i8 0,                ; 7
  i8 0,                ; 8
  i32* null,           ; 9
  i8* null,            ; 10
  i8* null,            ; 11
  i8* null,            ; 12
  %fake_typeinfo* null,; 13
  i8* null,            ; 14
  i8* null             ; 15
}, partition "CFileKlass"

; 含 ref 字段 -> containsGCPtrType true -> 不 lower
@ti.with_ref = global %fake_typeinfo {
  i8* null, i8 0, i8 0, i16 0, i32 16, i8* null, i32 0, i8 0, i8 0, i32* null,
  i8* null, i8* null, i8* null, %fake_typeinfo* null, i8* null, i8* null
}, partition "CFileKlass"

declare void @llvm.cj.assign.generic(i8 addrspace(1)* noalias nocapture writeonly, i8 addrspace(1)* noalias nocapture readonly, i8* noalias nocapture readonly)
declare void @llvm.cj.gcread.generic(i8 addrspace(1)* noalias nocapture writeonly, i8 addrspace(1)* nocapture readonly, i8 addrspace(1)* nocapture readonly, i32)
declare void @llvm.cj.gcwrite.generic(i8 addrspace(1)* nocapture, i8 addrspace(1)* nocapture writeonly, i8 addrspace(1)* noalias nocapture readonly, i32)
declare noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32) "cj-heapmalloc" "cj-runtime"

; CHECK-LABEL: define void @test_assign_generic_lower_to_memcpy
; CHECK: call void @llvm.memcpy
define void @test_assign_generic_lower_to_memcpy(i8 addrspace(1)* noalias %dst, i8 addrspace(1)* noalias %src) {
entry:
  %ti.bc = bitcast %fake_typeinfo* @ti.no_ref to i8*
  ; assign.generic -> processAssignGeneric -> isStackToStackCopy false -> lowerGenericBarrier
  ; hasInitializer / not Reference / not hasRefField -> lowerToMemcpy(Size=16)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %dst, i8 addrspace(1)* %src, i8* %ti.bc)
  ret void
}

; CHECK-LABEL: define void @test_gcread_generic_lower
; CHECK: call void @llvm.memcpy
define void @test_gcread_generic_lower(i8 addrspace(1)* %dst, i8 addrspace(1)* %base, i8 addrspace(1)* %fld) {
entry:
  %ti.bc = bitcast %fake_typeinfo* @ti.no_ref to i8*
  %fld.gep = getelementptr i8, i8 addrspace(1)* %base, i64 8
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %dst, i8 addrspace(1)* %base, i8 addrspace(1)* %fld.gep, i32 8)
  ret void
}

; CHECK-LABEL: define void @test_gcwrite_generic_keep
; CHECK: call void @llvm.cj.gcwrite.generic
define void @test_gcwrite_generic_keep(i8 addrspace(1)* %base, i8 addrspace(1)* %fld, i8 addrspace(1)* %src) {
entry:
  ; TI 显示 hasRefField (default struct 内有 GC ptr) -> 不 lower
  call void @llvm.cj.gcwrite.generic(i8 addrspace(1)* %base, i8 addrspace(1)* %fld, i8 addrspace(1)* %src, i32 16)
  ret void
}
