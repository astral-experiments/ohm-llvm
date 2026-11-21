; REQUIRES: x86-registered-target
; RUN: opt -passes='default<O0>' -verify-each -S %s | FileCheck %s --check-prefix=HELPER
; RUN: sed 's/ohm.proc-macro-helper/other/' %s | opt -passes='default<O0>' -S | FileCheck %s --check-prefix=PLAIN
; RUN: sed 's/!"ohm.proc-macro-helper", i32 1/!"ohm.proc-macro-helper", i32 2/' %s | opt -passes='default<O0>' -S | FileCheck %s --check-prefix=PLAIN
; RUN: sed 's/!"ohm.proc-macro-helper", i32 1/!"ohm.proc-macro-helper", i64 1/' %s | opt -passes='default<O0>' -S | FileCheck %s --check-prefix=PLAIN
; RUN: sed 's/rustc version/clang version/' %s | opt -passes='default<O0>' -S | FileCheck %s --check-prefix=PLAIN
; RUN: sed 's/^; DEBUG //g' %s | opt -passes='default<O0>' -S | FileCheck %s --check-prefix=PLAIN
; RUN: opt -mtriple=x86_64-apple-darwin -passes='default<O0>' -S %s | FileCheck %s --check-prefix=PLAIN
; RUN: opt -passes='thinlto-pre-link<O0>' -S %s | FileCheck %s --check-prefix=PLAIN

target triple = "x86_64-unknown-linux-gnu"

; HELPER-LABEL: @expand(
; HELPER-NOT: alloca
; HELPER-NOT: call
; HELPER: ret i32 %token
; PLAIN-LABEL: @expand(
; PLAIN: call i32 @helper(
define i32 @expand(i32 %token) {
  %v = call i32 @helper(i32 %token)
  ret i32 %v
}

define internal i32 @helper(i32 %x) {
  %slot = alloca i32
  store i32 %x, ptr %slot
  %v = load i32, ptr %slot
  ret i32 %v
}

!llvm.ident = !{!0}
!llvm.module.flags = !{!1, !2}
!0 = !{!"rustc version 1.100.0-dev"}
!1 = !{i32 7, !"ohm.proc-macro-helper", i32 1}
!2 = !{i32 2, !"Debug Info Version", i32 3}
; DEBUG !llvm.dbg.cu = !{!3}
; DEBUG !3 = distinct !DICompileUnit(language: DW_LANG_Rust, file: !4, producer: "rustc", isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly)
; DEBUG !4 = !DIFile(filename: "lib.rs", directory: "/")
