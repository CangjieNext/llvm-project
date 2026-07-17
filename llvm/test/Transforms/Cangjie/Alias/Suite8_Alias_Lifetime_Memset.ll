; RUN: opt -aa-pipeline=cangjie-aa,basic-aa -passes='dse' --cangjie-pipeline -S < %s | FileCheck %s
;
; 套件8-8: CJAliasAnalysis.cpp getModRefInfo() 剩余分支
;   覆盖目标:
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:151 (getLocInfoForCJIntrinsics default break)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:158-176 (cj_gcread_struct / cj_gcwrite_struct /
;         cj_gcwrite_generic / cj_gcread_generic / cj_assign_generic 各 Src/Dst/Base 计算)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:195-200 (lifetime_end -> NoAlias -> NoModRef)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:206-209 (cj_gcread_static_ref: MustAlias ->
;         MustRef / 否则 Ref)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:216-219 (cj_gcwrite_static_ref: MustAlias ->
;         MustMod / 否则 Mod)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:221-234 (cj_gcread_static_struct /
;         cj_gcwrite_static_struct SrcAA/DestAA 分支)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:254-261 (cj_memset -> NoAlias -> NoModRef,
;         否则 Mod)
;     - llvm/lib/Analysis/CJAliasAnalysis.cpp:274-297 (CangjieAAWrapperPass 注册/初始化 +
;         runOnFunction/getAnalysisUsage)
;   触发: 用 dse 跑 gcread.static.ref/gcwrite.static.ref/gcread.static.struct/
;     gcwrite.static.struct/cj.memset + lifetime.end, 验证 dead-store 被消除

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

declare void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.gcwrite.static.struct(i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.gcread.static.struct(i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.memset(i8 addrspace(1)*, i8, i64, i1)
declare void @llvm.lifetime.end.p0i8(i64, i8 addrspace(1)*)

@g.static = global { i8*, i8 addrspace(1)* } { i8* null, i8 addrspace(1)* null }

; CHECK-LABEL: define void @test_static_ref_modref
; CHECK: call i8 addrspace(1)* @llvm.cj.gcread.static.ref
; CHECK-NOT: call void @llvm.cj.gcwrite.static.ref
define void @test_static_ref_modref() gc "cangjie" {
entry:
  %gptr = getelementptr { i8*, i8 addrspace(1)* }, { i8*, i8 addrspace(1)* }* @g.static, i64 0, i32 1
  %slot = load i8 addrspace(1)*, i8 addrspace(1)** %gptr, align 8
  ; 写 static ref -> getModRefInfo cj_gcwrite_static_ref 分支 (行 216-219)
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %slot, i8 addrspace(1)* addrspace(1)* %gptr)
  ; 读 static ref (Loc == gptr) -> MustAlias -> MustRef 分支 (行 206-207) / 否则 Ref (行 209)
  %v = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)* addrspace(1)* %gptr)
  ; 后续不再用 -> DSE 应消除上面 gcwrite (NoModRef/MayAlias -> 保守保留), 至少调用 getModRefInfo
  ret void
}

; CHECK-LABEL: define void @test_static_struct_modref
define void @test_static_struct_modref(i8 addrspace(1)* %dst, i8 addrspace(1)* %src) gc "cangjie" {
entry:
  ; 触发 getModRefInfo cj_gcread_static_struct / cj_gcwrite_static_struct 分支
  ; (行 221-234 SrcAA + DestAA 计算)
  call void @llvm.cj.gcwrite.static.struct(i8 addrspace(1)* %dst, i8 addrspace(1)* %src, i64 16)
  call void @llvm.cj.gcread.static.struct(i8 addrspace(1)* %dst, i8 addrspace(1)* %src, i64 16)
  ret void
}

; CHECK-LABEL: define void @test_memset_and_lifetime
; CHECK-NOT: call void @llvm.cj.memset
define void @test_memset_and_lifetime(i8 addrspace(1)* %buf) gc "cangjie" {
entry:
  ; cj.memset -> getModRefInfo 行 254-261 (NoAlias -> NoModRef / 否则 Mod)
  call void @llvm.cj.memset(i8 addrspace(1)* %buf, i8 0, i64 32, i1 false)
  ; lifetime.end 紧随 -> NoAlias -> NoModRef (行 195-200)
  %bc = bitcast i8 addrspace(1)* %buf to i8*
  call void @llvm.lifetime.end.p0i8(i64 32, i8 addrspace(1)* %buf)
  ret void
}
