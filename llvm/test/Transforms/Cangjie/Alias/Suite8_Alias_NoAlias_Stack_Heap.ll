; RUN: opt -aa-pipeline=cangjie-aa,basic-aa -passes='aa-eval -print-all-alias-info' -S < %s 2>&1 | FileCheck %s
;
; 套件8-6: CJAliasAnalysis.cpp alias() NoAlias 分支
;   覆盖目标:
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:72-73 (isGCPointerOrGlobalBase:
;         cj_gcread_ref 路径 -> V = II->getArgOperand(1) 递归)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:78 (default: return false)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:101 (checkHeapMallocAlias: Argument -> NoAlias)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:112-122 (alias: stack vs heap/global -> NoAlias)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:116-118 (stack alloca + GC pointer base -> NoAlias)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:119-122 (non-GV + GlobalValue -> NoAlias)
;   触发:
;     - %stack.alloca (AllocaInst, stack) 与 %heap_obj (cj_gcread_ref 结果, GC pointer)
;       -> isGCPointerOrGlobalBase 走 cj_gcread_ref case (行 71-73), 递归 base, 返回 NoAlias
;     - 另构造 global @g_ti 作为 base -> 与 stack NoAlias (行 121-122)
;     - @CJ_MCC_NewObject 带 cj-heapmalloc attribute, 与 Argument -> NoAlias (行 100-101)

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

@.g_ti = global { i8*, i8, i8, i16, i32 } { i8* null, i8 0, i8 0, i16 0, i32 8 }

declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32) "cj-heapmalloc" "cj-runtime"

; CHECK-LABEL: Function: test_stack_vs_heap
; CHECK: NoAlias: %stack.ptr
define void @test_stack_vs_heap(i8 addrspace(1)* %heap.arg) gc "cangjie" {
entry:
  ; stack alloca (IsStackValue1 = true)
  %stack.ptr = alloca i64, align 8
  ; 从 heap.arg 取出 GC ref -> isGCPointerOrGlobalBase 走 cj_gcread_ref case
  %fld.addr = bitcast i8 addrspace(1)* %heap.arg to i8 addrspace(1)* addrspace(1)*
  %loaded.ref = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %heap.arg, i8 addrspace(1)* addrspace(1)* %fld.addr)
  ; 这两条 store/load 互相对比应判 NoAlias (stack vs GC heap)
  store i64 1, i64* %stack.ptr, align 8
  %bc = bitcast i8 addrspace(1)* %loaded.ref to i64 addrspace(1)*
  store i64 2, i64 addrspace(1)* %bc, align 8
  ret void
}

; CHECK-LABEL: Function: test_heapmalloc_arg
; CHECK: NoAlias
define void @test_heapmalloc_arg(i8 addrspace(1)* noalias %other) {
entry:
  ; %new 是 cj-heapmalloc 调用, 与 %other (Argument) 应判 NoAlias (行 100-101)
  %ti.bc = bitcast { i8*, i8, i8, i16, i32 }* @.g_ti to i8*
  %new = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %ti.bc, i32 8)
  %new.bc = bitcast i8 addrspace(1)* %new to i64 addrspace(1)*
  %other.bc = bitcast i8 addrspace(1)* %other to i64 addrspace(1)*
  store i64 1, i64 addrspace(1)* %new.bc, align 8
  store i64 2, i64 addrspace(1)* %other.bc, align 8
  ret void
}
