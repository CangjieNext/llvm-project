; RUN: opt < %s -insert-cj-tbaa -S | FileCheck %s
;
; 覆盖 InsertCJTBAA.cpp L779-780:
;   maybeAllocaWithTwoDifferentType: 指针经 PHI 汇聚后 load → TBAA 被清除
;
; 构造: alloca → bitcast → PHI(两个不同类型) → load → TBAA removed = !tbaa nullptr

%S = type { i64, i32 }
%T = type { i32, i64, i32 }

define void @phi_clear(i1 %cond) {
bb0:
  br i1 %cond, label %bb1, label %bb2
bb1:
  %s = alloca %S, align 8
  %bs = bitcast %S* %s to i8*
  br label %merge
bb2:
  %t = alloca %T, align 8
  %bt = bitcast %T* %t to i8*
  br label %merge
merge:
  ; PHI 汇聚两个不同类型的指针
  %p = phi i8* [ %bs, %bb1 ], [ %bt, %bb2 ]
  ; load from PHI → getUnderlyingObject 返回 PHI → setMetadata(tbaa, nullptr)
  ; CHECK: load i8, i8* %p{{.*}}
  ; CHECK-NOT: load i8, i8* %p{{.*}}!tbaa
  %v = load i8, i8* %p, align 1
  ret void
}
