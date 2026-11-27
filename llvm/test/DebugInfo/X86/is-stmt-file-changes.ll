; RUN: %llc_dwarf -mtriple=x86_64-unknown-linux -O0 -filetype=obj < %s | llvm-dwarfdump --debug-line - | FileCheck %s

; On returning from the header, the source file changes but the line does not.
; CHECK:      {{0x[0-9a-f]+}} 12 1 {{.+}} is_stmt
; CHECK-NEXT: {{0x[0-9a-f]+}} 12 2 {{.+}} is_stmt
define void @straight(ptr %p) !dbg !10 {
  store volatile i32 0, ptr %p, !dbg !12
  ret void, !dbg !13
}

; The else block has the same location as the preceding branch. Its actual
; predecessor ends on a different source file, so it needs its own statement.
; CHECK:      {{0x[0-9a-f]+}} 24 1 {{.+}} is_stmt
; CHECK:      {{0x[0-9a-f]+}} 24 2 {{.+}} is_stmt
; CHECK:      {{0x[0-9a-f]+}} 24 2 {{.+}} is_stmt
define void @edge(i1 %cond, ptr %p) !dbg !20 {
entry:
  br i1 %cond, label %then, label %else, !dbg !22
then:
  br label %end, !dbg !23
else:
  store volatile i32 0, ptr %p, !dbg !23
  ret void
end:
  ret void
}

; A key instruction must not float onto an earlier row from another file.
; CHECK:      {{0x[0-9a-f]+}} 36 1 {{.+}} is_stmt
; CHECK-NEXT: {{0x[0-9a-f]+}} 36 2 {{.+}} is_stmt
define void @key(ptr %p) !dbg !30 {
  store volatile i32 0, ptr %p, !dbg !32
  store volatile i32 1, ptr %p, !dbg !33
  ret void
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!4}
!0 = distinct !DICompileUnit(language: DW_LANG_C, file: !1, producer: "test", isOptimized: false, runtimeVersion: 0, emissionKind: LineTablesOnly)
!1 = !DIFile(filename: "test.c", directory: "/")
!2 = !DIFile(filename: "test.h", directory: "/")
!3 = !DISubroutineType(types: !{})
!4 = !{i32 2, !"Debug Info Version", i32 3}
!10 = distinct !DISubprogram(name: "straight", scope: !1, file: !1, line: 10, type: !3, scopeLine: 10, spFlags: DISPFlagDefinition, unit: !0)
!11 = !DILexicalBlockFile(scope: !10, file: !2, discriminator: 0)
!12 = !DILocation(line: 12, column: 1, scope: !11)
!13 = !DILocation(line: 12, column: 2, scope: !10)
!20 = distinct !DISubprogram(name: "edge", scope: !1, file: !1, line: 20, type: !3, scopeLine: 20, spFlags: DISPFlagDefinition, unit: !0)
!21 = !DILexicalBlockFile(scope: !20, file: !2, discriminator: 0)
!22 = !DILocation(line: 24, column: 1, scope: !21)
!23 = !DILocation(line: 24, column: 2, scope: !20)
!30 = distinct !DISubprogram(name: "key", scope: !1, file: !1, line: 30, type: !3, scopeLine: 30, spFlags: DISPFlagDefinition, unit: !0, keyInstructions: true)
!31 = !DILexicalBlockFile(scope: !30, file: !2, discriminator: 0)
!32 = !DILocation(line: 36, column: 1, scope: !31)
!33 = !DILocation(line: 36, column: 2, scope: !30, atomGroup: 1, atomRank: 1)
