; RUN: opt '-passes=cj-fill-metadata' --cangjie-pipeline -S < %s | FileCheck %s

source_filename = "core"

%BitMap = type { i32, [0 x i8] }

%ExtensionDef = type { i32, i8, i8*, i8*, i8*, i8* }

%TypeInfo = type { i8*, i8, i8, i16, i32, %BitMap*, i32, i8, i8, i16, i32*, i8*, i8*, i8*, %TypeInfo*, %ExtensionDef**, i8*, i8* }
%record.DefaultHasher = type { i64 }

@"DefaultHasher.ti" = global %TypeInfo { i8* null, i8 22, i8 0, i16 1, i32 8, %BitMap* null, i32 0, i8 8, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null }, !Reflection !4 #1

@"std.core:MemoryOrder.ti" = global %TypeInfo { i8* null, i8 23, i8 64, i16 0, i32 0, %BitMap* null, i32 0, i8 1, i8 0, i16 -32768, i32* null, i8* null, i8* null, i8* null, %TypeInfo* null, %ExtensionDef** null, i8* null, i8* null }, !Reflection !7 #1

; CHECK: @res.reflectStr = internal global [4 x i8] c"res\00", align 1 #1
; CHECK: @DefaultHasher.ti.fieldNames = internal global %DefaultHasher.ti.fieldNamesType { i8* getelementptr inbounds ([4 x i8], [4 x i8]* @res.reflectStr, i32 0, i32 0) } #1
; CHECK: @DefaultHasher.ti.reflect = internal unnamed_addr global %DefaultHasher.ti.reflectType { i8* bitcast (%DefaultHasher.ti.fieldNamesType* @DefaultHasher.ti.fieldNames to i8*) } #1
; CHECK: @SeqCst.reflectStr = internal global [7 x i8] c"SeqCst\00", align 1 #1
; CHECK: @"std.core:MemoryOrder.ti.enumctors" = internal global %"std.core:MemoryOrder.ti.enumctorsType" { [7 x i8]* @SeqCst.reflectStr, i8* null } #1
; CHECK: @"std.core:MemoryOrder.ti.reflect" = internal unnamed_addr global %"std.core:MemoryOrder.ti.reflectType" { i8* bitcast (%"std.core:MemoryOrder.ti.enumctorsType"* @"std.core:MemoryOrder.ti.enumctors" to i8*), i32 8192, i32 1 } #1
; CHECK: attributes #1 = { "CFileReflect" }


attributes #0 = { "cj_tt" }
attributes #1 = { "CFileKlass" }

!types = !{!4, !7}

!0 = !{!"std", !"std.core"}
!1 = !{!"private", !"none"}
!2 = !{}
!3 = !{!"public", !"none"}
!4 = !{!"DefaultHasher.ti", !5}
!5 = !{!6}
!6 = !{i32 0, !"res", !1}
!7 = !{!"std.core:MemoryOrder.ti", !8, !10}
!8 = !{!9}
!9 = !{!"SeqCst", !""}
!10 = !{!"enumKind0", !"none"}
