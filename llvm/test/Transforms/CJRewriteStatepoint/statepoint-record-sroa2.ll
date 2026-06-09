; RUN: opt -cj-rewrite-statepoint -sroa -S < %s | FileCheck %s
; RUN: opt -passes=cj-rewrite-statepoint,sroa -S < %s | FileCheck %s

%record = type { i64, i8 addrspace(1)* }

declare void @g0(%record addrspace(1)* %arg1, i8 addrspace(1)* %arg0) #0 gc "cangjie"
declare %record @g1(%record %arg0, %record %arg1) #0 gc "cangjie"

define %record @foo(%record addrspace(1)* %ptr2, i8 addrspace(1)* %ptr1, %record %arg0, i1 %b1, i1 %b2) #0 gc "cangjie" {
; CHECK:  entry:
; CHECK-NEXT:  %f = alloca %record, align 8
; CHECK-NEXT:  %g = alloca %record, align 8
; CHECK-NEXT:  %arg0.fca.0.extract3 = extractvalue %record %arg0, 0
; CHECK-NEXT:  %arg0.fca.0.gep4 = getelementptr inbounds %record, %record* %f, i32 0, i32 0
; CHECK-NEXT:  store i64 %arg0.fca.0.extract3, i64* %arg0.fca.0.gep4, align 8
; CHECK-NEXT:  %arg0.fca.1.extract5 = extractvalue %record %arg0, 1
; CHECK-NEXT:  %arg0.fca.1.gep6 = getelementptr inbounds %record, %record* %f, i32 0, i32 1
; CHECK-NEXT:  store i8 addrspace(1)* %arg0.fca.1.extract5, i8 addrspace(1)** %arg0.fca.1.gep6, align 8
; CHECK-NEXT:  %arg0.fca.0.extract = extractvalue %record %arg0, 0
; CHECK-NEXT:  %arg0.fca.0.gep = getelementptr inbounds %record, %record* %g, i32 0, i32 0
; CHECK-NEXT:  store i64 %arg0.fca.0.extract, i64* %arg0.fca.0.gep, align 8
; CHECK-NEXT:  %arg0.fca.1.extract = extractvalue %record %arg0, 1
; CHECK-NEXT:  %arg0.fca.1.gep = getelementptr inbounds %record, %record* %g, i32 0, i32 1
; CHECK-NEXT:  store i8 addrspace(1)* %arg0.fca.1.extract, i8 addrspace(1)** %arg0.fca.1.gep, align 8
; CHECK-NEXT:  %f.cast = addrspacecast %record* %f to %record addrspace(1)*
; CHECK-NEXT:  %f.cast.sroa.gep8 = getelementptr inbounds %record, %record addrspace(1)* %f.cast, i32 0, i32 1
; CHECK-NEXT:  %f.cast.sroa.gep = getelementptr inbounds %record, %record addrspace(1)* %f.cast, i32 0, i32 0
; CHECK-NEXT:  %g.cast = addrspacecast %record* %g to %record addrspace(1)*
; CHECK-NEXT:  %f.select = select i1 %b1, %record addrspace(1)* %f.cast, %record addrspace(1)* %g.cast
; CHECK-NEXT:  %f.select.sroa.gep9 = getelementptr inbounds %record, %record addrspace(1)* %f.select, i32 0, i32 1
; CHECK-NEXT:  %f.select.sroa.gep = getelementptr inbounds %record, %record addrspace(1)* %f.select, i32 0, i32 0
; CHECK-NEXT:  %token = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, void (%record addrspace(1)*, i8 addrspace(1)*)* @g0, i32 2, i32 0, %record addrspace(1)* %ptr2, i8 addrspace(1)* %ptr1) [ "gc-live"(%record addrspace(1)* %ptr2, i8 addrspace(1)* %ptr1), "struct-live"(%record* %f, %record* %g) ]
;
entry:
  %f = alloca %record
  %g = alloca %record
  store %record %arg0, %record* %f
  store %record %arg0, %record* %g
  %f.cast = addrspacecast %record* %f to %record addrspace(1)*
  %g.cast = addrspacecast %record* %g to %record addrspace(1)*
  %f.select = select i1 %b1, %record addrspace(1)* %f.cast, %record addrspace(1)* %g.cast
  call void @g0(%record addrspace(1)* %ptr2, i8 addrspace(1)* %ptr1)
  br i1 %b2, label %then, label %else

then:
  br label %exit

else:
  br label %exit

; CHECK:  exit:
; CHECK-NEXT:  %f.phi = phi %record addrspace(1)* [ %f.cast, %then ], [ %f.select, %else ]
; CHECK-NEXT:  %g.phi = phi %record addrspace(1)* [ %g.cast, %then ], [ %ptr2.reloc.casted, %else ]
; CHECK-NEXT:  %f.phi.sroa.phi = phi i64 addrspace(1)* [ %f.cast.sroa.gep, %then ], [ %f.select.sroa.gep, %else ]
; CHECK-NEXT:  %f.phi.sroa.phi7 = phi i8 addrspace(1)* addrspace(1)* [ %f.cast.sroa.gep8, %then ], [ %f.select.sroa.gep9, %else ]
; CHECK-NEXT:  %f.loaded.fca.0.load = load i64, i64 addrspace(1)* %f.phi.sroa.phi, align 8
; CHECK-NEXT:  %f.loaded.fca.0.insert = insertvalue %record poison, i64 %f.loaded.fca.0.load, 0
; CHECK-NEXT:  %f.loaded.fca.1.load = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %f.phi.sroa.phi7, align 8
; CHECK-NEXT:  %f.loaded.fca.1.insert = insertvalue %record %f.loaded.fca.0.insert, i8 addrspace(1)* %f.loaded.fca.1.load, 1
; CHECK-NEXT:  %g.select = select i1 %b1, %record addrspace(1)* %g.cast, %record addrspace(1)* %g.phi
; CHECK-NEXT:  %g.loaded.fca.0.gep = getelementptr inbounds %record, %record addrspace(1)* %g.select, i32 0, i32 0
; CHECK-NEXT:  %g.loaded.fca.0.load = load i64, i64 addrspace(1)* %g.loaded.fca.0.gep, align 8
; CHECK-NEXT:  %g.loaded.fca.0.insert = insertvalue %record poison, i64 %g.loaded.fca.0.load, 0
; CHECK-NEXT:  %g.loaded.fca.1.gep = getelementptr inbounds %record, %record addrspace(1)* %g.select, i32 0, i32 1
; CHECK-NEXT:  %g.loaded.fca.1.load = load i8 addrspace(1)*, i8 addrspace(1)* addrspace(1)* %g.loaded.fca.1.gep, align 8
; CHECK-NEXT:  %g.loaded.fca.1.insert = insertvalue %record %g.loaded.fca.0.insert, i8 addrspace(1)* %g.loaded.fca.1.load, 1
; CHECK-NEXT:  %token2 = call token (...) @llvm.cj.gc.statepoint(i64 0, i32 0, %record (%record, %record)* @g1, i32 2, i32 0, %record %f.loaded.fca.1.insert, %record %g.loaded.fca.1.insert) [ "gc-live"(%record %f.loaded.fca.1.insert) ]
;
exit:
  %f.phi = phi %record addrspace(1)* [ %f.cast, %then ], [ %f.select, %else ]
  %g.phi = phi %record addrspace(1)* [ %g.cast, %then ], [ %ptr2, %else ]
  %f.loaded = load %record, %record addrspace(1)* %f.phi
  %g.select = select i1 %b1, %record addrspace(1)* %g.cast, %record addrspace(1)* %g.phi
  %g.loaded = load %record, %record addrspace(1)* %g.select
  %result = call %record @g1(%record %f.loaded, %record %g.loaded)
  ret %record %f.loaded
}

attributes #0 = { "record_mut" }
