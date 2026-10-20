; RUN: opt -passes=cj-runtime-lowering -S < %s | FileCheck %s

; A local-mode array must never take the NewArrayFast inline fast path: the fast path
; allocates from the normal heap TLR AllocBuffer while the local-object root registry
; keeps referencing the memory, so a later GC cycle recycles it underneath the local
; roots and concurrent marking then dereferences overwritten memory. The lowering of
; llvm.cj.malloc.local.array therefore must always take the slow path into the
; local-object allocator (CJ_MCC_NewLocalObjArray), keeping the size operand poisoned
; and never attaching the !fast_new_array marker -- even when length and element size
; are compile-time constants, which is exactly the shape that would enable the fast
; path for a heap array.

%BitMap = type { i32, [0 x i8] }
%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8* }
%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i32*, i8*, i8*, i8*, %TypeInfo*, i8*, i8* }
%ArrayLayout.refArray = type { %ArrayBase, [0 x i8 addrspace(1)*] }
%ArrayBase = type { i64 }

@"RawArray<default:D>.ti" = internal global %TypeInfo { i8* getelementptr inbounds ([20 x i8], [20 x i8]* @"RawArray<default:D>.name", i32 0, i32 0), i8 -126, i8 0, i16 0, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i32* null, i8* bitcast (%TypeTemplate* @RawArray.tt to i8*), i8* null, i8* null, %TypeInfo* @"std.core:Object.ti", i8* null, i8* null }, !RelatedType !0
@"RawArray<default:D>.name" = internal global [20 x i8] c"RawArray<default:D>\00", align 1
@RawArray.tt = external global %TypeTemplate
@"std.core:Object.ti" = external global %TypeInfo

declare i8 addrspace(1)* @llvm.cj.malloc.local.array(i8*, i64, i64)

define i8 addrspace(1)* @local_array_const_length() gc "cangjie" {
; CHECK-LABEL: define i8 addrspace(1)* @local_array_const_length
; CHECK: call noalias i8 addrspace(1)* @CJ_MCC_NewLocalObjArray(i8* bitcast (%TypeInfo* @"RawArray<default:D>.ti" to i8*), i64 100, i64 poison)
; CHECK-NOT: !fast_new_array
entry:
  %r = call noalias i8 addrspace(1)* @llvm.cj.malloc.local.array(i8* bitcast (%TypeInfo* @"RawArray<default:D>.ti" to i8*), i64 100, i64 8)
  ret i8 addrspace(1)* %r
}

!0 = !{!"ArrayLayout.refArray"}
