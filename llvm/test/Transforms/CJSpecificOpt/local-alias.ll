; RUN: opt --cangjie-pipeline -passes=cj-specific-opt -S < %s | FileCheck %s
; RUN: opt --cangjie-pipeline -passes=cj-specific-opt --cj-local-alias=false -S < %s | FileCheck %s --check-prefix=OFF
;
; CJSpecificOpt rewrites intra-module constant references to preemptible
; Cangjie type metadata globals (*.ti/*.tt) to internal aliases, so that lld
; resolves them as R_*_RELATIVE instead of ABS64/GLOB_DAT in -shared links.

target triple = "x86_64-unknown-linux-gnu"

; --- case 1: external .ti referenced from another global's initializer ---
@pkg.Foo.ti = global ptr null
; CHECK-DAG: @pkg.Foo.ti = global ptr null

@pkg.Bar.ti = global { ptr, ptr } { ptr @pkg.Foo.ti, ptr null }
; CHECK-DAG: @pkg.Bar.ti = global { ptr, ptr } { ptr @alias_pkg.Foo.ti, ptr null }

; --- case 2: .tt suffix is also aliased ---
@pkg.Baz.tt = global ptr @pkg.Foo.ti
@baz.user = global ptr @pkg.Baz.tt
; CHECK-DAG: @pkg.Baz.tt = global ptr @alias_pkg.Foo.ti
; CHECK-DAG: @baz.user = global ptr @alias_pkg.Baz.tt

; --- case 3: weak/linkonce definitions (static generic instances) ---
@weak.ti = weak_odr global i32 0
@linkonce.ti = linkonce_odr global i32 0
@weak.user = global [2 x ptr] [ptr @weak.ti, ptr @linkonce.ti]
; CHECK-DAG: @weak.user = global [2 x ptr] [ptr @alias_weak.ti, ptr @alias_linkonce.ti]

; --- case 4 (regression #553): ThinLTO downgrades a non-prevailing
; linkonce_odr definition to an external declaration; it must be skipped
; without crashing (there is no initializer to alias). ---
@decl.ti = external global i32
@decl.user = global ptr @decl.ti
; CHECK-DAG: @decl.user = global ptr @decl.ti

; --- case 5: dso_local symbols are already non-preemptible ---
@local.ti = dso_local global i32 0
@local.user = global ptr @local.ti
; CHECK-DAG: @local.user = global ptr @local.ti

; --- case 6: globals whose name does not end in .ti/.tt are untouched ---
@pkg.Other = global i32 0
@other.user = global ptr @pkg.Other
; CHECK-DAG: @other.user = global ptr @pkg.Other

; --- case 7 (regression #290): FullLTO keeps an available_externally ti
; body alive (ElimAvailExtern); upgrade it to internal so the local
; definition is emitted. No alias is needed for an internal symbol. ---
@ae.ti = available_externally global i32 0
@ae.user = global ptr @ae.ti
; CHECK-DAG: @ae.ti = internal global i32 0
; CHECK-DAG: @ae.user = global ptr @ae.ti

; --- case 8: a ti global with no constant users needs no alias ---
@code.ti = global i32 0

; Aliases are emitted after the globals, in creation (module) order. Bar.ti
; has no users at all, so nothing needs rewriting and it gets no alias.
; CHECK: @alias_pkg.Foo.ti = internal alias ptr, ptr @pkg.Foo.ti
; CHECK: @alias_pkg.Baz.tt = internal alias ptr, ptr @pkg.Baz.tt
; CHECK: @alias_weak.ti = internal alias i32, ptr @weak.ti
; CHECK: @alias_linkonce.ti = internal alias i32, ptr @linkonce.ti
; No aliases for the skipped cases:
; CHECK-NOT: @alias_pkg.Bar.ti
; CHECK-NOT: @alias_decl.ti
; CHECK-NOT: @alias_local.ti
; CHECK-NOT: @alias_pkg.Other
; CHECK-NOT: @alias_ae.ti
; CHECK-NOT: @alias_code.ti

define i32 @load_code_ti() {
  %v = load i32, ptr @code.ti
  ret i32 %v
}
; CHECK-LABEL: define i32 @load_code_ti

; --- the escape hatch disables alias rewriting (but not the
; available_externally -> internal upgrade, which is a correctness fix) ---
; OFF-NOT: alias_
; OFF: @ae.ti = internal global i32 0
