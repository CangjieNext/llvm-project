; RUN: opt -passes=cj-generic-intrinsic-opt --cangjie-pipeline -S < %s | FileCheck %s
;
; 套件8-11: CJIntrinsics.cpp getBaseObj/getDest/getSource/getSize/getValueArg/
;           getPointerArg/getAtomicOrder 全 case 覆盖 (直接通过 lit IR 调 intrinsic)
;   覆盖目标:
;     - llvm/lib/IR/CJIntrinsics.cpp:27-37  (getBaseObj 各 case)
;     - llvm/lib/IR/CJIntrinsics.cpp:47-50  (getValueArg cj_gcwrite_ref / static_ref)
;     - llvm/lib/IR/CJIntrinsics.cpp:59-66  (getPointerArg 各 case)
;     - llvm/lib/IR/CJIntrinsics.cpp:75-92  (getDest cj_gcwrite_struct / static_struct /
;         gcread_struct / static_struct / gcwrite_generic / gcread_generic /
;         assign_generic / array_copy_*)
;     - llvm/lib/IR/CJIntrinsics.cpp:101-118 (getSource 各 case)
;     - llvm/lib/IR/CJIntrinsics.cpp:127-142 (getSize 各 case)
;     - llvm/lib/IR/CJIntrinsics.cpp:150-155 (getAtomicOrder: atomic_load/store/swap)
;   触发: 直接构造所有 intrinsic 调用, 由 cj-generic-intrinsic-opt 或 AA pipeline 间接
;         调用这些 helper (processGenericBarrier, processAssignGeneric, alias, getModRefInfo)

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"

%fake_typeinfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, %fake_typeinfo*, i8*, i8* }

declare void @llvm.cj.gcwrite.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*)
declare i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)* addrspace(1)*)
declare void @llvm.cj.gcwrite.struct(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.gcread.struct(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.gcwrite.static.struct(i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.gcread.static.struct(i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.gcwrite.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i32)
declare void @llvm.cj.gcread.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i32)
declare void @llvm.cj.assign.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8*)
declare void @llvm.cj.array.copy.ref(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.array.copy.struct(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare void @llvm.cj.array.copy.generic(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)*, i64)
declare i8 addrspace(1)* @llvm.cj.atomic.load(i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*, i32)
declare void @llvm.cj.atomic.store(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*, i32)
declare i8 addrspace(1)* @llvm.cj.atomic.swap(i8 addrspace(1)*, i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)*, i32)

; CHECK-LABEL: define void @cover_all_intrinsics
define void @cover_all_intrinsics(i8 addrspace(1)* %a, i8 addrspace(1)* %b, i8 addrspace(1)* %c) gc "cangjie" {
entry:
  ; 触发 getValueArg(cj_gcwrite_ref Val=arg0) 和 getBaseObj(cj_gcwrite_ref BaseObj=arg1),
  ; getPointerArg(cj_gcwrite_ref FieldPtr=arg2)
  %fld.cast = bitcast i8 addrspace(1)* %a to i8 addrspace(1)* addrspace(1)*
  call void @llvm.cj.gcwrite.ref(i8 addrspace(1)* %b, i8 addrspace(1)* %a, i8 addrspace(1)* addrspace(1)* %fld.cast)

  ; getBaseObj(cj_gcread_ref) + getPointerArg(cj_gcread_ref)
  %r1 = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %a, i8 addrspace(1)* addrspace(1)* %fld.cast)

  ; getValueArg(cj_gcwrite_static_ref) + getPointerArg(cj_gcwrite_static_ref)
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %r1, i8 addrspace(1)* addrspace(1)* %fld.cast)
  ; getPointerArg(cj_gcread_static_ref)
  %r2 = call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)* addrspace(1)* %fld.cast)

  ; getBaseObj + getDest + getSource + getSize on cj_gcwrite_struct
  call void @llvm.cj.gcwrite.struct(i8 addrspace(1)* %a, i8 addrspace(1)* %b, i8 addrspace(1)* %c, i64 16)
  ; cj_gcread_struct
  call void @llvm.cj.gcread.struct(i8 addrspace(1)* %b, i8 addrspace(1)* %a, i8 addrspace(1)* %c, i64 16)

  ; getDest + getSource + getSize on cj_gcwrite_static_struct / gcread_static_struct
  call void @llvm.cj.gcwrite.static.struct(i8 addrspace(1)* %b, i8 addrspace(1)* %c, i64 16)
  call void @llvm.cj.gcread.static.struct(i8 addrspace(1)* %b, i8 addrspace(1)* %c, i64 16)

  ; getBaseObj + getDest + getSource + getSize on cj_gcwrite_generic / gcread_generic
  call void @llvm.cj.gcwrite.generic(i8 addrspace(1)* %a, i8 addrspace(1)* %b, i8 addrspace(1)* %c, i32 16)
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %b, i8 addrspace(1)* %a, i8 addrspace(1)* %c, i32 16)

  ; getDest + getSource on cj_assign_generic
  %ti.bc = bitcast i8 addrspace(1)* %a to i8*
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %b, i8 addrspace(1)* %c, i8* %ti.bc)

  ; getDest + getSource + getSize on cj_array_copy_* (ArrayCopy::DstPtr/SrcPtr/Size)
  call void @llvm.cj.array.copy.ref(i8 addrspace(1)* %a, i8 addrspace(1)* %b, i8 addrspace(1)* %a, i8 addrspace(1)* %c, i64 16)
  call void @llvm.cj.array.copy.struct(i8 addrspace(1)* %a, i8 addrspace(1)* %b, i8 addrspace(1)* %a, i8 addrspace(1)* %c, i64 16)
  call void @llvm.cj.array.copy.generic(i8 addrspace(1)* %a, i8 addrspace(1)* %b, i8 addrspace(1)* %a, i8 addrspace(1)* %c, i64 16)

  ; getAtomicOrder on cj_atomic_load/store/swap
  %al = call i8 addrspace(1)* @llvm.cj.atomic.load(i8 addrspace(1)* %a, i8 addrspace(1)* addrspace(1)* %fld.cast, i32 5)
  call void @llvm.cj.atomic.store(i8 addrspace(1)* %al, i8 addrspace(1)* %b, i8 addrspace(1)* addrspace(1)* %fld.cast, i32 5)
  %as = call i8 addrspace(1)* @llvm.cj.atomic.swap(i8 addrspace(1)* %b, i8 addrspace(1)* %c, i8 addrspace(1)* addrspace(1)* %fld.cast, i32 5)
  ret void
}
