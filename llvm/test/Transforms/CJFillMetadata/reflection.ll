; RUN: opt '-passes=cj-fill-metadata' --cangjie-pipeline -S < %s | FileCheck %s

source_filename = "core"

%Int64.Type = type { i64 }
%BitMap = type { i32, [0 x i8] }

%ExtensionDef = type { i32, i8, i8*, i8*, i8*, i8* }
%TypeTemplate = type { i8*, i8, i8, i16, i16, i8*, i8*, i8*, i8*, %ExtensionDef**, i16 }

%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%record.DefaultHasher = type { i64 }

@Int64.ti = external global %TypeInfo, !RelatedType !17 #1
@Unit.ti = external global %TypeInfo, !RelatedType !17 #1

@gIDE = weak_odr constant i64 -1, align 8, !ReflectionGV !19
@"std.core:CPointerHandle.tt" = global %TypeTemplate { i8* null, i8 22, i8 0, i16 2, i16 1, i8* null, i8* null, i8* null, i8* null, %ExtensionDef** null, i16 -32768 }, !Reflection !13 #0
@"DefaultHasher.ti" = global %TypeInfo { i8* null, i8 22, i8 0, i16 1, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null }, !RelatedType !11, !Reflection !4 #1

; CHECK: @pointer.reflectStr = internal global [8 x i8] c"pointer\00", align 1 #2
; CHECK: @"std.core:CPointerHandle.tt.reflect" = internal unnamed_addr global %"std.core:CPointerHandle.tt.reflectType" { i8* bitcast (%reflect.fieldNamesType1* @"std.core:CPointerHandle.tt.fieldnames" to i8*), i32 8, i16 1, i16 0, i32 0, i32 0, i8* null, i8* null, i8* bitcast (%reflect.fieldType* @"std.core:CPointerHandle.tt.field0" to i8*) } #2
; CHECK: @res.reflectStr = internal global [4 x i8] c"res\00", align 1 #2
; CHECK: @DefaultHasher.ti.fieldnames = internal global %reflect.fieldNamesType1 { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @res.reflectStr, i32 0, i32 0) } #2
; CHECK: @finish.reflectStr = internal global [7 x i8] c"finish\00", align 1 #2
; CHECK: @DefaultHasher.ti.method0 = internal global %reflect.methodType { i8* getelementptr inbounds ([7 x i8], [7 x i8]* @finish.reflectStr, i32 0, i32 0), i32 8, i16 0, i16 0, i8* bitcast (void (%record.DefaultHasher*, %TypeInfo*)* @_CNat13DefaultHasher6finishHv to i8*), i8* bitcast (%TypeInfo* @Int64.ti to i8*), i8* null, i8* bitcast (%TypeInfo* @DefaultHasher.ti to i8*), i8* null, i8* null } #2
; CHECK: @reset.reflectStr = internal global [6 x i8] c"reset\00", align 1 #2
; CHECK: @DefaultHasher.ti.method1 = internal global %reflect.methodType { i8* getelementptr inbounds ([6 x i8], [6 x i8]* @reset.reflectStr, i32 0, i32 0), i32 133128, i16 0, i16 0, i8* null, i8* bitcast (%TypeInfo* @Unit.ti to i8*), i8* null, i8* bitcast (%TypeInfo* @DefaultHasher.ti to i8*), i8* null, i8* null } #2
; CHECK: @DefaultHasher.ti.reflect = internal unnamed_addr global %DefaultHasher.ti.reflectType { i8* bitcast (%reflect.fieldNamesType1* @DefaultHasher.ti.fieldnames to i8*), i32 8, i16 0, i16 0, i32 2, i32 0, i8* null, i8* null, i8* bitcast (%reflect.methodType* @DefaultHasher.ti.method0 to i8*), i8* bitcast (%reflect.methodType* @DefaultHasher.ti.method1 to i8*) } #2
; CHECK: @std_std.core.packageInfo = global %std_std.core.pkgInfoType { i8* null, i32 1, i32 1, i32 1, i32 0, i64 1, i8* getelementptr inbounds ([4 x i8], [4 x i8]* @std.reflectStr, i32 0, i32 0), i8* getelementptr inbounds ([9 x i8], [9 x i8]* @std.core.reflectStr, i32 0, i32 0), i8* getelementptr inbounds ([1 x i8], [1 x i8]* @NullString.reflectStr, i32 0, i32 0), i8* null, i8* null, i8* bitcast (%TypeInfo* @DefaultHasher.ti to i8*), i8* bitcast (%TypeTemplate* @"std.core:CPointerHandle.tt" to i8*), i8* bitcast (%reflect.methodType* @foo.staticMethod to i8*) }, no_sanitize_address #2
; CHECK: @std.reflectStr = internal global [4 x i8] c"std\00", align 1 #2
; CHECK: @std.core.reflectStr = internal global [9 x i8] c"std.core\00", align 1 #2
; CHECK: @NullString.reflectStr = internal global [1 x i8] zeroinitializer, align 1 #2
; CHECK: @next.reflectStr = internal global [5 x i8] c"next\00", align 1 #2
; CHECK: @foo.staticMethod.actualParam = internal global %reflect.actualParamType1 { i8* getelementptr inbounds ([1 x i8], [1 x i8]* @NullString.reflectStr, i32 0, i32 0), i32 0, i8* bitcast (%TypeInfo* @DefaultHasher.ti to i8*), i8* null } #2
; CHECK: @foo.staticMethod = internal global %reflect.methodType { i8* getelementptr inbounds ([5 x i8], [5 x i8]* @next.reflectStr, i32 0, i32 0), i32 24, i16 1, i16 0, i8* bitcast (void (%record.DefaultHasher*, i8 addrspace(1)*)* @foo to i8*), i8* bitcast (%TypeInfo* @DefaultHasher.ti to i8*), i8* null, i8* null, i8* bitcast (%reflect.actualParamType1* @foo.staticMethod.actualParam to i8*), i8* null } #2


; CHECK: attributes #2 = { "CFileReflect" }

declare void @llvm.memset.p0i8.i64(i8* nocapture writeonly, i8, i64, i1 immarg)

define internal void @foo(%"record.DefaultHasher"* noalias sret(%"record.DefaultHasher") %0, i8 addrspace(1)* %1) gc "cangjie" {
allocas:
  %2 = alloca %"record.DefaultHasher", align 8
  %3 = bitcast %"record.DefaultHasher"* %2 to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %3, i8 0, i64 8, i1 false)
  ret void
}

define void @_CNat13DefaultHasher6finishHv(%"record.DefaultHasher"* noalias nocapture readonly %this, %TypeInfo* %outerTI) gc "cangjie" {
allocas:
  %0 = alloca i64, align 8
  %1 = bitcast %"record.DefaultHasher"* %this to i8*
  call void @llvm.memset.p0i8.i64(i8* align 8 %1, i8 0, i64 8, i1 false)
  ret void
}

attributes #0 = { "cj_tt" }
attributes #1 = { "CFileKlass" }

!pkg_info = !{!0}
!types = !{!4}
!type_templates = !{!13}
!global_variables = !{!19}
!functions = !{!20}

!0 = !{i32 1, !"", !"std", !"std.core", !"std_std.core", !""}
!1 = !{!"private", !"none"}
!2 = !{}
!3 = !{!"public", !"none"}
!4 = !{!"DefaultHasher.ti", !"", !5, !2, !7, !2, !3}
!5 = !{!6}
!6 = !{i32 0, !"res", !1}
!7 = !{!8, !9}
!8 = !{!"finish", !"Int64.ti", !"_CNat13DefaultHasher6finishHv", !2, !2, !3}
!9 = !{!"reset", !"Unit.ti", !"_CNat13DefaultHasher5resetHv", !2, !2, !10}
!10 = !{!"hasSRet0", !"mut", !"public", !"none"}
!11 = !{!"record.DefaultHasher"}
!12 = !{!"public", !"none"}
!13 = !{!"std.core:CPointerHandle.tt", !"", !14, !2, !2, !2, !12}
!14 = !{!15}
!15 = !{i32 0, !"pointer", !16}
!16 = !{!"immutable", !"public", !"none"}
!17 = !{!"Int64.Type"}
!18 = !{!"immutable", !"none"}
!19 = !{!"INVALID_ID", !"Int64.ti", !"gIDE", !18}
!20 = !{!"next", !"DefaultHasher.ti", !"foo", !21, !2, !16}
!21 = !{!22}
!22 = !{!"", !"DefaultHasher.ti", !12}
