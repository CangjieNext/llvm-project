; RUN: opt < %s -passes='globaldce' --cangjie-pipeline -S | FileCheck %s

; interface I1<T> {
;   func func_I11() {}
;   func func_I12() {}
;   func func_I13() {}
; }
; interface I2<T> {
;   func func_I21() {}
;   func func_I22() {}
;   func func_I23() {}
; }
; class A<T> <: I1<T> & I2<I1<T>> {
;   public func func_I1() {}
;   public func func_I22() { func_A() }
;   public func func_A() { println("A::func_A") }
; }
; class B <: I2<Int64> {}
; class C<T> <: I2<T> {}
; public func call_I2_1<T>(v: I2<I1<T>>) { v.func_I22() }

%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8*, %ExtensionDef**, i16 }
%ExtensionDef = type { i32, i8, i8, i16, i8*, i8*, i8*, i8* }
%TypeInfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%Unit.Type = type {}

@"default:A.tt" = private global %TypeTemplate { i8* null, i8 -127, i8 0, i16 0, i16 1, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** getelementptr inbounds ([6 x %ExtensionDef*], [6 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 0), i16 -32768 } #2
@"default:B.tt" = private global %TypeTemplate { i8* null, i8 -127, i8 0, i16 0, i16 1, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** getelementptr inbounds ([6 x %ExtensionDef*], [6 x %ExtensionDef*]* @NonExternalExtensionDefs, i32 0, i32 2), i16 -32768 } #2
@"default:I1.tt" = private global %TypeTemplate { i8* null, i8 -127, i8 0, i16 0, i16 1, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** null, i16 -32768 } #2
@"default:I2.ti" = internal global %TypeInfo { i8* null, i8 -127, i8 0, i16 0, i32 0, i8* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo *null, %ExtensionDef** null, i8* null, i8* null } #1

@"default:A<T>_ed_default:I1<T>.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @_CN7default1AIG_E7func_I1Hv to i8*)]
@"default:A<T>_ed_default:I1<T>" = private global %ExtensionDef { i32 1, i8 0, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:A.tt" to i8*), i8* bitcast (i8* (i32, %TypeInfo**)* @"default:A<T>_ed_default:I1<T>_iFn" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:A<T>_ed_default:I1<T>.ft" to i8*) }, !inheritedType !0 #0
; CHECK: @"default:A<T>_ed_default:I2.ft" = private global [1 x i8*] zeroinitializer
@"default:A<T>_ed_default:I2.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @_CN7default1AIG_E7func_I2Hv to i8*)]
@"default:A<T>_ed_default:I2" = private global %ExtensionDef { i32 1, i8 1, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:A.tt" to i8*), i8* bitcast (%TypeInfo* @"default:I2.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:A<T>_ed_default:I2.ft" to i8*) }, !inheritedType !1 #0
; CHECK: @"default:B<T>_ed_default:A<T>.ft" = private global [1 x i8*] zeroinitializer
@"default:B<T>_ed_default:A<T>.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @_CN7default1BIG_E6func_AHv to i8*)]
@"default:B<T>_ed_default:A<T>" = private global %ExtensionDef { i32 1, i8 0, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:B.tt" to i8*), i8* bitcast (i8* (i32, %TypeInfo**)* @"default:B<T>_ed_default:A<T>_iFn" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:B<T>_ed_default:A<T>.ft" to i8*) }, !inheritedType !2 #0
@"default:B<T>_ed_default:I1<T>.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @_CN7default1BIG_E7func_I1Hv to i8*)]
@"default:B<T>_ed_default:I1<T>" = private global %ExtensionDef { i32 1, i8 0, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:B.tt" to i8*), i8* bitcast (i8* (i32, %TypeInfo**)* @"default:B<T>_ed_default:I1<T>_iFn" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:B<T>_ed_default:I1<T>.ft" to i8*) }, !inheritedType !0 #0
; CHECK: @"default:B<T>_ed_default:I2.ft" = private global [1 x i8*] zeroinitializer
@"default:B<T>_ed_default:I2.ft" = private global [1 x i8*] [i8* bitcast (void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)* @"_CVN7default1AIG_E7func_I2Hv$N7default1BIG_E$CN7default2I2E" to i8*)]
@"default:B<T>_ed_default:I2" = private global %ExtensionDef { i32 1, i8 1, i8 0, i16 0, i8* bitcast (%TypeTemplate* @"default:B.tt" to i8*), i8* bitcast (%TypeInfo* @"default:I2.ti" to i8*), i8* null, i8* bitcast ([1 x i8*]* @"default:B<T>_ed_default:I2.ft" to i8*) }, !inheritedType !1 #0

@NonExternalExtensionDefs = private global [6 x %ExtensionDef*] [%ExtensionDef* @"default:A<T>_ed_default:I1<T>", %ExtensionDef* @"default:A<T>_ed_default:I2", %ExtensionDef* @"default:B<T>_ed_default:A<T>", %ExtensionDef* @"default:B<T>_ed_default:I1<T>", %ExtensionDef* @"default:B<T>_ed_default:I2", %ExtensionDef* null]
@llvm.used = appending global [1 x i8*] [i8* bitcast ([6 x %ExtensionDef*]* @NonExternalExtensionDefs to i8*)]

declare i8* @llvm.cj.vfe.info(i8*, metadata, i64)

define private i8* @"default:A<T>_ed_default:I1<T>_iFn"(i32 %0, %TypeInfo** %1) {
allocas:
  %2 = alloca [1 x %TypeInfo*], align 8
  br label %prepTi

prepTi:
  %3 = getelementptr %TypeInfo*, %TypeInfo** %1, i32 0
  %4 = load %TypeInfo*, %TypeInfo** %3, align 8
  %5 = getelementptr inbounds [1 x %TypeInfo*], [1 x %TypeInfo*]* %2, i32 0, i32 0
  store %TypeInfo* %4, %TypeInfo** %5, align 8
  %6 = bitcast [1 x %TypeInfo*]* %2 to i8**
  %7 = call i8* @CJ_MCC_GetOrCreateTypeInfo(i8* bitcast (%TypeTemplate* @"default:I1.tt" to i8*), i32 1, i8** %6)
  ret i8* %7
}

define private i8* @"default:B<T>_ed_default:A<T>_iFn"(i32 %0, %TypeInfo** %1) {
allocas:
  %2 = alloca [1 x %TypeInfo*], align 8
  br label %prepTi

prepTi:
  %3 = getelementptr %TypeInfo*, %TypeInfo** %1, i32 0
  %4 = load %TypeInfo*, %TypeInfo** %3, align 8
  %5 = getelementptr inbounds [1 x %TypeInfo*], [1 x %TypeInfo*]* %2, i32 0, i32 0
  store %TypeInfo* %4, %TypeInfo** %5, align 8 
  %6 = bitcast [1 x %TypeInfo*]* %2 to i8**
  %7 = call i8* @CJ_MCC_GetOrCreateTypeInfo(i8* bitcast (%TypeTemplate* @"default:A.tt" to i8*), i32 1, i8** %6)
  ret i8* %7
}


define private i8* @"default:B<T>_ed_default:I1<T>_iFn"(i32 %0, %TypeInfo** %1) {
allocas:
  %2 = alloca [1 x %TypeInfo*], align 8
  br label %prepTi

prepTi:
  %3 = getelementptr %TypeInfo*, %TypeInfo** %1, i32 0
  %4 = load %TypeInfo*, %TypeInfo** %3, align 8
  %5 = getelementptr inbounds [1 x %TypeInfo*], [1 x %TypeInfo*]* %2, i32 0, i32 0
  store %TypeInfo* %4, %TypeInfo** %5, align 8 
  %6 = bitcast [1 x %TypeInfo*]* %2 to i8**
  %7 = call i8* @CJ_MCC_GetOrCreateTypeInfo(i8* bitcast (%TypeTemplate* @"default:I1.tt" to i8*), i32 1, i8** %6)
  ret i8* %7
}

define internal void @_CN7default1AIG_E7func_I1Hv(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
  ret void
}

define internal void @_CN7default1AIG_E7func_I2Hv(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
allocas:
  %1 = alloca %Unit.Type, align 8
  br label %prepTi

prepTi:
  %2 = bitcast i8 addrspace(1)* %this to %TypeInfo* addrspace(1)*
  %ti = load %TypeInfo*, %TypeInfo* addrspace(1)* %2, align 8
  %3 = bitcast %TypeInfo* %ti to i8*
  %4 = bitcast %TypeInfo* %outerTI to i8*
  %5 = call i8* @CJ_MCC_GetMTable(i8* %3, i8* %4)
  %6 = bitcast i8* %5 to i8**
  %7 = load i8*, i8** %6, align 8
  %8 = call i8* @llvm.cj.vfe.info(i8* %7, metadata !2, i64 0)
  %9 = bitcast i8* %8 to void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)*
  %ti1 = load %TypeInfo*, %TypeInfo* addrspace(1)* %2, align 8
  call void %9(%Unit.Type* noalias sret(%Unit.Type) %1, i8 addrspace(1)* %this, %TypeInfo* %ti1)
  ret void
}

define internal void @_CN7default1BIG_E6func_AHv(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
  ret void
}

define internal void @_CN7default1BIG_E7func_I1Hv(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %this, %TypeInfo* %outerTI) gc "cangjie" {
  ret void
}

define internal void @"_CVN7default1AIG_E7func_I2Hv$N7default1BIG_E$CN7default2I2E"(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %1, %TypeInfo* %outerTI) gc "cangjie" {
allocas:
  %2 = alloca %Unit.Type, align 8
  %3 = alloca [1 x %TypeInfo*], align 8
  br label %prepTi

prepTi:
  %ti.typeArgs = getelementptr inbounds %TypeInfo, %TypeInfo* %outerTI, i32 0, i32 12
  %4 = load i8*, i8** %ti.typeArgs, align 8
  %5 = bitcast i8* %4 to %TypeInfo**
  %6 = getelementptr inbounds %TypeInfo*, %TypeInfo** %5, i32 0
  %7 = load %TypeInfo*, %TypeInfo** %6, align 8
  %8 = getelementptr inbounds [1 x %TypeInfo*], [1 x %TypeInfo*]* %3, i32 0, i32 0
  store %TypeInfo* %7, %TypeInfo** %8, align 8
  %9 = bitcast [1 x %TypeInfo*]* %3 to i8**
  %10 = call i8* @CJ_MCC_GetOrCreateTypeInfo(i8* bitcast (%TypeTemplate* @"default:A.tt" to i8*), i32 1, i8** %9)
  %11 = bitcast i8* %10 to %TypeInfo*
  call void @_CN7default1AIG_E7func_I2Hv(%Unit.Type* noalias sret(%Unit.Type) %2, i8 addrspace(1)* %1, %TypeInfo* %11)
  ret void
}

define void @use_I1(%Unit.Type* noalias sret(%Unit.Type) %0, i8 addrspace(1)* %v, %TypeInfo* %ti.T) gc "cangjie" {
allocas:
  %1 = alloca %Unit.Type, align 8
  %2 = alloca [1 x %TypeInfo*], align 8
  br label %prepTi

prepTi:
  %3 = getelementptr inbounds [1 x %TypeInfo*], [1 x %TypeInfo*]* %2, i32 0, i32 0
  store %TypeInfo* %ti.T, %TypeInfo** %3, align 8
  %4 = bitcast [1 x %TypeInfo*]* %2 to i8**
  %5 = call i8* @CJ_MCC_GetOrCreateTypeInfo(i8* bitcast (%TypeTemplate* @"default:I1.tt" to i8*), i32 1, i8** %4)
  %6 = bitcast i8 addrspace(1)* %v to %TypeInfo* addrspace(1)*
  %ti = load %TypeInfo*, %TypeInfo* addrspace(1)* %6, align 8
  %7 = bitcast %TypeInfo* %ti to i8*
  %8 = call i8* @CJ_MCC_GetMTable(i8* %7, i8* %5)
  %9 = bitcast i8* %8 to i8**
  %10 = load i8*, i8** %9, align 8
  %11 = call i8* @llvm.cj.vfe.info(i8* %10, metadata !0, i64 0)
  %12 = bitcast i8* %11 to void (%Unit.Type*, i8 addrspace(1)*, %TypeInfo*)*
  %ti1 = load %TypeInfo*, %TypeInfo* addrspace(1)* %6, align 8
  call void %12(%Unit.Type* noalias sret(%Unit.Type) %1, i8 addrspace(1)* %v, %TypeInfo* %ti1)
  ret void
}

declare i8* @CJ_MCC_GetOrCreateTypeInfo(i8*, i32, i8**)
declare i8* @CJ_MCC_GetMTable(i8*, i8*)

attributes #0 = { "CFileMTable" }
attributes #1 = { "CFileKlass" }
attributes #2 = { "cj_tt" }

!0 = !{!"default:I1.tt"}
!1 = !{!"default:I2.ti"}
!2 = !{!"default:A.tt"}
!3 = !{i64 0}