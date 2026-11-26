; RUN: opt -passes=inline-small-rust-functions -verify-each -S %s | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; Each layer is small and adds no basic blocks when inlined. Expanding
; each newly cloned call into the next layer multiplies the work.
define void @_Rroot() {
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

declare void @opaque()
declare void @required_leaf()

!llvm.ident = !{!0}
!0 = !{!"rustc version 1.100.0"}
