; RUN: opt -passes=merge-repeated-calls -verify-each -S %s | FileCheck %s
; RUN: opt -passes='default<O1>' -verify-each -S %s | FileCheck %s --check-prefix=LINK --implicit-check-not='call void @unresolved'
; RUN: sed '/^!llvm.ident/d' %s | opt -passes=merge-repeated-calls -S | FileCheck %s --check-prefix=OTHER
; REQUIRES: x86-registered-target

target triple = "x86_64-unknown-linux-gnu"

declare noundef i32 @_RopaqueA(ptr) noreturn nounwind
declare noundef i32 @_RopaqueB(ptr) noreturn nounwind
declare void @unresolved()
declare i32 @rust_eh_personality(...)

; OTHER-LABEL: define void @_RrunA(
; OTHER: invoke i32 @_RopaqueA

; The concrete call target is the only difference. Retain its control-flow
; attributes on the indirect call in the shared body.

define void @_RrunA(ptr %p) personality ptr @rust_eh_personality {
  %q = getelementptr i8, ptr %p, i64 1
  %x = load i8, ptr %q
  %ok = icmp eq i8 %x, 0
  br i1 %ok, label %call, label %skip

call:
  invoke i32 @_RopaqueA(ptr %q) to label %ret unwind label %cleanup

skip:
  %y = add i8 %x, 1
  store i8 %y, ptr %q
  br label %ret

ret:
  ret void

cleanup:
  %exception = landingpad {ptr, i32} cleanup
  store i8 0, ptr %q
  resume {ptr, i32} %exception
}

define void @_RrunB(ptr %p) personality ptr @rust_eh_personality {
  %q = getelementptr i8, ptr %p, i64 1
  %x = load i8, ptr %q
  %ok = icmp eq i8 %x, 0
  br i1 %ok, label %call, label %skip

call:
  invoke i32 @_RopaqueB(ptr %q) to label %ret unwind label %cleanup

skip:
  %y = add i8 %x, 1
  store i8 %y, ptr %q
  br label %ret

ret:
  ret void

cleanup:
  %exception = landingpad {ptr, i32} cleanup
  store i8 0, ptr %q
  resume {ptr, i32} %exception
}

; CHECK-LABEL: define internal void @_RrunA.Tgm(
; CHECK: invoke noundef i32 %{{.*}}(ptr %q) #[[CONTROL:[0-9]+]]
; CHECK-LABEL: define void @_RrunA(
; CHECK: tail call void @_RrunA.Tgm(ptr %p, ptr @_RopaqueA)
; CHECK-LABEL: define void @_RrunB(
; CHECK: tail call void @_RrunA.Tgm(ptr %p, ptr @_RopaqueB)

; Retain the self-edge so tail recursion elimination can still form a loop.
; The opaque calls otherwise make these bodies candidates for sharing.
declare void @_Rleft(i64)
declare void @_Rright(i64)

define void @_Ragain(i64 %n, i64 %x) noinline {
; CHECK-LABEL: define void @_Ragain(
; CHECK: call void @_Rleft(
; CHECK: call void @_Ragain(
  call void @_Rleft(i64 %x)
  %zero = icmp eq i64 %n, 0
  br i1 %zero, label %done, label %recurse

recurse:
  %a = add i64 %x, %n
  %b = lshr i64 %a, 3
  %c = xor i64 %b, %x
  %d = mul i64 %c, %n
  %e = add i64 %d, %a
  %next = sub i64 %n, 1
  call void @_Ragain(i64 %next, i64 %e)
  br label %done

done:
  ret void
}

define void @_Ronce(i64 %n, i64 %x) noinline {
; CHECK-LABEL: define void @_Ronce(
; CHECK: call void @_Rright(
; CHECK: call void @_Ragain(
  call void @_Rright(i64 %x)
  %zero = icmp eq i64 %n, 0
  br i1 %zero, label %done, label %recurse

recurse:
  %a = add i64 %x, %n
  %b = lshr i64 %a, 3
  %c = xor i64 %b, %x
  %d = mul i64 %c, %n
  %e = add i64 %d, %a
  %next = sub i64 %n, 1
  call void @_Ragain(i64 %next, i64 %e)
  br label %done

done:
  ret void
}

; CHECK: attributes #[[CONTROL]] = { noreturn nounwind }

; Inlining resolves all three tests before a body can be shared. Both
; copies receive values 0 and 2, so an interprocedural range includes 1.
; The emitted object must link without a definition of @unresolved.

declare void @_RmarkA()
declare void @_RmarkB()

define internal i1 @_RpredicateA(ptr %p) {
  store volatile i8 1, ptr %p
  ret i1 false
}

define internal void @_RwriteA(ptr %p, ptr %out) {
  store volatile i8 1, ptr %p
  store i1 false, ptr %out
  ret void
}

define internal void @_RcheckA(i64 %x, ptr %p) {
  %out = alloca i1
  store i1 true, ptr %out
  call void @_RmarkA()
  %a = call i1 @_RpredicateA(ptr %p)
  call void @_RwriteA(ptr %p, ptr %out)
  %b = load i1, ptr %out
  %c = icmp eq i64 %x, 1
  %d = or i1 %a, %b
  %bad = or i1 %d, %c
  br i1 %bad, label %fail, label %ok

fail:
  call void @unresolved()
  unreachable

ok:
  %q = getelementptr i8, ptr %p, i64 1
  %y = load volatile i8, ptr %q
  %z = add i8 %y, 1
  store volatile i8 %z, ptr %q
  ret void
}

define internal i1 @_RpredicateB(ptr %p) {
  store volatile i8 2, ptr %p
  ret i1 false
}

define internal void @_RwriteB(ptr %p, ptr %out) {
  store volatile i8 2, ptr %p
  store i1 false, ptr %out
  ret void
}

define internal void @_RcheckB(i64 %x, ptr %p) {
  %out = alloca i1
  store i1 true, ptr %out
  call void @_RmarkB()
  %a = call i1 @_RpredicateB(ptr %p)
  call void @_RwriteB(ptr %p, ptr %out)
  %b = load i1, ptr %out
  %c = icmp eq i64 %x, 1
  %d = or i1 %a, %b
  %bad = or i1 %d, %c
  br i1 %bad, label %fail, label %ok

fail:
  call void @unresolved()
  unreachable

ok:
  %q = getelementptr i8, ptr %p, i64 1
  %y = load volatile i8, ptr %q
  %z = add i8 %y, 1
  store volatile i8 %z, ptr %q
  ret void
}

define void @entry(ptr %p) {
; LINK-LABEL: define void @entry(
; LINK: ret void
  call void @_RcheckA(i64 0, ptr %p)
  call void @_RcheckA(i64 2, ptr %p)
  call void @_RcheckB(i64 0, ptr %p)
  call void @_RcheckB(i64 2, ptr %p)
  ret void
}

; Inlining a small function with a large alloca into a recursive caller
; would reserve the full allocation in every frame.
declare void @_Ruse(ptr)
declare void @_Rdone(i32)
define internal void @_Rhuge() {
  %a = alloca [65536 x i8], align 16
  call void @_Ruse(ptr %a)
  ret void
}
define void @_Rrecursive(i32 %n) {
; LINK-LABEL: define void @_Rrecursive(
; LINK-NOT: alloca
; LINK: call {{.*}}@_Rhuge(
  %zero = icmp eq i32 %n, 0
  br i1 %zero, label %base, label %recurse
base:
  call void @_Rhuge()
  ret void
recurse:
  %nn = sub i32 %n, 1
  call void @_Rrecursive(i32 %nn)
  call void @_Rdone(i32 %n)
  ret void
}

!llvm.ident = !{!0}
!0 = !{!"rustc version 1.100.0"}
