; RUN: opt '-passes=cj-runtime-lowering,cj-devirtual-opt' --cangjie-pipeline -S < %s | FileCheck %s

%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8*, %ExtensionDef**, i16 }
%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%ExtensionDef = type { i32, i8, i8, i16, i8*, i8*, i8*, i8* }

@"std.core:Object.ti" = external global %TypeInfo
@"default:A1.ti" = global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 1, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"std.core:Object.ti", %ExtensionDef** getelementptr inbounds ([6 x %ExtensionDef*], [6 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 2), i8* null, i8* null } #0
@"default:A2.ti" = global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 2, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"default:A1.ti", %ExtensionDef** getelementptr inbounds ([6 x %ExtensionDef*], [6 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 3), i8* null, i8* null } #0
@"default:B.tt" = internal global %TypeTemplate { i8* null, i8 -128, i8 0, i16 0, i16 1, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** getelementptr inbounds ([6 x %ExtensionDef*], [6 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 0), i16 1 }
@"default:B<Int32>.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 1, i16 2, i32* null, i8* bitcast (%TypeTemplate* @"default:B.tt" to i8*), i8* null, i8* null, %TypeInfo* @"default:A2.ti", %ExtensionDef** getelementptr inbounds ([6 x %ExtensionDef*], [6 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 0), i8* null, i8* null } #0

@"default:B<T>_ed_default:A1.ft" = private global [1 x i8*] [i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_2_B to i8*)]
@"default:B<T>_ed_default:A1" = private global %ExtensionDef { i32 1, i8 1, i8 -128, i16 1, i8* bitcast (%TypeTemplate* @"default:B.tt" to i8*), i8* bitcast (%TypeInfo* @"default:A1.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:B<T>_ed_default:A1.ft" to i8*) }
@"default:B<T>_ed_default:A2.ft" = private global [1 x i8*] [i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_1_B to i8*)]
@"default:B<T>_ed_default:A2" = private global %ExtensionDef { i32 1, i8 1, i8 -128, i16 1, i8* bitcast (%TypeTemplate* @"default:B.tt" to i8*), i8* bitcast (%TypeInfo* @"default:A2.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:B<T>_ed_default:A2.ft" to i8*) }
@"default:A1_ed_default:A1.ft" = private global [1 x i8*] [i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_2_A1 to i8*)]
@"default:A1_ed_default:A1" = private global %ExtensionDef { i32 0, i8 1, i8 -128, i16 1, i8* bitcast (%TypeInfo* @"default:A1.ti" to i8*), i8* bitcast (%TypeInfo* @"default:A1.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:A1_ed_default:A1.ft" to i8*) }
@"default:A2_ed_default:A1.ft" = private global [1 x i8*] [i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_2_A1 to i8*)]
@"default:A2_ed_default:A1" = private global %ExtensionDef { i32 0, i8 1, i8 -128, i16 1, i8* bitcast (%TypeInfo* @"default:A2.ti" to i8*), i8* bitcast (%TypeInfo* @"default:A1.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:A2_ed_default:A1.ft" to i8*) }
@"default:A2_ed_default:A2.ft" = private global [1 x i8*] [i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_1_A2 to i8*)]
@"default:A2_ed_default:A2" = private global %ExtensionDef { i32 0, i8 1, i8 -128, i16 1, i8* bitcast (%TypeInfo* @"default:A2.ti" to i8*), i8* bitcast (%TypeInfo* @"default:A2.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:A2_ed_default:A2.ft" to i8*) }

@NonExternalExtensionDefs = private global [6 x %ExtensionDef*] [%ExtensionDef* @"default:B<T>_ed_default:A1", %ExtensionDef* @"default:B<T>_ed_default:A2", %ExtensionDef* @"default:A1_ed_default:A1", %ExtensionDef* @"default:A2_ed_default:A1", %ExtensionDef* @"default:A2_ed_default:A2", %ExtensionDef* null] #4

; CHECK: [[FUNC0:%.*]] = bitcast i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_2_B to i8*) to i64 (i8 addrspace(1)*, %TypeInfo*)*
; CHECK-NEXT: [[TMP2:%.*]] = call i64 [[FUNC0]](i8 addrspace(1)* [[TMP0:%.*]], %TypeInfo* [[TMP1:%.*]])
; CHECK: [[FUNC1:%.*]] = bitcast i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_2_B to i8*) to i64 (i8 addrspace(1)*, %TypeInfo*)*
; CHECK-NEXT: [[TMP3:%.*]] = call i64 [[FUNC1]](i8 addrspace(1)* [[TMP0]], %TypeInfo* [[TMP1]])
; CHECK: [[FUNC2:%.*]] = bitcast i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_1_B to i8*) to i64 (i8 addrspace(1)*, %TypeInfo*)*
; CHECK-NEXT: [[TMP4:%.*]] = call i64 [[FUNC2]](i8 addrspace(1)* [[TMP0]], %TypeInfo* [[TMP1]])
define internal void @callee(i8 addrspace(1)* %ptr) {
  %1 = bitcast i8 addrspace(1)* %ptr to %TypeInfo* addrspace(1)*
  %2 = load %TypeInfo*, %TypeInfo* addrspace(1)* %1, align 8
  %3 = bitcast %TypeInfo* %2 to i8*
  %4 = call i8* @llvm.cj.get.vtable.func(i8* %3, i64 0, i64 0, i8* bitcast (%TypeInfo* @"default:A1.ti" to i8*)), !IntroType !0, !objType !2
  %5 = bitcast i8* %4 to i64 (i8 addrspace(1)*, %TypeInfo*)*
  %6 = call i64 %5(i8 addrspace(1)* %ptr, %TypeInfo* %2)
  %7 = call i8* @llvm.cj.get.vtable.func(i8* %3, i64 0, i64 0, i8* bitcast (%TypeInfo* @"default:A1.ti" to i8*)), !IntroType !0, !objType !2
  %8 = bitcast i8* %7 to i64 (i8 addrspace(1)*, %TypeInfo*)*
  %9 = call i64 %8(i8 addrspace(1)* %ptr, %TypeInfo* %2)
  %10 = call i8* @llvm.cj.get.vtable.func(i8* %3, i64 1, i64 0, i8* bitcast (%TypeInfo* @"default:A2.ti" to i8*)), !IntroType !1, !objType !3
  %11 = bitcast i8* %10 to i64 (i8 addrspace(1)*, %TypeInfo*)*
  %12 = call i64 %11(i8 addrspace(1)* %ptr, %TypeInfo* %2)
  ret void
}

define void @caller() {
  %1 = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @"default:B<Int32>.ti" to i8*), i32 8)
  call void @callee(i8 addrspace(1)* %1)
  ret void
}

define internal i64 @test_1_B(i8 addrspace(1)* %this, %TypeInfo* %outerTI) {
  ret i64 1
}
define internal i64 @test_2_B(i8 addrspace(1)* %this, %TypeInfo* %outerTI) {
  ret i64 0
}
define internal i64 @test_2_A1(i8 addrspace(1)* %this, %TypeInfo* %outerTI) {
  ret i64 0
}
define internal i64 @test_1_A2(i8 addrspace(1)* %this, %TypeInfo* %outerTI) {
  ret i64 0
}

declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32)
declare i8* @llvm.cj.get.vtable.func(i8*, i64, i64, i8*)

attributes #0 = { "CFileKlass" }

!0 = !{!"default:A1"}
!1 = !{!"default:A2"}
!2 = !{!"default:A1.ti"}
!3 = !{!"default:A2.ti"}