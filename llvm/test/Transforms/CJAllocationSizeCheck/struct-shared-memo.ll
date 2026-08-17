; RUN: not opt '-passes=default<O0>' --cangjie-pipeline -disable-output < %s 2>&1 | FileCheck %s
;
; CHECK: The allocation type size exceeds the maximum representable size

; Identified struct types form a DAG: each level below references the
; previous one twice, so a naive recursive size computation would visit
; 2^64 nodes and never terminate. The memoized check stays linear and
; still detects the overflow (S59 reaches 2^59 * 64 = 2^65 bits).

%S0 = type { i64 }
%S1 = type { %S0, %S0 }
%S2 = type { %S1, %S1 }
%S3 = type { %S2, %S2 }
%S4 = type { %S3, %S3 }
%S5 = type { %S4, %S4 }
%S6 = type { %S5, %S5 }
%S7 = type { %S6, %S6 }
%S8 = type { %S7, %S7 }
%S9 = type { %S8, %S8 }
%S10 = type { %S9, %S9 }
%S11 = type { %S10, %S10 }
%S12 = type { %S11, %S11 }
%S13 = type { %S12, %S12 }
%S14 = type { %S13, %S13 }
%S15 = type { %S14, %S14 }
%S16 = type { %S15, %S15 }
%S17 = type { %S16, %S16 }
%S18 = type { %S17, %S17 }
%S19 = type { %S18, %S18 }
%S20 = type { %S19, %S19 }
%S21 = type { %S20, %S20 }
%S22 = type { %S21, %S21 }
%S23 = type { %S22, %S22 }
%S24 = type { %S23, %S23 }
%S25 = type { %S24, %S24 }
%S26 = type { %S25, %S25 }
%S27 = type { %S26, %S26 }
%S28 = type { %S27, %S27 }
%S29 = type { %S28, %S28 }
%S30 = type { %S29, %S29 }
%S31 = type { %S30, %S30 }
%S32 = type { %S31, %S31 }
%S33 = type { %S32, %S32 }
%S34 = type { %S33, %S33 }
%S35 = type { %S34, %S34 }
%S36 = type { %S35, %S35 }
%S37 = type { %S36, %S36 }
%S38 = type { %S37, %S37 }
%S39 = type { %S38, %S38 }
%S40 = type { %S39, %S39 }
%S41 = type { %S40, %S40 }
%S42 = type { %S41, %S41 }
%S43 = type { %S42, %S42 }
%S44 = type { %S43, %S43 }
%S45 = type { %S44, %S44 }
%S46 = type { %S45, %S45 }
%S47 = type { %S46, %S46 }
%S48 = type { %S47, %S47 }
%S49 = type { %S48, %S48 }
%S50 = type { %S49, %S49 }
%S51 = type { %S50, %S50 }
%S52 = type { %S51, %S51 }
%S53 = type { %S52, %S52 }
%S54 = type { %S53, %S53 }
%S55 = type { %S54, %S54 }
%S56 = type { %S55, %S55 }
%S57 = type { %S56, %S56 }
%S58 = type { %S57, %S57 }
%S59 = type { %S58, %S58 }
%S60 = type { %S59, %S59 }
%S61 = type { %S60, %S60 }
%S62 = type { %S61, %S61 }
%S63 = type { %S62, %S62 }
%S64 = type { %S63, %S63 }

define void @shared_struct_overflow() gc "cangjie" {
entry:
  %s = alloca %S64, align 8
  ret void
}
