; RUN: opt -passes=cj-annotation-outline -S %s | FileCheck %s

; The helper has no DISubprogram, so cloned instructions must not keep the
; representative's debug locations.

declare i8 addrspace(1)* @llvm.cj.malloc.object(i8*, i32)
declare void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)*, i8 addrspace(1)**)

@_CAO7default3fooE = internal global i8 addrspace(1)* null
@_CAO7default3barE = internal global i8 addrspace(1)* null

define internal void @_CAO7default3foo_iiHv() gc "cangjie" !dbg !4 {
bb3:
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 16), !dbg !7
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*, !dbg !7
  store i64 42, i64 addrspace(1)* %1, align 8, !dbg !7
  %2 = getelementptr i64, i64 addrspace(1)* %1, i32 1, !dbg !7
  store i64 7, i64 addrspace(1)* %2, align 8, !dbg !7
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %0, i8 addrspace(1)** @_CAO7default3fooE), !dbg !7
  ret void, !dbg !7
}

define internal void @_CAO7default3bar_iiHv() gc "cangjie" !dbg !8 {
bb3:
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 16), !dbg !11
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*, !dbg !11
  store i64 42, i64 addrspace(1)* %1, align 8, !dbg !11
  %2 = getelementptr i64, i64 addrspace(1)* %1, i32 1, !dbg !11
  store i64 7, i64 addrspace(1)* %2, align 8, !dbg !11
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %0, i8 addrspace(1)** @_CAO7default3barE), !dbg !11
  ret void, !dbg !11
}

; CHECK-LABEL: define internal void @_CAO7default3foo_iiHv_imp(
; CHECK-NOT: !dbg
; CHECK: ret void

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: "test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "test.ll", directory: "/")
!2 = !{i32 2, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "foo", scope: !1, file: !1, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{}
!7 = !DILocation(line: 2, column: 1, scope: !4)
!8 = distinct !DISubprogram(name: "bar", scope: !1, file: !1, line: 10, type: !9, scopeLine: 10, spFlags: DISPFlagDefinition, unit: !0)
!9 = !DISubroutineType(types: !10)
!10 = !{}
!11 = !DILocation(line: 11, column: 1, scope: !8)
