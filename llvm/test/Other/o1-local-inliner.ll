; RUN: opt -disable-output -passes=o1-local-inliner -debug-pass-manager %s 2>&1 | FileCheck %s --check-prefix=ORDER --implicit-check-not="SROAPass on erased"
; RUN: opt -S -passes=o1-local-inliner %s | FileCheck %s --check-prefix=IR
; RUN: opt -disable-output -passes='default<O1>' -print-pipeline-passes %s | FileCheck %s --check-prefix=PRINT
; RUN: opt -disable-output -passes='default<O1>' -passes-ep-scalar-optimizer-late='no-op-function' -print-pipeline-passes %s | FileCheck %s --check-prefix=EXT --implicit-check-not=o1-local-inliner
;
; The call-only definition that remains after inlining still gets the full
; simplification pipeline. An address-taken definition gets its usual visit.
; ORDER: Running pass: SROAPass on cyclic
; ORDER-DAG: Running pass: SROAPass on caller
; ORDER-DAG: Running pass: SROAPass on addressed
; ORDER: Running pass: SROAPass on retained
; IR-NOT: define internal i32 @erased
; IR-LABEL: define internal {{.*}}i32 @retained(
; IR-NOT: add i32
; IR: ret i32 %x
; IR-LABEL: define {{.*}}i32 @caller(
; IR: call {{.*}}i32 @retained(
; IR-NOT: call {{.*}} @erased(
; IR: ret i32
; PRINT: o1-local-inliner
; EXT: no-op-function

@address = global ptr @addressed

define internal i32 @addressed(i32 %x) {
  ret i32 %x
}

define internal i32 @erased(i32 %x) {
  %y = add i32 %x, 1
  ret i32 %y
}

define internal i32 @retained(i32 %x) {
  %y = add i32 %x, 0
  ret i32 %y
}

; Multiple uses need the usual early loop simplification before inlining.
define internal i32 @cyclic(i32 %x) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %next, %loop ]
  %next = add i32 %i, 1
  %done = icmp eq i32 %next, %x
  br i1 %done, label %exit, label %loop
exit:
  ret i32 %next
}

define i32 @caller(i32 %x) {
  %a = call i32 @cyclic(i32 %x) noinline
  %b = call i32 @cyclic(i32 %a) noinline
  %r = call i32 @retained(i32 %b) noinline
  %s = call i32 @erased(i32 %r)
  ret i32 %s
}
