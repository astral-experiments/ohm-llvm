; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O0 -fast-isel -verify-machineinstrs -pass-remarks-missed=sdagisel %s -o %t.s 2>%t.remarks
; RUN: FileCheck %s --check-prefix=ASM < %t.s
; RUN: FileCheck %s --check-prefix=REMARK < %t.remarks
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O0 -fast-isel -stop-after=finalize-isel -o - %s | FileCheck %s --check-prefix=NOMERGE
; RUN: llc -mtriple=x86_64-unknown-linux-gnu -O0 -fast-isel -emit-call-site-info -stop-after=finalize-isel -o - %s | FileCheck %s --check-prefix=CALLSITE

declare i32 @may_throw(i32)
declare i32 @__gxx_personality_v0(...)
declare ptr @__cxa_begin_catch(ptr)
declare void @__cxa_end_catch()
declare void @on_cleanup()
declare i32 @typed_throw(i32)
declare i32 @tagged_throw(i32)
declare i32 @llvm.eh.typeid.for(ptr)
@_ZTIi = external constant ptr

; The invoke result is also an incoming PHI value on the normal edge.
; CALLSITE-LABEL: name: invoke_or_skip
; CALLSITE: fwdArgRegs:
; CALLSITE-NEXT: {{.*}}arg: 0, reg: '$edi'
; ASM-LABEL: invoke_or_skip:
; ASM: [[BEGIN:\.Ltmp[0-9]+]]:
; ASM-NEXT: {{[ \t]*}}callq may_throw@PLT
; ASM-NEXT: [[END:\.Ltmp[0-9]+]]:
; ASM: .section .gcc_except_table
; ASM: .uleb128 [[BEGIN]]-
; ASM: .uleb128 [[END]]-[[BEGIN]]
; REMARK-NOT: FastISel missed terminator: {{.*}}invoke i32 @may_throw
define i32 @invoke_or_skip(i32 %arg, i1 %take) personality ptr @__gxx_personality_v0 {
entry:
  br i1 %take, label %try, label %normal
try:
  %v = invoke i32 @may_throw(i32 %arg) to label %normal unwind label %catch
normal:
  %r = phi i32 [ 19, %entry ], [ %v, %try ]
  ret i32 %r
catch:
  %lp = landingpad { ptr, i32 } catch ptr null
  %ex = extractvalue { ptr, i32 } %lp, 0
  %caught = call ptr @__cxa_begin_catch(ptr %ex)
  call void @__cxa_end_catch()
  ret i32 -77
}

; An indirect invoke still takes the existing SelectionDAG path.
; REMARK: FastISel missed terminator: {{.*}}invoke i32 %callee
define i32 @invoke_indirect(ptr %callee, i32 %arg) personality ptr @__gxx_personality_v0 {
entry:
  %v = invoke i32 %callee(i32 %arg) to label %normal unwind label %cleanup
normal:
  ret i32 %v
cleanup:
  %lp = landingpad { ptr, i32 } cleanup
  call void @on_cleanup()
  resume { ptr, i32 } %lp
}

; Both invokes share one cleanup pad.
; REMARK-NOT: FastISel missed terminator: {{.*}}invoke i32 @may_throw
; NOMERGE-LABEL: name: invoke_twice
; NOMERGE: nomerge CALL64pcrel32 {{.*}}@may_throw
define i32 @invoke_twice(i32 %a, i32 %b) personality ptr @__gxx_personality_v0 {
entry:
  %first = invoke i32 @may_throw(i32 %a) nomerge to label %next unwind label %cleanup
next:
  %second = invoke i32 @may_throw(i32 %b) to label %normal unwind label %cleanup
normal:
  %r = add i32 %first, %second
  ret i32 %r
cleanup:
  %lp = landingpad { ptr, i32 } cleanup
  call void @on_cleanup()
  resume { ptr, i32 } %lp
}

; Both incoming landing-pad registers are used.
; REMARK-NOT: FastISel missed: {{.*}}landingpad { ptr, i32 }
define i32 @typed_catch(i32 %arg) personality ptr @__gxx_personality_v0 {
entry:
  %n = invoke i32 @typed_throw(i32 %arg) to label %ok unwind label %catch
ok:
  ret i32 %n
catch:
  %lp = landingpad {ptr, i32} catch ptr @_ZTIi catch ptr null
  %ex = extractvalue {ptr, i32} %lp, 0
  %tag = extractvalue {ptr, i32} %lp, 1
  %ty = call i32 @llvm.eh.typeid.for(ptr @_ZTIi)
  %isint = icmp eq i32 %tag, %ty
  %caught = call ptr @__cxa_begin_catch(ptr %ex)
  call void @__cxa_end_catch()
  %r = select i1 %isint, i32 41, i32 42
  ret i32 %r
}

; Keep the target's existing PC-section placement for annotated instructions.
; REMARK: FastISel missed terminator: {{.*}}invoke i32 @tagged_throw
; REMARK: FastISel missed: {{.*}}landingpad
define i32 @tagged_invoke(i32 %arg) personality ptr @__gxx_personality_v0 {
entry:
  %v = invoke i32 @tagged_throw(i32 %arg) to label %ok unwind label %pad, !pcsections !0
ok:
  ret i32 %v
pad:
  %lp = landingpad {ptr, i32} cleanup, !pcsections !0
  call void @on_cleanup()
  resume {ptr, i32} %lp
}

!0 = !{!"__llvm_test_pc"}
