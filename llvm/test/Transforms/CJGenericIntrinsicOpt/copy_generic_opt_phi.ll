; RUN: opt -passes=cj-generic-intrinsic-opt --cangjie-pipeline -S < %s | FileCheck %s

%fake_typeinfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, %fake_typeinfo*, i8*, i8* }

define void @test_func_1(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, %fake_typeinfo* %ti, i1 %cond) {
  %3 = bitcast %fake_typeinfo* %ti to i8*
  %ti.size = getelementptr inbounds i8, i8* %3, i64 12
  %4 = bitcast i8* %ti.size to i32*
  %5 = load i32, i32* %4, align 4
  %6 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %6, i8 addrspace(1)* %this, i8* %3)
  br i1 %cond, label %bb1, label %bb2

bb1:
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %6, i8 addrspace(1)* %1, i8* %3)
  br label %bb2

bb2:
  ; CHECK: %7 = phi i8 addrspace(1)* [ %1, %bb1 ], [ %this, %2 ]
  ; CHECK-NEXT: %8 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  ; CHECK-NEXT: call void @llvm.cj.assign.generic(i8 addrspace(1)* %8, i8 addrspace(1)* %7, i8* %3)
  %7 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %6, i8* %3)
  ret void
}

define void @test_func_2(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, %fake_typeinfo* %ti, i1 %cond) {
  %3 = bitcast %fake_typeinfo* %ti to i8*
  %ti.size = getelementptr inbounds i8, i8* %3, i64 12
  %4 = bitcast i8* %ti.size to i32*
  %5 = load i32, i32* %4, align 4
  %6 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %6, i8 addrspace(1)* %this, i8* %3)
  br i1 %cond, label %bb1, label %bb2

bb1:
  %7 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %this, i8* %3)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %6, i8* %3)
  br label %bb3

bb2:
  %8 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %8, i8 addrspace(1)* %0, i8* %3)
  br label %bb3

bb3:
  ; CHECK: %9 = phi i8 addrspace(1)* [ %this, %bb1 ], [ %0, %bb2 ]
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %9, i8* %3)
  %9 = phi i8 addrspace(1)* [%7, %bb1], [%8, %bb2]
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %9, i8* %3)
  ret void
}

define void @test_func_3(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, %fake_typeinfo* %ti, i1 %cond) {
  %3 = bitcast %fake_typeinfo* %ti to i8*
  %ti.size = getelementptr inbounds i8, i8* %3, i64 12
  %4 = bitcast i8* %ti.size to i32*
  %5 = load i32, i32* %4, align 4
  %6 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %6, i8 addrspace(1)* %this, i8* %3)
  br i1 %cond, label %bb1, label %bb2

bb1:
  %7 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %this, i8* %3)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %6, i8* %3)
  call void @clobber(i8 addrspace(1)* %7)
  br label %bb3

bb2:
  %8 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %8, i8 addrspace(1)* %0, i8* %3)
  call void @clobber(i8 addrspace(1)* %0)
  br label %bb3

bb3:
  ; CHECK: %9 = phi i8 addrspace(1)* [ %7, %bb1 ], [ %8, %bb2 ]
  ; CHECK-NEXT: call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %9, i8* %3)
  %9 = phi i8 addrspace(1)* [%7, %bb1], [%8, %bb2]
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %9, i8* %3)
  ret void
}

define void @test_func_4(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, %fake_typeinfo* %ti, i1 %cond) {
  %3 = bitcast %fake_typeinfo* %ti to i8*
  %ti.size = getelementptr inbounds i8, i8* %3, i64 12
  %4 = bitcast i8* %ti.size to i32*
  %5 = load i32, i32* %4, align 4
  %6 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %6, i8 addrspace(1)* %this, i8* %3)
  br i1 %cond, label %bb1, label %bb2

bb1:
  %7 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %this, i8* %3)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %6, i8* %3)
  call void @clobber(i8 addrspace(1)* %7)
  br label %bb3

bb2:
  %8 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %8, i8 addrspace(1)* %0, i8* %3)
  br label %bb3

bb3:
  ; CHECK: %9 = phi i8 addrspace(1)* [ %7, %bb1 ], [ %0, %bb2 ]
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %9, i8* %3)
  %9 = phi i8 addrspace(1)* [%7, %bb1], [%8, %bb2]
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %9, i8* %3)
  ret void
}

define void @test_func_5(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, %fake_typeinfo* %ti1, %fake_typeinfo* %ti2, i1 %cond1, i1 %cond2) {
  %3 = bitcast %fake_typeinfo* %ti1 to i8*
  %ti.size = getelementptr inbounds i8, i8* %3, i64 12
  %4 = bitcast i8* %ti.size to i32*
  %5 = load i32, i32* %4, align 4
  %6 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %6, i8 addrspace(1)* %this, i8* %3)
  br i1 %cond1, label %bb1, label %bb2

bb1:
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %this, i8* %3)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %6, i8* %3)
  call void @clobber(i8 addrspace(1)* %1)
  br label %bb3

bb2:
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %0, i8* %3)
  br label %bb3

bb3:
  ; CHECK: [[PHI:%.*]] = phi i8 addrspace(1)* [ %0, %bb2 ], [ %1, %bb1 ]
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* [[T0:%.*]], i8 addrspace(1)* [[PHI]], i8* %3)
  %7 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %3, i32 %5)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %7, i8 addrspace(1)* %1, i8* nonnull %3)
  br i1 %cond2, label %bb4, label %common.ret

bb4:
  %8 = bitcast %fake_typeinfo* %ti2 to i8*
  %ti.size2 = getelementptr inbounds i8, i8* %8, i64 12
  %9 = bitcast i8* %ti.size2 to i32*
  %10 = load i32, i32* %9, align 4
  %11 = call i8 addrspace(1)* @CJ_MCC_NewObject(i8* %8, i32 %10)
  %12 = call i64 @get_offsets(i8* %3)
  %13 = getelementptr inbounds i8, i8 addrspace(1)* %7, i64 %12
  ; CHECK: [[GEP0:%.*]] = getelementptr i8, i8 addrspace(1)* [[PHI]], i64 [[T1:%.*]]
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* [[T2:%.*]], i8 addrspace(1)* [[PHI]], i8 addrspace(1)* [[GEP0]], i32 [[T2:%.*]])
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %11, i8 addrspace(1)* %7, i8 addrspace(1)* %13, i32 %10)
  %14 = bitcast i8 addrspace(1)* %this to i8 addrspace(1)* addrspace(1)*
  %15 = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %14, align 8
  ; CHECK: [[GEP1:%.*]] = getelementptr i8, i8 addrspace(1)* [[PHI]], i64 [[T1]]
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* [[T3:%.*]], i8 addrspace(1)* [[PHI]], i8 addrspace(1)* [[GEP1]], i32 [[T2]])
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %15, i8 addrspace(1)* %11, i8* %8)
  br label %common.ret

common.ret:
  ret void
}

define void @test_func_6(i8 addrspace(1)* noalias %base, i8 addrspace(1)* noalias %mid, i8 addrspace(1)* noalias %dst,
                         %fake_typeinfo* %ti0, %fake_typeinfo* %ti1, i64 %offset, i1 %cond) {
  %ti0.i8 = bitcast %fake_typeinfo* %ti0 to i8*
  %ti.size0 = getelementptr inbounds i8, i8* %ti0.i8, i64 12
  %ti.size0.ptr = bitcast i8* %ti.size0 to i32*
  %size0 = load i32, i32* %ti.size0.ptr, align 4
  %ti1.i8 = bitcast %fake_typeinfo* %ti1 to i8*
  %ti.size1 = getelementptr inbounds i8, i8* %ti1.i8, i64 12
  %ti.size1.ptr = bitcast i8* %ti.size1 to i32*
  %size1 = load i32, i32* %ti.size1.ptr, align 4
  br i1 %cond, label %left, label %right

left:
  %src.left = getelementptr inbounds i8, i8 addrspace(1)* %base, i64 8
  br label %merge

right:
  %src.right = getelementptr inbounds i8, i8 addrspace(1)* %base, i64 8
  br label %merge

merge:
  ; CHECK-LABEL: @test_func_6(
  ; CHECK: [[SRC:%.*]] = phi i8 addrspace(1)* [ %src.left, %left ], [ %src.right, %right ]
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %mid, i8 addrspace(1)* %base, i8 addrspace(1)* [[SRC]], i32 %size0)
  ; CHECK-NOT: sub i64 %offset, 8
  ; CHECK: [[FIELD:%.*]] = getelementptr i8, i8 addrspace(1)* %base, i64 %offset
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %dst, i8 addrspace(1)* %base, i8 addrspace(1)* [[FIELD]], i32 %size1)
  %src = phi i8 addrspace(1)* [ %src.left, %left ], [ %src.right, %right ]
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %mid, i8 addrspace(1)* %base, i8 addrspace(1)* %src, i32 %size0)
  %field = getelementptr inbounds i8, i8 addrspace(1)* %mid, i64 %offset
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %dst, i8 addrspace(1)* %mid, i8 addrspace(1)* %field, i32 %size1)
  ret void
}

declare void @clobber(i8 addrspace(1)*)
declare i64 @get_offsets(i8*) #1
declare void @llvm.cj.assign.generic(i8 addrspace(1)* noalias nocapture writeonly %0, i8 addrspace(1)* noalias nocapture readonly %1, i8* noalias nocapture readonly %2) #0
declare void @llvm.cj.gcread.generic(i8 addrspace(1)* noalias nocapture writeonly %0, i8 addrspace(1)* nocapture readonly %1, i8 addrspace(1)* nocapture readonly %2, i32 %3) #0
declare void @llvm.cj.gcwrite.generic(i8 addrspace(1)* nocapture %0, i8 addrspace(1)* nocapture writeonly %1, i8 addrspace(1)* noalias nocapture readonly %2, i32 %3) #0
declare noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32) #2
declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* nocapture, i8 addrspace(1)* addrspace(1)* nocapture) #1

attributes #0 = { argmemonly mustprogress nounwind willreturn }
attributes #1 = { argmemonly nounwind readonly willreturn }
attributes #2 = { nounwind "cj-heapmalloc" "cj-runtime" }
