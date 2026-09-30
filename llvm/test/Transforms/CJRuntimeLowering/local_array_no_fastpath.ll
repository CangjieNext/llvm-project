; RUN: opt < %s -passes=cj-runtime-lowering -S | FileCheck %s

; Constant-length llvm.cj.malloc.local.array must NOT get !fast_new_array.
; NewArrayFast bumps the heap TLS allocator and never marks the object local;
; writing a local element into it then fails in MaybeLocalWriteRef.

%ArrayBase = type { i64 }
%ArrayLayout.refArray = type { %ArrayBase, [0 x i8 addrspace(1)*] }
%BitMap = type opaque
%TypeTemplate = type opaque
%TypeInfo = type {
  i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i16, i32*, i8*, i8*, i8*,
  %TypeInfo*, i8*, i8*, i8*
}

@"RawArray.ti" = linkonce_odr global %TypeInfo {
  i8* null,
  i8 -126,
  i8 0,
  i16 0,
  i32 8,
  %BitMap* null,
  i32 0,
  i8 1,
  i8 0,
  i16 -32768,
  i32* null,
  i8* null,
  i8* null,
  i8* null,
  %TypeInfo* null,
  i8* null,
  i8* null,
  i8* null
}, !RelatedType !0

declare i8 addrspace(1)* @llvm.cj.malloc.local.array(i8*, i64, i64)
declare i8 addrspace(1)* @llvm.cj.malloc.array(i8*, i64, i64)

define i8 addrspace(1)* @local_const_len() gc "cangjie" {
; CHECK-LABEL: define {{.*}} @local_const_len
; CHECK: call {{.*}} @CJ_MCC_NewLocalObjArray(i8* bitcast ({{.*}} @{{"?}}RawArray.ti{{"?}} to i8*), i64 16, i64 poison)
; CHECK-NOT: !fast_new_array
; CHECK-NOT: llvm.cj.malloc.local.array
entry:
  %0 = call noalias i8 addrspace(1)* @llvm.cj.malloc.local.array(i8* bitcast (%TypeInfo* @"RawArray.ti" to i8*), i64 16, i64 8)
  ret i8 addrspace(1)* %0
}

; Heap arrays with constant length may still use the fast path.
define i8 addrspace(1)* @heap_const_len() gc "cangjie" {
; CHECK-LABEL: define {{.*}} @heap_const_len
; CHECK: call {{.*}} @CJ_MCC_NewObjArray({{.*}}), {{.*}}!fast_new_array
; CHECK-NOT: llvm.cj.malloc.array
entry:
  %0 = call noalias i8 addrspace(1)* @llvm.cj.malloc.array(i8* bitcast (%TypeInfo* @"RawArray.ti" to i8*), i64 16, i64 8)
  ret i8 addrspace(1)* %0
}

!0 = !{!"ArrayLayout.refArray"}
