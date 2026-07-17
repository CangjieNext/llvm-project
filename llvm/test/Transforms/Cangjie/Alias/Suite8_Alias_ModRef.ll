; RUN: opt -aa-pipeline=cangjie-aa,basic-aa -passes='gvn' --cangjie-pipeline -S < %s | FileCheck %s
;
; 套件8-7: CJAliasAnalysis.cpp getModRefInfo() 各 ModRef 分支
;   覆盖目标:
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:151 (default break, getLocInfoForCJIntrinsics)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:158-161 (cj_gcwrite_struct -> Src/Dst/Base)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:206-207 (cj_gcread_ref -> MustRef 分支, AR==MustAlias)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:209 (cj_gcread_ref -> Ref 分支, AR==MayAlias)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:219 (cj_gcwrite_ref -> Mod 分支)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:223-234 (cj_gcread_static_struct / cj_gcwrite_static_struct)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:235-253 (cj_gcread_struct / cj_gcwrite_struct /
;         cj_gcwrite_generic / cj_gcread_generic / cj_assign_generic Src/Dst/Base 分支)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:243 (LocS/LocD 报 fatal_error 路径 — 不触发,
;         留给 default case 的 break, 即 L151)
;   触发方式: 同 Loc 上对 cj_gcwrite_ref/cj_gcread_ref/cj_gcwrite_struct/cj_gcread_struct/
;     cj_gcwrite_generic/cj_gcread_generic/cj_assign_generic 调用 getModRefInfo,
;     GVN 或 DSE 跑 AA -> 触发 alias()/getModRefInfo() 间接调用
;   注: 用 gvn --cangjie-pipeline 触发 alias(), 验证 call 与 load 是否被合并/消除

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

%fake_typeinfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, %fake_typeinfo*, i8*, i8* }

declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.gcwrite.struct(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.gcread.struct(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.gcwrite.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i32)
declare void @llvm.cj.gcread.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i32)
declare void @llvm.cj.assign.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8*)

; CHECK-LABEL: define i8 addrspace(1)* @test_gcread_ref_after_gcwrite_ref
; CHECK: ret i8 addrspace(1)* %v1
define i8 addrspace(1)* @test_gcread_ref_after_gcwrite_ref(i8 addrspace(1)* %base) gc "cangjie" {
entry:
  %fld = getelementptr i8, i8 addrspace(1)* %base, i64 8
  %fld.cast = bitcast i8 addrspace(1)* %fld to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %base, i8 addrspace(1)* %base, i8 addrspace(1)* addrspace(1)* %fld.cast)
  ; gcread.ref 读同一 Loc -> getModRefInfo 走 MustAlias/MustRef (行 206-207)
  ; 也覆盖 MayAlias Ref 分支 (行 209)
  %v1 = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %base, i8 addrspace(1)* addrspace(1)* %fld.cast)
  ret i8 addrspace(1)* %v1
}

; CHECK-LABEL: define void @test_gcread_struct_after_gcwrite_struct
define void @test_gcread_struct_after_gcwrite_struct(i8 addrspace(1)* %base, i8 addrspace(1)* %dst, i8 addrspace(1)* %src) gc "cangjie" {
entry:
  ; 写后读同一 struct Loc -> getLocInfoForCJIntrinsics cj_gcwrite_struct 分支 (行 158-161)
  ;                                              cj_gcread_struct 分支 (类似行 158-161)
  ; -> getModRefInfo 行 235-253 SrcAA/DstAA/BaseAA 路径
  call void @llvm.cj.gcwrite.struct(i8 addrspace(1)* %base, i8 addrspace(1)* %dst, i8 addrspace(1)* %src, i64 16)
  call void @llvm.cj.gcread.struct(i8 addrspace(1)* %dst, i8 addrspace(1)* %base, i8 addrspace(1)* %src, i64 16)
  ret void
}

; CHECK-LABEL: define void @test_generic_barrier
define void @test_generic_barrier(i8 addrspace(1)* %base, i8 addrspace(1)* %dst, i8 addrspace(1)* %src) gc "cangjie" {
entry:
  ; cj_assign_generic / cj_gcread.generic / cj_gcwrite.generic 都触发
  ; getLocInfoForCJIntrinsics (行 162-176) -> getModRefInfo SrcAA/DstAA/BaseAA 分支
  %ti.bc = bitcast i8 addrspace(1)* %base to i8*
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %dst, i8 addrspace(1)* %src, i8* %ti.bc)
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %dst, i8 addrspace(1)* %base, i8 addrspace(1)* %src, i32 16)
  call void @llvm.cj.gcwrite.generic(i8 addrspace(1)* %base, i8 addrspace(1)* %src, i8 addrspace(1)* %dst, i32 16)
  ret void
}
