; RUN: opt -passes=cj-annotation-outline -S %s | FileCheck %s

; Debug and pseudo instructions are not cloned into helpers, so they must not
; make a group look profitable. These two bodies tie at four cloned
; instructions each; the debug labels must not tip them into being outlined.

declare i8 addrspace(1)* @llvm.cj.malloc.object(i8*, i32)
declare void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)*, i8 addrspace(1)**)
declare void @llvm.dbg.label(metadata)

@_CAO7default3fooE = internal global i8 addrspace(1)* null
@_CAO7default3barE = internal global i8 addrspace(1)* null

define internal void @_CAO7default3foo_iiHv() gc "cangjie" !dbg !4 {
bb3:
  call void @llvm.dbg.label(metadata !7), !dbg !16
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 8)
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*
  call void @llvm.dbg.label(metadata !8), !dbg !16
  store i64 42, i64 addrspace(1)* %1, align 8
  call void @llvm.dbg.label(metadata !9), !dbg !16
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %0, i8 addrspace(1)** @_CAO7default3fooE)
  ret void
}

define internal void @_CAO7default3bar_iiHv() gc "cangjie" !dbg !10 {
bb3:
  call void @llvm.dbg.label(metadata !13), !dbg !17
  %0 = call i8 addrspace(1)* @llvm.cj.malloc.object(i8* null, i32 8)
  %1 = bitcast i8 addrspace(1)* %0 to i64 addrspace(1)*
  call void @llvm.dbg.label(metadata !14), !dbg !17
  store i64 42, i64 addrspace(1)* %1, align 8
  call void @llvm.dbg.label(metadata !15), !dbg !17
  call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %0, i8 addrspace(1)** @_CAO7default3barE)
  ret void
}

; CHECK-LABEL: define internal void @_CAO7default3foo_iiHv()
; CHECK: call i8 addrspace(1)* @llvm.cj.malloc.object
; CHECK: call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %[[FOO_OBJ:.*]], i8 addrspace(1)** @_CAO7default3fooE)

; CHECK-LABEL: define internal void @_CAO7default3bar_iiHv()
; CHECK: call i8 addrspace(1)* @llvm.cj.malloc.object
; CHECK: call void @llvm.cj.gcwrite.static.ref(i8 addrspace(1)* %[[BAR_OBJ:.*]], i8 addrspace(1)** @_CAO7default3barE)

; CHECK-NOT: _imp

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3}

!0 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !1, producer: "test", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug)
!1 = !DIFile(filename: "test.ll", directory: "/")
!2 = !{i32 2, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = distinct !DISubprogram(name: "foo", scope: !1, file: !1, line: 1, type: !5, scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)
!5 = !DISubroutineType(types: !6)
!6 = !{}
!7 = !DILabel(scope: !4, name: "foo0", file: !1, line: 2)
!8 = !DILabel(scope: !4, name: "foo1", file: !1, line: 3)
!9 = !DILabel(scope: !4, name: "foo2", file: !1, line: 4)
!10 = distinct !DISubprogram(name: "bar", scope: !1, file: !1, line: 10, type: !11, scopeLine: 10, spFlags: DISPFlagDefinition, unit: !0)
!11 = !DISubroutineType(types: !12)
!12 = !{}
!13 = !DILabel(scope: !10, name: "bar0", file: !1, line: 11)
!14 = !DILabel(scope: !10, name: "bar1", file: !1, line: 12)
!15 = !DILabel(scope: !10, name: "bar2", file: !1, line: 13)
!16 = !DILocation(line: 2, column: 1, scope: !4)
!17 = !DILocation(line: 11, column: 1, scope: !10)
