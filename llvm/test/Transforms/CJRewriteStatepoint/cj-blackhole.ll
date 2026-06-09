;RUN: opt < %s -passes=cj-rewrite-statepoint -S 2>&1 | FileCheck %s

%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%BitMap = type { i32, [0 x i8] }
%ExtensionDef = type { i32, i8, i8*, i8*, i8*, i8* }
%Unit.Type = type {}

@"default:cc.ti" = global %TypeInfo { i8* null, i8 -128, i8 64, i16 2, i32 16, %BitMap* inttoptr (i64 -9223372036854775808 to %BitMap*), i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null }

declare i8* @CJ_LLVM_BlackHole(i8* %0)

declare cangjiegccc void @CJ_MCC_HandleSafepoint()

declare void @CJ_MCC_StackCheck()

declare { i64, i1 } @llvm.sadd.with.overflow.i64(i64, i64)

declare i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32)

; CHECK-LABEL: _CN7default3fooHv(
define i64 @_CN7default3fooHv() {
allocas:
  call cangjiegccc void @CJ_MCC_StackCheck()
  %b = alloca i64, align 8
  %c = alloca i64, align 8
  store i64 5, i64* %b, align 8
  %0 = bitcast i64* %b to i8*
  call cangjiegccc void @CJ_MCC_HandleSafepoint()
  %1 = call i8* @CJ_LLVM_BlackHole(i8* nonnull %0)
  %2 = bitcast i8* %1 to i64*
  %3 = load i64, i64* %2, align 8
  %4 = call { i64, i1 } @llvm.sadd.with.overflow.i64(i64 %3, i64 1)
  %.fca.1.extract = extractvalue { i64, i1 } %4, 1
  br i1 %.fca.1.extract, label %codeRepl, label %normal

normal:                                           ; preds = %allocas
  %.fca.0.extract = extractvalue { i64, i1 } %4, 0
  store i64 %.fca.0.extract, i64* %c, align 8
  %5 = bitcast i64* %c to i8*
; CHECK-NOT: %6 = call i8* @CJ_LLVM_BlackHole(i8* nonnull %5)
  %6 = call i8* @CJ_LLVM_BlackHole(i8* nonnull %5)
  ret i64 0

codeRepl:                                         ; preds = %allocas
  unreachable
}

; CHECK-LABEL: _CN7default4foo2HCNY_1AE(
define i8 addrspace(1)* @_CN7default4foo2HCNY_1AE(i8 addrspace(1)* %a) gc "cangjie" {
allocas:
  call cangjiegccc void @CJ_MCC_StackCheck()
  %0 = alloca %Unit.Type, align 8
  call cangjiegccc void @CJ_MCC_HandleSafepoint()
  %1 = tail call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* bitcast (%TypeInfo* @"default:cc.ti" to i8*), i32 24)
  %2 = getelementptr i8, i8 addrspace(1)* %1, i64 8
  %3 = bitcast i8 addrspace(1)* %2 to <2 x i64> addrspace(1)*
  store <2 x i64> <i64 1, i64 10>, <2 x i64> addrspace(1)* %3, align 8
  %4 = bitcast i8 addrspace(1)* %a to %TypeInfo* addrspace(1)*
  %ti = load %TypeInfo*, %TypeInfo* addrspace(1)* %4, align 8
  %5 = getelementptr %TypeInfo, %TypeInfo* %ti, i64 0, i32 15
  %6 = load %ExtensionDef**, %ExtensionDef*** %5, align 8
  %7 = load %ExtensionDef*, %ExtensionDef** %6, align 8
  %8 = getelementptr %ExtensionDef, %ExtensionDef* %7, i64 0, i32 5
  %9 = bitcast i8** %8 to void (%Unit.Type*, i8 addrspace(1)*, i8 addrspace(1)*, %TypeInfo*)***
  %10 = load void (%Unit.Type*, i8 addrspace(1)*, i8 addrspace(1)*, %TypeInfo*)**, void (%Unit.Type*, i8 addrspace(1)*, i8 addrspace(1)*, %TypeInfo*)*** %9, align 8
  %11 = load void (%Unit.Type*, i8 addrspace(1)*, i8 addrspace(1)*, %TypeInfo*)*, void (%Unit.Type*, i8 addrspace(1)*, i8 addrspace(1)*, %TypeInfo*)** %10, align 8
; CHECK: %token7 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%Unit.Type*, i8 addrspace(1)*, i8 addrspace(1)*, %TypeInfo*)* %11, i32 4, i32 0, %Unit.Type* nonnull sret(%Unit.Type) %0, i8 addrspace(1)* %a.reloc5, i8 addrspace(1)* %1, %TypeInfo* %ti) [ "gc-live"(i8 addrspace(1)* %1) ]
  call void %11(%Unit.Type* noalias nonnull sret(%Unit.Type) %0, i8 addrspace(1)* %a, i8 addrspace(1)* %1, %TypeInfo* %ti)
  %12 = addrspacecast i8 addrspace(1)* %1 to i8*
  %13 = call i8* @CJ_LLVM_BlackHole(i8* %12)
  %14 = addrspacecast i8* %13 to i8 addrspace(1)*
  ret i8 addrspace(1)* %14
}
