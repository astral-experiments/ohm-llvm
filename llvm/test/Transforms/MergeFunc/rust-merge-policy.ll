; RUN: opt -passes='default<O1>' -S %s | FileCheck %s --check-prefix=MERGE
; RUN: opt -passes='default<O1>' -enable-merge-functions=false -S %s | FileCheck %s --check-prefix=SEPARATE
; REQUIRES: x86-registered-target

target triple = "x86_64-unknown-linux-gnu"

; SEPARATE-LABEL: define {{.*}}@first(
; SEPARATE: mul i32
define i32 @first(i32 %x) noinline {
  %a = mul i32 %x, 17
  %b = xor i32 %a, 123
  ret i32 %b
}

; MERGE-LABEL: define {{.*}}@second(
; MERGE: tail call i32 @first(
; SEPARATE-LABEL: define {{.*}}@second(
; SEPARATE: mul i32
define i32 @second(i32 %x) noinline {
  %a = mul i32 %x, 17
  %b = xor i32 %a, 123
  ret i32 %b
}

!llvm.ident = !{!0}
!0 = !{!"rustc version 1.100.0"}
