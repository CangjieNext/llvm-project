; RUN: opt --cangjie-pipeline -passes=cj-specific-opt -S < %s | FileCheck %s
; RUN: opt --cangjie-pipeline -passes=cj-specific-opt --cj-local-alias=false -S < %s | FileCheck %s --check-prefix=OFF
; RUN: opt --cangjie-pipeline -passes='cj-specific-opt,cj-specific-opt' -S < %s | FileCheck %s --check-prefix=TWICE --allow-empty
;
; CJSpecificOpt rewrites intra-module references (constant initializers and
; instruction operands) to preemptible Cangjie type metadata globals
; (*.ti/*.tt) via internal aliases, so that lld resolves them as
; R_*_RELATIVE instead of ABS64/GLOB_DAT in -shared links.
;
; The TWICE run pins idempotency: a second run must not create
; alias-of-alias chains (aliases are excluded as rewrite targets).
; TWICE-NOT: alias_alias

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

; --- case 4 (regression !553): ThinLTO downgrades a non-prevailing
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

; --- case 7 (regression !290): FullLTO keeps an available_externally ti
; body alive (ElimAvailExtern); upgrade it to internal so the local
; definition is emitted. No alias is needed for an internal symbol. ---
@ae.ti = available_externally global i32 0
@ae.user = global ptr @ae.ti
; CHECK-DAG: @ae.ti = internal global i32 0
; CHECK-DAG: @ae.user = global ptr @ae.ti

; --- case 8: instruction operands are rewritten too (a load's address
; operand is not a Constant user, but the alias is bit-identical) ---
@code.ti = global i32 0

; --- case 9: hidden visibility symbols are already non-preemptible ---
@hidden.ti = hidden global i32 0
@hidden.user = global ptr @hidden.ti
; CHECK-DAG: @hidden.user = global ptr @hidden.ti

; --- case 9b: linkage outside the whitelist (weak-any is preemptible but
; not ODR; interposition semantics must be preserved) ---
@weakany.ti = weak global i32 0
@weakany.user = global ptr @weakany.ti
; CHECK-DAG: @weakany.user = global ptr @weakany.ti

; --- case 9c: comdat members may be discarded when the group is not
; prevailing; an internal alias would rebind to another module's copy, so
; they are skipped (cjc does not emit comdats; this pins the guard) ---
$cd.ti = comdat any
@cd.ti = weak_odr global i32 0, comdat
@cd.user = global ptr @cd.ti
; CHECK-DAG: @cd.user = global ptr @cd.ti

; --- case 10: a StaticGenericTI array operand is aliased even with
; --cj-local-alias=false: the switch falls back to the legacy
; generateAliasForStaticGI path (private alias), which covers the
; link-correctness-critical table operands. ---
@sg.ti = linkonce_odr global i32 0
@StaticGenericTIs = private global [1 x ptr] [ptr @sg.ti] #0
@llvm.used = appending global [1 x ptr] [ptr @StaticGenericTIs], section "llvm.metadata"
attributes #0 = { "CFileStaticGenericTI" }
; CHECK-DAG: @StaticGenericTIs = private global [1 x ptr] [ptr @alias_sg.ti]

; --- case 11: a direct call operand (no ConstantExpr) is rewritten ---
@call.ti = global i32 0

; Aliases are emitted after the globals, in creation (module) order.
; Bar.ti has no users at all, so it gets no alias.
; CHECK: @alias_pkg.Foo.ti = internal alias ptr, ptr @pkg.Foo.ti
; CHECK: @alias_pkg.Baz.tt = internal alias ptr, ptr @pkg.Baz.tt
; CHECK: @alias_weak.ti = internal alias i32, ptr @weak.ti
; CHECK: @alias_linkonce.ti = internal alias i32, ptr @linkonce.ti
; CHECK: @alias_code.ti = internal alias i32, ptr @code.ti
; CHECK: @alias_sg.ti = internal alias i32, ptr @sg.ti
; CHECK: @alias_call.ti = internal alias i32, ptr @call.ti
; No aliases for the skipped cases:
; CHECK-NOT: @alias_pkg.Bar.ti
; CHECK-NOT: @alias_decl.ti
; CHECK-NOT: @alias_local.ti
; CHECK-NOT: @alias_pkg.Other
; CHECK-NOT: @alias_ae.ti
; CHECK-NOT: @alias_hidden.ti
; CHECK-NOT: @alias_weakany.ti
; CHECK-NOT: @alias_cd.ti

define i32 @load_code_ti() {
  %v = load i32, ptr @code.ti
  ret i32 %v
}
; CHECK-LABEL: define i32 @load_code_ti
; CHECK: %v = load i32, ptr @alias_code.ti

declare void @use_ti(ptr)

define void @call_use() {
  call void @use_ti(ptr @call.ti)
  ret void
}
; CHECK-LABEL: define void @call_use
; CHECK: call void @use_ti(ptr @alias_call.ti)

; --- the escape hatch falls back to the legacy generateAliasForStaticGI
; path: only StaticGenericTI array operands are aliased (private alias),
; and the new path's available_externally -> internal upgrade for
; non-array globals does not run (exactly the pre-feature behavior). ---
; OFF: @pkg.Bar.ti = global { ptr, ptr } { ptr @pkg.Foo.ti, ptr null }
; OFF: @weak.user = global [2 x ptr] [ptr @weak.ti, ptr @linkonce.ti]
; OFF: @ae.ti = available_externally global i32 0
; OFF: @StaticGenericTIs = private global [1 x ptr] [ptr @alias_sg.ti]
; OFF-NOT: @alias_pkg
; OFF-NOT: @alias_weak
; OFF-NOT: @alias_linkonce
; OFF-NOT: @alias_code
; OFF-NOT: @alias_call
; OFF: @alias_sg.ti = private alias i32, ptr @sg.ti
; OFF: %v = load i32, ptr @code.ti
; OFF: call void @use_ti(ptr @call.ti)
; OFF-NOT: @alias_pkg
; OFF-NOT: @alias_weak
; OFF-NOT: @alias_code
