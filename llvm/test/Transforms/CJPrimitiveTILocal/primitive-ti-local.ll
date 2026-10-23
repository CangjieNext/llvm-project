; RUN: opt --cangjie-pipeline -passes=cj-primitive-ti-local -S < %s | FileCheck %s
; RUN: opt --cangjie-pipeline -passes=cj-primitive-ti-local -cj-primitive-ti-local-min-refs=1 -S < %s | FileCheck %s --check-prefix=ALL
; RUN: opt --cangjie-pipeline -passes=cj-primitive-ti-local -cj-primitive-ti-local=false -S < %s | FileCheck %s --check-prefix=OFF
;
; CJPrimitiveTILocal materializes a hidden local body for imported primitive
; type infos (Unit.ti, Int64.ti, ...) referenced at least
; --cj-primitive-ti-local-min-refs times (default 4, the package-level
; break-even point) from constant initializers, so that the references bind
; at static link time (RELATIVE/RELR) instead of costing one ABS64 dynamic
; relocation each.

target triple = "aarch64-unknown-linux-ohos"

%TypeInfo = type { ptr, i8, i8, i16, i32, ptr, i32, i8, i8, i16, ptr, ptr, ptr, ptr, ptr, ptr, ptr, ptr }

@Int64.ti = external global %TypeInfo
@Unit.ti = external global %TypeInfo
@"my.pkg:Foo.ti" = external global %TypeInfo

; Int64.ti is referenced 4 times: it crosses the default threshold.
; CHECK: @Int64.ti = linkonce_odr hidden global %TypeInfo { ptr @Int64.name, i8 12, i8 0, i16 0, i32 8, ptr null, i32 0, i8 8, i8 0, i16 0, ptr null, ptr null, ptr null, ptr null, ptr null, ptr null, ptr null, ptr null }, align 16
; OFF: @Int64.ti = external global %TypeInfo
@use1 = global [2 x ptr] [ptr @Int64.ti, ptr @Int64.ti]
@use2 = global [2 x ptr] [ptr @Int64.ti, ptr @Int64.ti]

; Unit.ti is referenced only once: below the default threshold of 4.
; CHECK: @Unit.ti = external global %TypeInfo
; ALL: @Unit.ti = linkonce_odr hidden global %TypeInfo { ptr @Unit.name, i8 1, i8 0, i16 0, i32 0, ptr null, i32 0, i8 1, i8 0, i16 0, ptr null, ptr null, ptr null, ptr null, ptr null, ptr null, ptr null, ptr null }, align 16
; OFF: @Unit.ti = external global %TypeInfo
@use3 = global ptr @Unit.ti

; Non-primitive type infos are never touched, regardless of reference count.
; CHECK: @"my.pkg:Foo.ti" = external global %TypeInfo
; ALL: @"my.pkg:Foo.ti" = external global %TypeInfo
@use4 = global [4 x ptr] [ptr @"my.pkg:Foo.ti", ptr @"my.pkg:Foo.ti", ptr @"my.pkg:Foo.ti", ptr @"my.pkg:Foo.ti"]

; The pass emits the private name strings after the other globals.
; CHECK: @Int64.name = private unnamed_addr constant [6 x i8] c"Int64\00"
; ALL: @Unit.name = private unnamed_addr constant [5 x i8] c"Unit\00"
