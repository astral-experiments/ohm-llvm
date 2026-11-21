; RUN: opt -passes=inline-small-rust-functions -verify-each -S %s | FileCheck %s
; RUN: opt -passes='default<O1>' -inline-threshold=0 -S %s | FileCheck %s --check-prefix=OUTLINED
; RUN: opt -passes='default<O1>' -passes-ep-pipeline-early-simplification=print -disable-output %s 2>&1 | FileCheck %s --check-prefix=OUTLINED
; RUN: opt -passes='default<O1>' -S %s | FileCheck %s --check-prefix=INSTRUMENT

target triple = "x86_64-unknown-linux-gnu"

; Each layer is small and adds no basic blocks when inlined. Expanding
; each newly cloned call into the next layer multiplies the work.
define void @_Rroot() {
; OUTLINED-LABEL: define void @_Rroot(
; OUTLINED: call {{.*}}@_Router()
; CHECK-LABEL: define void @_Rroot(
; CHECK: call void @_Rinner()
; CHECK-NOT: call void @_Rrequired()
; CHECK: call void @required_leaf()
; CHECK: ret void
  call void @_Router()
  call void @_Router()
  call void @_Router()
  call void @_Router()
  call void @_Rrequired()
  ret void
}

define internal void @_Router() {
  call void @_Rinner()
  call void @_Rinner()
  call void @_Rinner()
  call void @_Rinner()
  ret void
}

define internal void @_Rinner() {
  call void @opaque()
  call void @opaque()
  call void @opaque()
  call void @opaque()
  ret void
}

define internal void @_Rrequired() alwaysinline {
  call void @required_leaf()
  ret void
}

; Both calls must continue to share the single noduplicate site.
define void @_Rtwice() {
; CHECK-LABEL: define void @_Rtwice(
; CHECK: call void @_Rwrapper()
; CHECK: call void @_Rwrapper()
  call void @_Rwrapper()
  call void @_Rwrapper()
  ret void
}

define internal void @_Rwrapper() {
; CHECK-LABEL: define internal void @_Rwrapper(
; CHECK: call void @single_site()
  call void @single_site()
  ret void
}

define i32 @_Rmeasured() {
; INSTRUMENT-LABEL: define {{.*}}@_Rmeasured(
; INSTRUMENT: call void @__cyg_profile_func_enter(
; INSTRUMENT: call void @__cyg_profile_func_exit(
; INSTRUMENT: ret i32 8
  %v = call i32 @seven()
  %r = add i32 %v, 1
  ret i32 %r
}

define i32 @seven() alwaysinline "instrument-function-entry"="__cyg_profile_func_enter" "instrument-function-exit"="__cyg_profile_func_exit" {
  ret i32 7
}

declare void @single_site() noduplicate

declare void @opaque()
declare void @required_leaf()

!llvm.ident = !{!0}
!0 = !{!"rustc version 1.100.0"}
