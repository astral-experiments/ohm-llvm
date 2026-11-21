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

declare { i32, i1 } @llvm.sadd.with.overflow.i32(i32, i32)
declare void @llvm.assume(i1)

; The range iterator establishes that advancing its index cannot overflow.
; Fold that fact before inlining so the caller can count the iterations.
; IR-LABEL: define {{.*}}i8 @xor_range(
; IR: ret i8 0
define i8 @xor_range(i8 %x) {
entry:
  %p = alloca i32
  store i32 0, ptr %p
  br label %loop
loop:
  %sum = phi i8 [0, %entry], [%add, %body]
  %m = call i32 @range_next(ptr %p)
  %more = icmp eq i32 %m, 1
  br i1 %more, label %body, label %exit
body:
  %add = xor i8 %sum, %x
  br label %loop
exit:
  ret i8 %sum
}

define internal i32 @range_next(ptr %p) {
entry:
  %i = load i32, ptr %p
  %more = icmp slt i32 %i, 64
  br i1 %more, label %step, label %exit
step:
  %res = call { i32, i1 } @llvm.sadd.with.overflow.i32(i32 %i, i32 1)
  %overflow = extractvalue { i32, i1 } %res, 1
  %ok = xor i1 %overflow, true
  call void @llvm.assume(i1 %ok)
  %v = extractvalue { i32, i1 } %res, 0
  store i32 %v, ptr %p
  br label %exit
exit:
  %z = phi i32 [1, %step], [0, %entry]
  ret i32 %z
}
