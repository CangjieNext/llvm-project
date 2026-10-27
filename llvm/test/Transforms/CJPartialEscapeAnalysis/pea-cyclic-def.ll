; RUN: opt < %s '-passes=cj-pea' -S | FileCheck %s

%TypeInfo = type { i8 }
%ObjLayout.Box = type { [16 x i8] } ; 16: two eight-byte reference slots.

@Box.ti = global %TypeInfo zeroinitializer, !RelatedType !0

declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32)
declare void @escape(i8 addrspace(1)*)
declare void @llvm.memcpy.p0i8.p1i8.i64(i8*, i8 addrspace(1)*, i64, i1 immarg)
declare void @llvm.memcpy.p1i8.p0i8.i64(i8 addrspace(1)*, i8*, i64, i1 immarg)

; CHECK-LABEL: define void @cyclic_def(
define void @cyclic_def() gc "cangjie" {
entry:
  %a = alloca [16 x i8]
  %tmp = alloca [16 x i8]
  %a.i8 = bitcast [16 x i8]* %a to i8*
  %tmp.i8 = bitcast [16 x i8]* %tmp to i8*
  %h = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @Box.ti to i8*), i32 24) ; 24: header plus payload.
  %h.field = getelementptr i8, i8 addrspace(1)* %h, i64 40 ; 40: source and destination offsets differ.
  call void @llvm.memcpy.p1i8.p0i8.i64(i8 addrspace(1)* %h.field, i8* %a.i8, i64 16, i1 false) ; 16: copy the payload.
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* %a.i8, i8 addrspace(1)* %h.field, i64 16, i1 false) ; 16: copy the payload.
  call void @llvm.memcpy.p0i8.p1i8.i64(i8* %tmp.i8, i8 addrspace(1)* %h.field, i64 16, i1 false) ; 16: copy the payload.
  call void @escape(i8 addrspace(1)* %h)
  ret void
}

!0 = !{!"ObjLayout.Box"}
