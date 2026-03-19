; RUN: opt -passes=cj-generic-intrinsic-opt --cangjie-pipeline -S < %s | FileCheck %s

%fake_typeinfo = type { i8*, i8, i8, i16, i32, i8*, i32, i8, i8, i32*, i8*, i8*, i8*, %fake_typeinfo*, i8*, i8* }

define void @test_func_1(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, i8 addrspace(1)* noalias %2, i8* %ti) {
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %0, i8 addrspace(1)* %this, i8* %ti)
  ; CHECK-NEXT: call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %this, i8* %ti)
  ; CHECK-NEXT: call void @llvm.cj.assign.generic(i8 addrspace(1)* %2, i8 addrspace(1)* %this, i8* %ti)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %0, i8 addrspace(1)* %this, i8* %ti)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %0, i8* %ti)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %2, i8 addrspace(1)* %1, i8* %ti)
  ret void
}

define void @test_func_2(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, i8 addrspace(1)* noalias %2,
                         %fake_typeinfo* %ti0, %fake_typeinfo* %ti1, %fake_typeinfo* %ti2) {
  %4 = getelementptr inbounds i8, i8 addrspace(1)* %this, i64 8
  %5 = bitcast %fake_typeinfo* %ti0 to i8*
  %ti.size0 = getelementptr inbounds i8, i8* %5, i64 12
  %6 = bitcast i8* %ti.size0 to i32*
  %7 = load i32, i32* %6, align 4
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %0, i8 addrspace(1)* %this, i8 addrspace(1)* %4, i32 %7)
  %8 = getelementptr inbounds i8, i8 addrspace(1)* %0, i64 8
  %9 = bitcast %fake_typeinfo* %ti1 to i8*
  %ti.size1 = getelementptr inbounds i8, i8* %9, i64 12
  %10 = bitcast i8* %ti.size1 to i32*
  %11 = load i32, i32* %10, align 4
  ; CHECK: %12 = getelementptr i8, i8 addrspace(1)* %this, i64 8
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %this, i8 addrspace(1)* %12, i32 %11)
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %0, i8 addrspace(1)* %8, i32 %11)
  %12 = getelementptr inbounds i8, i8 addrspace(1)* %1, i64 8
  %13 = bitcast %fake_typeinfo* %ti2 to i8*
  %ti.size2 = getelementptr inbounds i8, i8* %13, i64 12
  %14 = bitcast i8* %ti.size2 to i32*
  %15 = load i32, i32* %14, align 4
  ; CHECK: %17 = getelementptr i8, i8 addrspace(1)* %this, i64 8
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %2, i8 addrspace(1)* %this, i8 addrspace(1)* %17, i32 %16)
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %2, i8 addrspace(1)* %1, i8 addrspace(1)* %12, i32 %15)
  ret void
}

define void @test_func_3(i8 addrspace(1)* noalias nocapture writeonly sret(i8) %0, i8 addrspace(1)* nocapture %this, %fake_typeinfo* nocapture readonly %outerTI) {
  %ti.typeArgs = getelementptr inbounds %fake_typeinfo, %fake_typeinfo* %outerTI, i64 0, i32 11
  %2 = bitcast i8** %ti.typeArgs to %fake_typeinfo***
  %3 = load %fake_typeinfo**, %fake_typeinfo*** %2, align 8
  %4 = load %fake_typeinfo*, %fake_typeinfo** %3, align 8
  %ti.size1 = getelementptr inbounds %fake_typeinfo, %fake_typeinfo* %4, i64 0, i32 4
  %5 = load i32, i32* %ti.size1, align 4
  %6 = add i32 %5, 15
  %7 = and i32 %6, -8
  %8 = bitcast %fake_typeinfo* %4 to i8*
  %9 = tail call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %8, i32 %7)

  %10 = getelementptr i8, i8 addrspace(1)* %this, i64 8
  %11 = bitcast i8 addrspace(1)* %10 to i8 addrspace(1)* addrspace(1)*
  %12 = tail call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %this, i8 addrspace(1)* addrspace(1)* %11)
  %13 = getelementptr inbounds i8, i8 addrspace(1)* %this, i64 16
  %14 = bitcast i8 addrspace(1)* %13 to i64 addrspace(1)*
  %15 = load i64, i64 addrspace(1)* %14, align 8
  %sext = sext i32 %5 to i64
  %16 = mul i64 %15, %sext
  %17 = add i64 %16, 16
  %18 = getelementptr inbounds i8, i8 addrspace(1)* %12, i64 %17
  tail call void @llvm.cj.gcread.generic(i8 addrspace(1)* %9, i8 addrspace(1)* %12, i8 addrspace(1)* %18, i32 %5)
  %19 = tail call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %8, i32 %7)
  ; CHECK: %20 = getelementptr i8, i8 addrspace(1)* %12, i64 %17
  ; CHECK: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %19, i8 addrspace(1)* %12, i8 addrspace(1)* %20, i32 %5)
  tail call void @llvm.cj.assign.generic(i8 addrspace(1)* %19, i8 addrspace(1)* %9, i8* %8)
  %20 = tail call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %8, i32 %7)
  ; CHECK: %22 = getelementptr i8, i8 addrspace(1)* %12, i64 %17
  ; CHECK: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %21, i8 addrspace(1)* %12, i8 addrspace(1)* %22, i32 %5)
  tail call void @llvm.cj.assign.generic(i8 addrspace(1)* %20, i8 addrspace(1)* %19, i8* %8)
  ret void
}

define void @test_func_4(i8 addrspace(1)* noalias nocapture %this, i8 addrspace(1)* noalias nocapture %0, %fake_typeinfo* nocapture readonly %outerTI) {
  %ti.typeArgs = getelementptr inbounds %fake_typeinfo, %fake_typeinfo* %outerTI, i64 0, i32 11
  %2 = bitcast i8** %ti.typeArgs to %fake_typeinfo***
  %3 = load %fake_typeinfo**, %fake_typeinfo*** %2, align 8
  %4 = load %fake_typeinfo*, %fake_typeinfo** %3, align 8
  %ti.size1 = getelementptr inbounds %fake_typeinfo, %fake_typeinfo* %4, i64 0, i32 4
  %5 = load i32, i32* %ti.size1, align 4
  %6 = add i32 %5, 15
  %7 = and i32 %6, -8
  ; CHECK: %8 = bitcast %fake_typeinfo* %4 to i8*
  %8 = bitcast %fake_typeinfo* %4 to i8*
  %9 = tail call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %8, i32 %7)
  %10 = getelementptr i8, i8 addrspace(1)* %this, i64 8
  %11 = bitcast i8 addrspace(1)* %10 to i8 addrspace(1)* addrspace(1)*
  %12 = tail call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %this, i8 addrspace(1)* addrspace(1)* %11)
  %13 = getelementptr inbounds i8, i8 addrspace(1)* %this, i64 16
  %14 = bitcast i8 addrspace(1)* %13 to i64 addrspace(1)*
  %15 = load i64, i64 addrspace(1)* %14, align 8
  %sext = sext i32 %5 to i64
  %16 = mul i64 %15, %sext
  %17 = add i64 %16, 16
  %18 = getelementptr inbounds i8, i8 addrspace(1)* %12, i64 %17
  tail call void @llvm.cj.gcwrite.generic(i8 addrspace(1)* %12, i8 addrspace(1)* %18, i8 addrspace(1)* %0, i32 %5)
  ; CHECK: [[TMP0:%.*]] = bitcast %fake_typeinfo* %4 to i8*
  ; CHECK-NEXT: call void @llvm.cj.assign.generic(i8 addrspace(1)* %9, i8 addrspace(1)* %0, i8* [[TMP0]])
  tail call void @llvm.cj.gcread.generic(i8 addrspace(1)* %9, i8 addrspace(1)* %12, i8 addrspace(1)* %18, i32 %5)
  %19 = tail call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %8, i32 %7)
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %20, i8 addrspace(1)* %0, i8* %8)
  tail call void @llvm.cj.assign.generic(i8 addrspace(1)* %19, i8 addrspace(1)* %9, i8* %8)
  %20 = tail call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %8, i32 %7)
  ; CHECK: call void @llvm.cj.assign.generic(i8 addrspace(1)* %21, i8 addrspace(1)* %0, i8* %8)
  tail call void @llvm.cj.assign.generic(i8 addrspace(1)* %20, i8 addrspace(1)* %19, i8* %8)
  ret void
}

; %ti1<%ti3, %ti0>
define void @test_func_5(%fake_typeinfo* %ti0, %fake_typeinfo* %ti1, %fake_typeinfo* %ti3) {
  %1 = bitcast %fake_typeinfo* %ti0 to i8*
  %ti.size = getelementptr inbounds i8, i8* %1, i64 12
  %2 = bitcast i8* %ti.size to i32*
  %3 = load i32, i32* %2, align 4
  %4 = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %1, i32 %3)
  call void @init_func(i8 addrspace(1)* noalias sret(i8) %4)
  %5 = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %1, i32 %3)
  call void @llvm.cj.assign.generic(i8 addrspace(1)* %5, i8 addrspace(1)* %4, i8* %1)
  %6 = bitcast %fake_typeinfo* %ti1 to i8*
  %ti.size2 = getelementptr inbounds i8, i8* %6, i64 12
  %7 = bitcast i8* %ti.size2 to i32*
  %8 = load i32, i32* %7, align 4
  %9 = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %6, i32 %8)
  %10 = call i64 @get_offsets(i8* %1)
  %11 = add nuw nsw i64 %10, 8
  %12 = getelementptr inbounds i8, i8 addrspace(1)* %5, i64 %11
  ; CHECK: [[TMP1:%.*]] = getelementptr i8, i8 addrspace(1)* %4, i64 %11
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %9, i8 addrspace(1)* %4, i8 addrspace(1)* [[TMP1]], i32 %8)
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %9, i8 addrspace(1)* %5, i8 addrspace(1)* %12, i32 %8)
  %13 = getelementptr inbounds i8, i8 addrspace(1)* %9, i64 8
  %14 = getelementptr inbounds %fake_typeinfo, %fake_typeinfo* %ti3, i64 0, i32 1
  %ti.kind.i = load i8, i8* %14, align 1
  %15 = icmp slt i8 %ti.kind.i, 0
  br i1 %15, label %handle_load_ref, label %handle_load_non_ref

; CHECK: [[ADD1:%.*]] = add i64 8, %11
; CHECK-NEXT: [[SUB1:%.*]] = sub i64 [[ADD1]], 8
; CHECK-NEXT: [[TMP2:%.*]] = getelementptr i8, i8 addrspace(1)* %4, i64 [[SUB1]]
; CHECK-NEXT: [[TMP3:%.*]] = bitcast i8 addrspace(1)* [[TMP2]] to i8 addrspace(1)* addrspace(1)*
; CHECK-NEXT: [[TMP4:%.*]] = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %4, i8 addrspace(1)* addrspace(1)* [[TMP3]])
handle_load_ref:
  %16 = bitcast i8 addrspace(1)* %13 to i8 addrspace(1)* addrspace(1)*
  %17 = call i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* %9, i8 addrspace(1)* addrspace(1)* %16)
  br label %load_exit

; CHECK: [[ADD2:%.*]] = add i64 8, %11
; CHECK-NEXT: [[SUB2:%.*]] = sub i64 [[ADD2]], 8
; CHECK-NEXT: [[TMP5:%.*]] = getelementptr i8, i8 addrspace(1)* %4, i64 [[SUB2]]
; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* [[TMP6:%.*]], i8 addrspace(1)* %4, i8 addrspace(1)* [[TMP5]], i32 %23)
handle_load_non_ref:
  %ti.size3 = getelementptr inbounds %fake_typeinfo, %fake_typeinfo* %ti3, i64 0, i32 4
  %18 = load i32, i32* %ti.size3, align 4
  %19 = bitcast %fake_typeinfo* %ti3 to i8*
  %20 = call noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8* %19, i32 %18)
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %20, i8 addrspace(1)* %9, i8 addrspace(1)* %13, i32 %18)
  br label %load_exit

load_exit:
  %21 = phi i8 addrspace(1)* [ %17, %handle_load_ref ], [ %20, %handle_load_non_ref ]
  ret void
}

define void @test_func_6(i8 addrspace(1)* noalias %this, i8 addrspace(1)* noalias %0, i8 addrspace(1)* noalias %1, i8 addrspace(1)* noalias %2, i64 %offset1, i64 %offset2, %fake_typeinfo* %ti) {
  %ti.size3 = getelementptr inbounds %fake_typeinfo, %fake_typeinfo* %ti, i64 0, i32 4
  %size = load i32, i32* %ti.size3, align 4
  %4 = getelementptr inbounds i8, i8 addrspace(1)* %this, i64 %offset1
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %0, i8 addrspace(1)* %this, i8 addrspace(1)* %4, i32 %size)
  %5 = getelementptr inbounds i8, i8 addrspace(1)* %1, i64 %offset2
  call void @llvm.cj.gcwrite.generic(i8 addrspace(1)* %1, i8 addrspace(1)* %5, i8 addrspace(1)* %0, i32 %size)
  ; CHECK: %7 = getelementptr i8, i8 addrspace(1)* %this, i64 %offset1
  ; CHECK-NEXT: call void @llvm.cj.gcread.generic(i8 addrspace(1)* %2, i8 addrspace(1)* %this, i8 addrspace(1)* %7, i32 %size)
  call void @llvm.cj.gcread.generic(i8 addrspace(1)* %2, i8 addrspace(1)* %1, i8 addrspace(1)* %5, i32 %size)
  ret void
}

declare void @init_func(i8 addrspace(1)* noalias sret(i8))
declare i64 @get_offsets(i8*) #1
declare void @llvm.cj.assign.generic(i8 addrspace(1)* noalias nocapture writeonly %0, i8 addrspace(1)* noalias nocapture readonly %1, i8* noalias nocapture readonly %2) #0
declare void @llvm.cj.gcread.generic(i8 addrspace(1)* noalias nocapture writeonly %0, i8 addrspace(1)* nocapture readonly %1, i8 addrspace(1)* nocapture readonly %2, i32 %3) #0
declare void @llvm.cj.gcwrite.generic(i8 addrspace(1)* nocapture %0, i8 addrspace(1)* nocapture writeonly %1, i8 addrspace(1)* noalias nocapture readonly %2, i32 %3) #0
declare noalias i8 addrspace(1)* @CJ_MCC_NewObject(i8*, i32) #2
declare i8 addrspace(1)* @llvm.cj.gcread.ref(i8 addrspace(1)* nocapture, i8 addrspace(1)* addrspace(1)* nocapture) #1

attributes #0 = { argmemonly mustprogress nounwind willreturn }
attributes #1 = { argmemonly nounwind readonly willreturn }
attributes #2 = { nounwind "cj-heapmalloc" "cj-runtime" }
