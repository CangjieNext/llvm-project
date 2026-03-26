; RUN: opt '-passes=cj-runtime-lowering,cj-devirtual-opt' --cangjie-pipeline -S < %s | FileCheck %s

%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%ExtensionDef = type { i32, i8, i8, i16, i8*, i8*, i8*, i8* }

@"std.core:Object.ti" = external global %TypeInfo
@"default:A.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 1, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"std.core:Object.ti", %ExtensionDef** getelementptr inbounds ([3 x %ExtensionDef*], [3 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 1), i8* null, i8* null } #0
@"default:B.ti" = internal global %TypeInfo { i8* null, i8 -128, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 1, i32* null, i8* null, i8* null, i8* null, %TypeInfo* @"default:A.ti", %ExtensionDef** getelementptr inbounds ([3 x %ExtensionDef*], [3 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 0), i8* null, i8* null } #0

@"default:B_ed_default:A.ft" = internal global [1 x i8*] [i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_func_B to i8*)]
@"default:B_ed_default:A" = internal global %ExtensionDef { i32 0, i8 1, i8 -128, i16 1, i8* bitcast (%TypeInfo* @"default:B.ti" to i8*), i8* bitcast (%TypeInfo* @"default:A.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:B_ed_default:A.ft" to i8*) }
@"default:A_ed_default:A.ft" = internal global [1 x i8*] [i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_func_A to i8*)]
@"default:A_ed_default:A" = internal global %ExtensionDef { i32 0, i8 1, i8 -128, i16 1, i8* bitcast (%TypeInfo* @"default:A.ti" to i8*), i8* bitcast (%TypeInfo* @"default:A.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:A_ed_default:A.ft" to i8*) }

@NonExternalExtensionDefs = internal global [3 x %ExtensionDef*] [%ExtensionDef* @"default:B_ed_default:A", %ExtensionDef* @"default:A_ed_default:A", %ExtensionDef* null]

; CHECK: [[TMP0:%.*]] = bitcast i8* bitcast (i64 (i8 addrspace(1)*, %TypeInfo*)* @test_func_B to i8*) to i64 (i8 addrspace(1)*, %TypeInfo*)*
; CHECK-NEXT: [[TMP3:%.*]] = call i64 [[TMP0]](i8 addrspace(1)* [[TMP1:%.*]], %TypeInfo* [[TMP2:%.*]])
define internal i64 @callee(i8 addrspace(1)* %ptr) {
  %1 = bitcast i8 addrspace(1)* %ptr to %TypeInfo* addrspace(1)*
  %2 = load %TypeInfo*, %TypeInfo* addrspace(1)* %1, align 8
  %3 = bitcast %TypeInfo* %2 to i8*
  %4 = call i8* @llvm.cj.get.vtable.func(i8* %3, i64 0, i64 0, i8* bitcast (%TypeInfo* @"default:A.ti" to i8*)), !IntroType !0, !objType !1
  %5 = bitcast i8* %4 to i64 (i8 addrspace(1)*, %TypeInfo*)*
  %6 = call i64 %5(i8 addrspace(1)* %ptr, %TypeInfo* %2)
  ret i64 %6
}

define void @caller() {
  %1 = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @"default:B.ti" to i8*), i32 8)
  %2 = call i64 @callee(i8 addrspace(1)* %1)
  ret void
}

define internal i64 @test_func_B(i8 addrspace(1)* %this, %TypeInfo* %outerTI) {
  ret i64 1
}
define internal i64 @test_func_A(i8 addrspace(1)* %this, %TypeInfo* %outerTI) {
  ret i64 0
}

declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32)
declare i8* @llvm.cj.get.vtable.func(i8*, i64, i64, i8*)

attributes #0 = { "CFileKlass" }

!0 = !{!"default:A"}
!1 = !{!"default:A.ti"}
