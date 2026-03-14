; RUN: opt < %s -passes='function(simplifycfg),globaldce' -sink-common-insts --cangjie-pipeline -S | FileCheck %s

%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8*, %ExtensionDef**, i16 }
%ExtensionDef = type { i32, i8, i8*, i8*, i8*, i8* }
%Unit.Type = type {}

@"I.tt" = internal global %TypeTemplate zeroinitializer #1
@"J.tt" = internal global %TypeTemplate zeroinitializer #1
@"K.tt" = internal global %TypeTemplate zeroinitializer #1
@"L.tt" = internal global %TypeTemplate zeroinitializer #1
@"A.tt" = internal global %TypeTemplate zeroinitializer #1
; CHECK: @A_ed_I.ft = private global [1 x i8*] [i8* bitcast (void ()* @funcA_I to i8*)]
@"A_ed_I.ft" = private global [1 x i8*] [i8* bitcast (void ()* @funcA_I to i8*)]
@"A_ed_I" = private global %ExtensionDef { i32 1, i8 0, i8* bitcast (%TypeTemplate* @"A.tt" to i8*), i8* null, i8* null, i8* bitcast ([1 x i8*]* @"A_ed_I.ft" to i8*) }, !inheritedType !0 #0
; CHECK: @A_ed_J.ft = private global [1 x i8*] [i8* bitcast (void ()* @funcA_J to i8*)]
@"A_ed_J.ft" = private global [1 x i8*] [i8* bitcast (void ()* @funcA_J to i8*)]
@"A_ed_J" = private global %ExtensionDef { i32 1, i8 0, i8* bitcast (%TypeTemplate* @"A.tt" to i8*), i8* null, i8* null, i8* bitcast ([1 x i8*]* @"A_ed_J.ft" to i8*) }, !inheritedType !1 #0
; CHECK: @A_ed_K.ft = private global [1 x i8*] zeroinitializer
@"A_ed_K.ft" = private global [1 x i8*] [i8* bitcast (void ()* @funcA_K to i8*)]
@"A_ed_K" = private global %ExtensionDef { i32 1, i8 0, i8* bitcast (%TypeTemplate* @"A.tt" to i8*), i8* null, i8* null, i8* bitcast ([1 x i8*]* @"A_ed_K.ft" to i8*) }, !inheritedType !2 #0
; CHECK: @A_ed_L.ft = private global [1 x i8*] [i8* bitcast (void ()* @funcA_L to i8*)]
@"A_ed_L.ft" = private global [1 x i8*] [i8* bitcast (void ()* @funcA_L to i8*)]
@"A_ed_L" = private global %ExtensionDef { i32 1, i8 0, i8* bitcast (%TypeTemplate* @"A.tt" to i8*), i8* null, i8* null, i8* bitcast ([1 x i8*]* @"A_ed_L.ft" to i8*) }, !inheritedType !3 #0


@NonExternalExtensionDefs = private global [5 x %ExtensionDef*] [
  %ExtensionDef* @"A_ed_I",
  %ExtensionDef* @"A_ed_J",
  %ExtensionDef* @"A_ed_K",
  %ExtensionDef* @"A_ed_L",
  %ExtensionDef* null
]
@llvm.used = appending global [1 x i8*] [i8* bitcast ([5 x %ExtensionDef*]* @NonExternalExtensionDefs to i8*)]

define internal void @funcA_I() gc "cangjie" {
  ret void
}
define internal void @funcA_J() gc "cangjie" {
  ret void
}
define internal void @funcA_K() gc "cangjie" {
  ret void
}
define internal void @funcA_L() gc "cangjie" {
  ret void
}

; CHECK: define internal void @funcA_I()
; CHECK: define internal void @funcA_J()
; CHECK-NOT: @funcA_K

define void @caller1(i1 %cond, void ()** %ptr0, void ()** %ptr1) gc "cangjie" {
entry:
  br i1 %cond, label %then, label %else

then:
  %fp1 = load void ()*, void ()** %ptr0, !objType !0, !FuncTable !7
  br label %merge

else:
  %fp2 = load void ()*, void ()** %ptr1, !objType !1, !FuncTable !7
  br label %merge

merge:
  %fp = phi void ()* [ %fp1, %then ], [ %fp2, %else ]
  call void %fp()
  ret void
}

define void @caller2(i1 %cond, void ()** %ptr0, void ()** %ptr1) gc "cangjie" {
entry:
  br i1 %cond, label %then, label %else

then:
  %fp1 = load void ()*, void ()** %ptr0, !objType !0, !FuncTable !7
  br label %merge

else:
  %fp2 = load void ()*, void ()** %ptr1, !objType !4, !FuncTable !5
  br label %merge

merge:
  %fp = phi void ()* [ %fp1, %then ], [ %fp2, %else ]
  call void %fp()
  ret void
}

define void @caller3(i1 %cond, void ()** %ptr0, void ()** %ptr1) gc "cangjie" {
entry:
  br i1 %cond, label %then, label %else

then:
  %fp1 = load void ()*, void ()** %ptr0, !objType !4, !FuncTable !5
  br label %merge

else:
  %fp2 = load void ()*, void ()** %ptr1, !objType !6, !FuncTable !5
  br label %merge

merge:
  %fp = phi void ()* [ %fp1, %then ], [ %fp2, %else ]
  call void %fp()
  ret void
}

attributes #0 = { "CFileMTable" }
attributes #1 = { "cj_tt" }

!0 = !{!"I.tt"}
!1 = !{!"J.tt"}
!2 = !{!"K.tt"}
!3 = !{!"L.tt"}
!4 = !{!0, !1}
!5 = !{!7, !7}
!6 = !{!0, !3}
!7 = !{i64 0}

