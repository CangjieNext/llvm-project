; RUN: opt < %s -insert-cj-tbaa -S | FileCheck %s
;
; 覆盖 InsertCJTBAA.cpp getTypeString/getTBAARoot 各基础类型分支：
;   bfloat(L125) / half(L120) / float(L114) / double(L117) / iN(L101-110)
; Cangjie 前端不产生 bfloat/half，此 lit 精确构造。

; CHECK-LABEL: define void @primitive_types()
define void @primitive_types() {
entry:
  %i8p  = alloca i8,  align 1
  %i16p = alloca i16, align 2
  %i32p = alloca i32, align 4
  %i64p = alloca i64, align 8
  %f32p = alloca float,  align 4
  %f64p = alloca double, align 8
  %bfp  = alloca bfloat, align 2
  %hfp  = alloca half,   align 2
  ; 验证每种 load 被插入 !tbaa
  ; CHECK: load i8,    i8*    %i8p,  align 1, !tbaa
  ; CHECK: load i16,   i16*   %i16p, align 2, !tbaa
  ; CHECK: load i32,   i32*   %i32p, align 4, !tbaa
  ; CHECK: load i64,   i64*   %i64p, align 8, !tbaa
  ; CHECK: load float,  float*  %f32p, align 4, !tbaa
  ; CHECK: load double, double* %f64p, align 8, !tbaa
  ; CHECK: load bfloat, bfloat* %bfp,  align 2, !tbaa
  ; CHECK: load half,   half*   %hfp,  align 2, !tbaa
  %a = load i8,    i8*    %i8p,  align 1
  %b = load i16,   i16*   %i16p, align 2
  %c = load i32,   i32*   %i32p, align 4
  %d = load i64,   i64*   %i64p, align 8
  %e = load float,  float*  %f32p, align 4
  %f = load double, double* %f64p, align 8
  %g = load bfloat, bfloat* %bfp,  align 2
  %h = load half,   half*   %hfp,  align 2
  ret void
}

; 元数据段: !2 和 !3 是共享节点，跟在 i8 后面就出现
; CHECK: !{!"i8", !{{[0-9]+}}}
; 这里插入 omnipotent char 和 Simple Cangjie TBAA（紧接着 i8 之后）
; CHECK: !{!"omnipotent char", !{{[0-9]+}}}
; CHECK: !{!"Simple Cangjie TBAA"}
; 其余类型根节点名称
; CHECK: !{!"i16", !{{[0-9]+}}}
; CHECK: !{!"i32", !{{[0-9]+}}}
; CHECK: !{!"i64", !{{[0-9]+}}}
; CHECK: !{!"f32", !{{[0-9]+}}}
; CHECK: !{!"f64", !{{[0-9]+}}}
; CHECK: !{!"bfloat", !{{[0-9]+}}}
; CHECK: !{!"half", !{{[0-9]+}}}
