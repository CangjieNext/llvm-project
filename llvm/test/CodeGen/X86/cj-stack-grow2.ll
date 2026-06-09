; RUN: llc -O0 --cangjie-pipeline -mtriple x86_64-pc-linux-gnu  < %s  | FileCheck %s

%Unit.Type = type { i8 }
%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i32*, i8*, i8*, i8*, %TypeInfo*, i8*, i8* }
%BitMap = type { i32, [0 x i8] }
%string = type { %record, i64 }
%record = type { i8 addrspace(1)*, i64, i64 }
declare void @cj_stack_grow()

define internal void @test_vec_use_fp(%record* nocapture sret(%record) %0, i8 addrspace(1)* nocapture %1, %TypeInfo* nocapture %2, %string* %3, i64 %4, <2 x i64> %5, void (%Unit.Type*, i8 addrspace(1)*, %record*)* %6) #14 gc "cangjie" {
; CHECK-LABEL:   test_vec_use_fp:
; CHECK:         movq	%rdi, -40(%rbp)
; CHECK:         callq	CJ_MCC_StackCheck@PLT
; CHECK-LABEL:   .Ltmp1:
; CHECK:         movdqu	%xmm0, -24(%rbp)
; CHECK:         leaq	-32(%rbp), %rax
; CHECK:         movq	%rax, -48(%rbp)
; CHECK:         callq	cj_stack_grow@PLT
; CHECK-LABEL:   .Ltmp2:
; CHECK:         .long	.Ltmp1-test_vec_use_fp
; CHECK-NEXT:    #[RegIdx: -1, SlotIdx: -1, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 0]
; CHECK:         .long	.Ltmp2-test_vec_use_fp
; CHECK-NEXT:    #[RegIdx: -1, SlotIdx: -1, LNIdx: -1, DerivedStartIdx: -1, SPRegIdx: -1, SPSlotIdx: 1]
; CHECK:         #RegNums: 0
; CHECK:         #SlotsNums: 2
; CHECK:         #Idx[0]: BaseOffset: -40, SlotBits: 0x1[ -40 ]
; CHECK:         #Idx[1]: BaseOffset: -40, SlotBits: 0x3[ -40 -48 ]
; CHECK:         #LineNumbersNums: 0
; CHECK:         #DerivedInfoNums: 0
entry:
  %token = call cangjiegccc token (...) @llvm.cj.gc.statepoint(i64 4, i32 0, void ()* @CJ_MCC_StackCheck, i32 0, i32 0)
  %7 = alloca %record, align 8
  %sroa = getelementptr inbounds %record, %record* %7, i64 0, i32 1
  %8 = bitcast i64* %sroa to <2 x i64>*
  store <2 x i64> %5, <2 x i64>* %8, align 8
  br label %body

body:
  %r.cast = bitcast %record* %7 to i8*
  %token1 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void ()* @cj_stack_grow, i32 0, i32 0)
  call void @llvm.memset.p0i8.i64(i8* nonnull align 8 %r.cast, i8 0, i64 16, i1 false)
  ret void
}

; Function Attrs: argmemonly nocallback nofree nounwind willreturn writeonly
declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly, i8, i64, i1 immarg) #1
declare void @CJ_MCC_StackCheck()
declare token @llvm.cj.gc.statepoint(...)
