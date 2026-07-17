; RUN: opt -passes=cj-generic-intrinsic-opt --cangjie-pipeline -S < %s | FileCheck %s
;
; 套件8-10: CJGenericIntrinsicOpt.cpp eliminateReferenceChecking
;   覆盖目标:
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:205-214 (扫 LoadInst, 查
;         ti_load metadata)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:215-226 (ConstantExpr +
;         GlobalVariable isCJTypeInfo 检查)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:228-243 (ICmpInst slt 0 替换)
;     - llvm/lib/Transforms/Scalar/CJGenericIntrinsicOpt.cpp:246-253 (RemoveInsts 消除
;         无效 LoadTI/ICmp)
;   触发: 构造一段 LLVM IR 模拟 cangjie 前端"加载 typeinfo + icmp slt 0 检查 ref 类型"
;   IR, 用 !ti_load 标记, typeinfo GV 带 CFileKlass 属性, 让 pass 走替换 + 删除路径

%fake_typeinfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, %fake_typeinfo*, i8*, i8* }

; "CFileKlass" partition 属性 -> isCJTypeInfo() true
@ti.ref.klass = global %fake_typeinfo {
  i8* null,            ; 0
  i8 -1,               ; 1: kind < 0 表示 ref type
  i8 0,                ; 2
  i16 0,               ; 3
  i32 0,               ; 4
  i8* null,            ; 5
  i32 0, i8 0, i8 0, i32* null,
  i8* null, i8* null, i8* null, %fake_typeinfo* null, i8* null, i8* null
}, partition "CFileKlass"

@ti.value.klass = global %fake_typeinfo {
  i8* null,
  i8 1,                ; 1: kind > 0 表示 value type
  i8 0, i16 0, i32 0, i8* null,
  i32 0, i8 0, i8 0, i32* null,
  i8* null, i8* null, i8* null, %fake_typeinfo* null, i8* null, i8* null
}, partition "CFileKlass"

declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)

; CHECK-LABEL: define void @test_elim_ref_check_ref
; CHECK-NOT: icmp slt
; CHECK: br i1 true
define void @test_elim_ref_check_ref(i8 addrspace(1)* %obj, i1 %cond) gc "cangjie" {
entry:
  ; 加载 ti 并用 !ti.load 标记 (模仿 cangjie 前端生成代码)
  %ti.const = bitcast %fake_typeinfo* @ti.ref.klass to i8*
  %ti.ptr = getelementptr i8, i8* %ti.const, i64 0
  %ti.kind.ptr = getelementptr i8, i8* %ti.ptr, i64 1
  %ti.kind = load i8, i8* %ti.kind.ptr, align 1, !ti_load !0
  ; icmp slt i8 %ti.kind, 0 -> pass 替换为 getReferenceType 返回值
  %is.ref = icmp slt i8 %ti.kind, 0
  br i1 %is.ref, label %ref, label %nonref

ref:
  %fld.ref = getelementptr i8, i8 addrspace(1)* %obj, i64 8
  %fld.cast.ref = bitcast i8 addrspace(1)* %fld.ref to i8 addrspace(1)* addrspace(1)*
  %r = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %obj, i8 addrspace(1)* addrspace(1)* %fld.cast.ref)
  br label %merge

nonref:
  br label %merge

merge:
  ret void
}

; CHECK-LABEL: define void @test_elim_ref_check_value
; CHECK-NOT: icmp slt
; CHECK: br i1 false
define void @test_elim_ref_check_value(i8 addrspace(1)* %obj, i1 %cond) gc "cangjie" {
entry:
  %ti.const = bitcast %fake_typeinfo* @ti.value.klass to i8*
  %ti.ptr = getelementptr i8, i8* %ti.const, i64 0
  %ti.kind.ptr = getelementptr i8, i8* %ti.ptr, i64 1
  %ti.kind = load i8, i8* %ti.kind.ptr, align 1, !ti_load !0
  %is.ref = icmp slt i8 %ti.kind, 0
  br i1 %is.ref, label %ref, label %nonref

ref:
  br label %merge

nonref:
  br label %merge

merge:
  ret void
}

!0 = !{}
