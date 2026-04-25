; RUN: opt < %s -insert-cj-tbaa -licm --cangjie-pipeline -S | FileCheck %s

%record.OptionArray = type { i1, %record.ArrayIbE }
%record.ArrayIbE = type { i8 addrspace(1)*, i64, i64 }
%ArrayLayout.Int64 = type { %ArrayBase, [0 x i64] }
%ArrayBase = type { i8 addrspace(1)*, i64 }

@test_type_0 = global %record.OptionArray zeroinitializer

%test_type_1 = type { i8, %record.OptionArray }
@test_global_val = global %test_type_1 zeroinitializer

declare i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** nocapture) #1

; CHECK-LABEL: define i64 @foo1(
; CHECK: bb0:
; CHECK: [[HOISTED:%[0-9]+]] = load i64, i64 addrspace(1)* {{%[0-9]+}}, align 1, !tbaa !10
; CHECK: br label %bb1
; CHECK: bb4:
; CHECK-NEXT: {{%[0-9]+}} = add nuw nsw i64 %.ret, [[HOISTED]]
; CHECK-NEXT: {{%[0-9]+}} = icmp eq i64 {{%[0-9]+}}, 10000
; CHECK-NEXT: br i1 %14, label %bb6, label %bb1
define i64 @foo1() {
bb0:
  br label %bb1

bb1:
  %0 = phi i64 [0, %bb0], [%1, %bb4]
  %.ret = phi i64 [0, %bb0], [%13, %bb4]
  %1 = add nuw nsw i64 %0, 1
  %2 = load i8, i8* bitcast (%record.OptionArray* @test_type_0 to i8*), align 8
  %3 = and i8 %2, 1
  %4 = icmp eq i8 %3, 0
  br i1 %4, label %bb2, label %bb3

bb3:
  unreachable

bb2:
  %5 = load i64, i64* getelementptr inbounds (%record.OptionArray, %record.OptionArray* @test_type_0, i64 0, i32 1, i32 2), align 8
  %6 = icmp eq i64 %5, 8
  br i1 %6, label %bb4, label %bb5

bb5:
  unreachable

bb4:
  %7 = tail call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** getelementptr inbounds (%record.OptionArray, %record.OptionArray* @test_type_0, i64 0, i32 1, i32 0))
  %8 = bitcast i8 addrspace(1)* %7 to %ArrayLayout.Int64 addrspace(1)*
  %9 = load i64, i64* getelementptr inbounds (%record.OptionArray, %record.OptionArray* @test_type_0, i64 0, i32 1, i32 1), align 8
  %10 = add i64 %9, 7
  %11 = getelementptr inbounds %ArrayLayout.Int64, %ArrayLayout.Int64 addrspace(1)* %8, i64 0, i32 1, i64 %10
  %12 = load i64, i64 addrspace(1)* %11, align 1
  %13 = add nuw nsw i64 %.ret, %12
  %14 = icmp eq i64 %1, 10000
  br i1 %14, label %bb6, label %bb1

bb6:
  ret i64 %.ret
}

; CHECK-LABEL: define i64 @foo2(
; CHECK: bb0:
; CHECK: [[HOISTED:%[0-9]+]] = load i64, i64 addrspace(1)* {{%[0-9]+}}, align 1, !tbaa !10
; CHECK: br label %bb1
; CHECK: bb4:
; CHECK-NEXT: {{%[0-9]+}} = add nuw nsw i64 %.ret, [[HOISTED]]
; CHECK-NEXT: {{%[0-9]+}} = icmp eq i64 {{%[0-9]+}}, 10000
; CHECK-NEXT: br i1 %14, label %bb6, label %bb1
define i64 @foo2() {
bb0:
  %.gep0 = getelementptr inbounds %test_type_1, %test_type_1* @test_global_val, i64 0, i32 1
  br label %bb1

bb1:
  %0 = phi i64 [0, %bb0], [%1, %bb4]
  %.ret = phi i64 [0, %bb0], [%13, %bb4]
  %1 = add nuw nsw i64 %0, 1
  %.cast0 = bitcast %record.OptionArray* %.gep0 to i8*
  %2 = load i8, i8* %.cast0, align 8
  %3 = and i8 %2, 1
  %4 = icmp eq i8 %3, 0
  br i1 %4, label %bb2, label %bb3

bb3:
  unreachable

bb2:
  %.gep1 = getelementptr inbounds %record.OptionArray, %record.OptionArray* %.gep0, i64 0, i32 1, i32 2
  %5 = load i64, i64* %.gep1, align 8
  %6 = icmp eq i64 %5, 8
  br i1 %6, label %bb4, label %bb5

bb5:
  unreachable

bb4:
  %7 = tail call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** getelementptr inbounds (%test_type_1, %test_type_1* @test_global_val, i64 0, i32 1, i32 1, i32 0))
  %8 = bitcast i8 addrspace(1)* %7 to %ArrayLayout.Int64 addrspace(1)*
  %.gep2 = getelementptr inbounds %record.OptionArray, %record.OptionArray* %.gep0, i64 0, i32 1, i32 1
  %9 = load i64, i64* %.gep2, align 8
  %10 = add i64 %9, 7
  %11 = getelementptr inbounds %ArrayLayout.Int64, %ArrayLayout.Int64 addrspace(1)* %8, i64 0, i32 1, i64 %10
  %12 = load i64, i64 addrspace(1)* %11, align 1
  %13 = add nuw nsw i64 %.ret, %12
  %14 = icmp eq i64 %1, 10000
  br i1 %14, label %bb6, label %bb1

bb6:
  ret i64 %.ret
}

; CHECK-LABEL: define i64 @foo3(
; CHECK: bb0:
; CHECK: [[HOISTED:%[0-9]+]] = load i64, i64 addrspace(1)* {{%[0-9]+}}, align 1, !tbaa !10
; CHECK: br label %bb1
; CHECK: bb4:
; CHECK-NEXT: {{%[0-9]+}} = add nuw nsw i64 %.ret, [[HOISTED]]
; CHECK-NEXT: {{%[0-9]+}} = icmp eq i64 {{%[0-9]+}}, 10000
; CHECK-NEXT: br i1 %14, label %bb6, label %bb1
define i64 @foo3() {
bb0:
  %.gep0 = getelementptr inbounds %record.OptionArray, %record.OptionArray* @test_type_0, i64 0, i32 1
  br label %bb1

bb1:
  %0 = phi i64 [0, %bb0], [%1, %bb4]
  %.ret = phi i64 [0, %bb0], [%13, %bb4]
  %1 = add nuw nsw i64 %0, 1
  %2 = load i8, i8* bitcast (%record.OptionArray* @test_type_0 to i8*), align 8
  %3 = and i8 %2, 1
  %4 = icmp eq i8 %3, 0
  br i1 %4, label %bb2, label %bb3

bb3:
  unreachable

bb2:
  %5 = load i64, i64* getelementptr inbounds (%record.OptionArray, %record.OptionArray* @test_type_0, i64 0, i32 1, i32 2), align 8
  %6 = icmp eq i64 %5, 8
  br i1 %6, label %bb4, label %bb5

bb5:
  unreachable

bb4:
  %.gep1 = getelementptr inbounds %record.ArrayIbE, %record.ArrayIbE* %.gep0, i64 0, i32 0
  %7 = tail call i8 addrspace(1)* @llvm.cj.gcread.static.ref(i8 addrspace(1)** %.gep1)
  %8 = bitcast i8 addrspace(1)* %7 to %ArrayLayout.Int64 addrspace(1)*
  %9 = load i64, i64* getelementptr inbounds (%record.OptionArray, %record.OptionArray* @test_type_0, i64 0, i32 1, i32 1), align 8
  %10 = add i64 %9, 7
  %11 = getelementptr inbounds %ArrayLayout.Int64, %ArrayLayout.Int64 addrspace(1)* %8, i64 0, i32 1, i64 %10
  %12 = load i64, i64 addrspace(1)* %11, align 1
  %13 = add nuw nsw i64 %.ret, %12
  %14 = icmp eq i64 %1, 10000
  br i1 %14, label %bb6, label %bb1

bb6:
  ret i64 %.ret
}

attributes #1 = { argmemonly nounwind readonly }
