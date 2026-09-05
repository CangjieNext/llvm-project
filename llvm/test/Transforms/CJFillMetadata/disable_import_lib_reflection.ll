; RUN: opt '-passes=cj-disable-import-lib-reflection,globaldce' -S < %s | FileCheck %s
; RUN: opt '-passes=cj-disable-import-lib-reflection' -S < %s | FileCheck %s --check-prefix=ERASE

%BitMap = type { i32, [0 x i8] }
%ExtensionDef = type { i32, i8, i8*, i8*, i8*, i8* }
%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8*, %ExtensionDef**, i16 }
%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%DefaultHasher.ti.reflectType = type { i8* }
%"std.core:CPointerHandle.tt.reflectType" = type { i8* }
%test.pkgInfoType = type { i8*, i32, i32, i32, i32, i64, i8*, i8*, i8*, i8*, i8*, i8*, i8* }

@"DefaultHasher.ti.reflect" = internal global %DefaultHasher.ti.reflectType { i8* null }, align 8 #1
@"std.core:CPointerHandle.tt.reflect" = internal global %"std.core:CPointerHandle.tt.reflectType" { i8* null }, align 8 #1
@"DefaultHasher.ti" = global %TypeInfo { i8* null, i8 22, i8 64, i16 1, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* bitcast (%DefaultHasher.ti.reflectType* @"DefaultHasher.ti.reflect" to i8*) }, !RelatedType !0 #0
@"std.core:CPointerHandle.tt" = global %TypeTemplate { i8* null, i8 22, i8 64, i16 0, i16 0, i8* null, i8* null, i8* null, i8* bitcast (%"std.core:CPointerHandle.tt.reflectType"* @"std.core:CPointerHandle.tt.reflect" to i8*), %ExtensionDef** null, i16 0 } #2
@test_test.packageInfo = global %test.pkgInfoType { i8* null, i32 1, i32 1, i32 0, i32 0, i64 1, i8* null, i8* null, i8* null, i8* null, i8* null, i8* bitcast (%TypeInfo* @"DefaultHasher.ti" to i8*), i8* bitcast (%TypeTemplate* @"std.core:CPointerHandle.tt" to i8*) }, no_sanitize_address #1

; Multi-level chain: .reflect -> .tt.method0 -> .actualParam -> .paramName, all
; reclaimed by the fixpoint once the head payload is downgraded (only
; fieldnames survives, referenced by the new .dbg global).
%Bar.ti.reflectType = type { i8*, i8*, i8*, i8* }
@"Bar.ti.reflect" = internal global %Bar.ti.reflectType {
  i8* bitcast ([3 x i8]* @"Bar.fieldnames" to i8*),
  i8* bitcast ([7 x i8]* @"Bar.methodName" to i8*),
  i8* bitcast ([10 x i8]* @"Bar.annoName" to i8*),
  i8* bitcast (%Bar.tt.method0Type* @"Bar.tt.method0" to i8*)
}, align 8 #1
%Bar.tt.method0Type = type { i8*, i8* }
@"Bar.tt.method0" = internal global %Bar.tt.method0Type {
  i8* bitcast ([7 x i8]* @"Bar.methodName" to i8*),
  i8* bitcast (%Bar.tt.actualParamType* @"Bar.tt.method0.actualParam" to i8*)
}, align 8 #1
%Bar.tt.actualParamType = type { i8* }
@"Bar.tt.method0.actualParam" = internal global %Bar.tt.actualParamType {
  i8* bitcast ([12 x i8]* @"Bar.paramName" to i8*)
}, align 8 #1
@"Bar.paramName" = internal global [12 x i8] c"actualParam0", align 1 #1
@"Bar.fieldnames" = internal global [3 x i8] c"Bar", align 1 #1
@"Bar.methodName" = internal global [7 x i8] c"method0", align 1 #1
@"Bar.annoName" = internal global [10 x i8] c"annotation", align 1 #1
@"Bar.ti" = global %TypeInfo { i8* null, i8 22, i8 64, i16 1, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* bitcast (%Bar.ti.reflectType* @"Bar.ti.reflect" to i8*) }, !RelatedType !1 #0

define i8* @keep_ti() {
  ret i8* bitcast (%TypeInfo* @"DefaultHasher.ti" to i8*)
}

define i8* @keep_tt() {
  ret i8* bitcast (%TypeTemplate* @"std.core:CPointerHandle.tt" to i8*)
}

; With GlobalDCE: TI/TT flags cleared and reflection pointers nulled/downgraded,
; all dead reflection data gone. External globals (packageInfo, TI) survive.
; CHECK-NOT: @DefaultHasher.ti.reflect =
; CHECK-NOT: @"std.core:CPointerHandle.tt.reflect" =
; CHECK: @DefaultHasher.ti = global %TypeInfo { i8* null, i8 22, i8 0, i16 1, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null }, !RelatedType !0
; CHECK: @"std.core:CPointerHandle.tt" = global %TypeTemplate { i8* null, i8 22, i8 0, i16 0, i16 0, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** null, i16 0 }
; CHECK: @test_test.packageInfo = global %test.pkgInfoType { i8* null, i32 1, i32 1, i32 0, i32 0, i64 0
; CHECK-NOT: @Bar.ti.reflect =
; CHECK: @Bar.fieldnames = internal global [3 x i8] c"Bar"
; CHECK-NOT: @Bar.tt.method0 =
; CHECK-NOT: @Bar.tt.method0.actualParam =
; CHECK-NOT: @Bar.paramName
; CHECK: @Bar.ti = global %TypeInfo { i8* null, i8 22, i8 0, i16 1, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* bitcast ({ i8* }* @Bar.ti.reflect.dbg to i8*) }, !RelatedType !1
; CHECK-NOT: @Bar.methodName
; CHECK-NOT: @Bar.annoName
; CHECK: @Bar.ti.reflect.dbg = internal global { i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @Bar.fieldnames

; Without GlobalDCE (-O0 FullLTO): the pass self-erases the dead reflection
; subgraph; .dbg and fieldnames survive (still referenced), packageInfo stays
; (external) with its flag cleared.
; ERASE-NOT: @DefaultHasher.ti.reflect =
; ERASE-NOT: @"std.core:CPointerHandle.tt.reflect" =
; ERASE: @DefaultHasher.ti = global %TypeInfo { i8* null, i8 22, i8 0, i16 1, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null }, !RelatedType !0
; ERASE: @test_test.packageInfo = global %test.pkgInfoType { i8* null, i32 1, i32 1, i32 0, i32 0, i64 0
; ERASE-NOT: @Bar.ti.reflect =
; ERASE-NOT: @Bar.tt.method0 =
; ERASE-NOT: @Bar.tt.method0.actualParam =
; ERASE-NOT: @Bar.paramName
; ERASE: @Bar.fieldnames = internal global [3 x i8] c"Bar"
; ERASE-NOT: @Bar.methodName
; ERASE-NOT: @Bar.annoName
; ERASE: @Bar.ti.reflect.dbg = internal global { i8* } { i8* getelementptr inbounds ([3 x i8], [3 x i8]* @Bar.fieldnames

attributes #0 = { "CFileKlass" }
attributes #1 = { "CFileReflect" }
attributes #2 = { "cj_tt" }

!0 = !{!"record.DefaultHasher"}
!1 = !{!"record.Bar"}
