; RUN: opt < %s -insert-cj-tbaa -S | FileCheck %s
;
; 覆盖 InsertCJTBAA.cpp:
;   L100  getArrayName (4*array_int8, ArrayType 内嵌 int8)
;   L146  getArrayInnerType (穿透 [4xi8] 到 i8)
;   L364  insertCJTBAAStructImpl (DstTy 是 StructType)
;   L42-71 getPrimitiveTypeName 的 int8 fallthrough (array_int8)
;
; S = { i64, [4 x i8], i32 } -> gep offset=12 -> store i32 -> 通过数组成员求 TBAA

%S = type { i64, [4 x i8], i32 }

define void @struct_with_array() {
bb0:
  ; CHECK: store i32 {{.*}}, !tbaa
  %0 = alloca %S, align 8
  %1 = bitcast %S* %0 to i8*
  %2 = getelementptr i8, i8* %1, i32 12
  %3 = bitcast i8* %2 to i32*
  store i32 77, i32* %3, align 4
  ret void
}

; 输出元数据: !1 = !{!"S", !2, i64 0, !5, i64 8, !7, i64 12}
; !5 = !{!"4*array_int8", !6}   <- getArrayName + getPrimitiveTypeName
; !6 = !{!"i8", !3}              <- array inner type (getArrayInnerType)
;
; CHECK: !{!"S"
; CHECK: !{!"i64"
; CHECK: !{!"omnipotent char"
; CHECK: !{!"Simple Cangjie TBAA"}
; CHECK: !{!"4*array_int8"
; CHECK: !{!"i8"
; CHECK: !{!"i32"
