; RUN: opt -cj-rewrite-statepoint -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint -S < %s | FileCheck %s

%BitMap = type { i32, [0 x i8] }
%KlassInfo.0 = type { i32, i32, %BitMap*, %KlassInfo.0*, i8**, i64*, i64*, i8*, i64*, i32, [0 x %KlassInfo.0*] }
%ObjLayout.Object = type { %KlassInfo.0* }
%ArrayBase = type { %ObjLayout.Object, i64 }

@"_ZN8std$core6FutureIuE.objKlass" = external global %KlassInfo.0
@"_ZN8std$core6FutureIuE.refArrayKlass" = internal global %KlassInfo.0 { i32 0, i32 0, %BitMap* null, %KlassInfo.0* @"_ZN8std$core6FutureIuE.objKlass", i8** null, i64* null, i64* null, i8* null, i64* null, i32 0, [0 x %KlassInfo.0*] zeroinitializer }

declare i8 addrspace(1)* @CJ_MCC_GetObjClass(i8 addrspace(1)*)
declare i8 addrspace(1)* @CJ_MCC_NewObjectStub(i8*, i32)
declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly, i8, i64, i1 immarg)

; CHECK-LABEL: bb6:
; CHECK-NEXT:    %phi.val.base = phi i8 addrspace(1)* [ null, %entry ], [ [[HEAP:%[0-9]+]], %bb5 ], !is_base_value !1
; CHECK-NEXT:    %phi.val = phi i8 addrspace(1)* [ [[STACK:%[0-9]+]], %entry ], [ [[HEAP]], %bb5 ]
; CHECK-NEXT:    [[TMP0:%.*]] = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, i8 addrspace(1)* (i8 addrspace(1)*)* @CJ_MCC_GetObjClass, i32 1, i32 0, i8 addrspace(1)* %phi.val) [ "gc-live"(i8 addrspace(1)* %phi.val.base, i8 addrspace(1)* %phi.val), "struct-live"({ %ArrayBase, [16 x i8 addrspace(1)*] }* %0) ]

define i8 addrspace(1)* @foo(i64 %w) #0 gc "cangjie" {
entry:
  %0 = alloca { %ArrayBase, [16 x i8 addrspace(1)*] }, align 8, !ea_val !0
  %1 = bitcast { %ArrayBase, [16 x i8 addrspace(1)*] }* %0 to i8**
  store i8* bitcast (%KlassInfo.0* @"_ZN8std$core6FutureIuE.refArrayKlass" to i8*), i8** %1, align 8
  %2 = getelementptr inbounds { %ArrayBase, [16 x i8 addrspace(1)*] }, { %ArrayBase, [16 x i8 addrspace(1)*] }* %0, i64 0, i32 1
  %3 = bitcast [16 x i8 addrspace(1)*]* %2 to i8*
  call void @llvm.memset.p0i8.i64(i8* nonnull %3, i8 0, i64 128, i1 false)
  %4 = bitcast { %ArrayBase, [16 x i8 addrspace(1)*] }* %0 to i8*
  %5 = addrspacecast i8* %4 to i8 addrspace(1)*
  %icmp1 = icmp sgt i64 %w, 0
  br i1 %icmp1, label %bb5, label %bb6

bb5:                                  ; preds = %entry
  %6 = call i8 addrspace(1)* @CJ_MCC_NewObjectStub(i8* bitcast (%KlassInfo.0* @"_ZN8std$core6FutureIuE.objKlass" to i8*), i32 24)
  br label %bb6

bb6:                                   ; preds = %entry, %bb5
  %phi.val = phi i8 addrspace(1)* [ %5, %entry ], [ %6, %bb5 ]
  %7 = call i8 addrspace(1)* @CJ_MCC_GetObjClass(i8 addrspace(1)* %phi.val)
  br label %for

for:                                              ; preds = %bb6, %for
  %phi.val2 = phi i8 addrspace(1)* [ %7, %bb6 ], [ %result, %for ]
  %result = call i8 addrspace(1)* @CJ_MCC_GetObjClass(i8 addrspace(1)* %phi.val2)
  %loop.cond = icmp ult i64 %w, 20
  br i1 %loop.cond, label %for, label %end

end:                                    ; preds = %for
  ret i8 addrspace(1)* %result
}

!0 = !{}
