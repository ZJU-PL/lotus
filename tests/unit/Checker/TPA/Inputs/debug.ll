; ModuleID = '/tmp/tpa-debug-fixture.c'
source_filename = "/tmp/tpa-debug-fixture.c"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx15.0.0"

; Function Attrs: noinline nounwind optnone ssp uwtable
define i32 @main(i32 noundef %argc, i8** noundef %argv) #0 !dbg !13 {
entry:
  %retval = alloca i32, align 4
  %argc.addr = alloca i32, align 4
  %argv.addr = alloca i8**, align 8
  %value = alloca i32, align 4
  store i32 0, i32* %retval, align 4
  store i32 %argc, i32* %argc.addr, align 4
  call void @llvm.dbg.declare(metadata i32* %argc.addr, metadata !21, metadata !DIExpression()), !dbg !22
  store i8** %argv, i8*** %argv.addr, align 8
  call void @llvm.dbg.declare(metadata i8*** %argv.addr, metadata !23, metadata !DIExpression()), !dbg !24
  br label %label, !dbg !25

label:                                            ; preds = %entry
  call void @llvm.dbg.label(metadata !26), !dbg !27
  call void @llvm.dbg.declare(metadata i32* %value, metadata !28, metadata !DIExpression()), !dbg !29
  %call = call i32 @source(), !dbg !30
  call void @llvm.dbg.value(metadata i32 %call, metadata !28, metadata !DIExpression()), !dbg !29
  store i32 %call, i32* %value, align 4, !dbg !29
  %0 = load i32, i32* %value, align 4, !dbg !31
  call void @sink(i32 noundef %0), !dbg !32
  ret i32 0, !dbg !33
}

; Function Attrs: nofree nosync nounwind readnone speculatable willreturn
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: nofree nosync nounwind readnone speculatable willreturn
declare void @llvm.dbg.label(metadata) #1
declare void @llvm.dbg.value(metadata, metadata, metadata) #1

declare i32 @source() #2

declare void @sink(i32 noundef) #2

attributes #0 = { noinline nounwind optnone ssp uwtable "frame-pointer"="non-leaf" "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+crc,+crypto,+dotprod,+fp-armv8,+fp16fml,+fullfp16,+lse,+neon,+ras,+rcpc,+rdm,+sha2,+v8.5a,+zcm,+zcz" }
attributes #1 = { nofree nosync nounwind readnone speculatable willreturn }
attributes #2 = { "frame-pointer"="non-leaf" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="apple-m1" "target-features"="+aes,+crc,+crypto,+dotprod,+fp-armv8,+fp16fml,+fullfp16,+lse,+neon,+ras,+rcpc,+rdm,+sha2,+v8.5a,+zcm,+zcz" }

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5, !6, !7, !8, !9, !10, !11}
!llvm.ident = !{!12}

!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: "clang version 14.0.0", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None, sysroot: "/")
!1 = !DIFile(filename: "./tpa-debug-fixture.c", directory: ".")
!2 = !{i32 7, !"Dwarf Version", i32 4}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 1, !"wchar_size", i32 4}
!5 = !{i32 1, !"branch-target-enforcement", i32 0}
!6 = !{i32 1, !"sign-return-address", i32 0}
!7 = !{i32 1, !"sign-return-address-all", i32 0}
!8 = !{i32 1, !"sign-return-address-with-bkey", i32 0}
!9 = !{i32 7, !"PIC Level", i32 2}
!10 = !{i32 7, !"uwtable", i32 1}
!11 = !{i32 7, !"frame-pointer", i32 1}
!12 = !{!"clang version 14.0.0"}
!13 = distinct !DISubprogram(name: "main", scope: !1, file: !1, line: 3, type: !14, scopeLine: 3, flags: DIFlagPrototyped, spFlags: DISPFlagDefinition, unit: !0, retainedNodes: !20)
!14 = !DISubroutineType(types: !15)
!15 = !{!16, !16, !17}
!16 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!17 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !18, size: 64)
!18 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !19, size: 64)
!19 = !DIBasicType(name: "char", size: 8, encoding: DW_ATE_signed_char)
!20 = !{}
!21 = !DILocalVariable(name: "argc", arg: 1, scope: !13, file: !1, line: 3, type: !16)
!22 = !DILocation(line: 3, column: 14, scope: !13)
!23 = !DILocalVariable(name: "argv", arg: 2, scope: !13, file: !1, line: 3, type: !17)
!24 = !DILocation(line: 3, column: 27, scope: !13)
!25 = !DILocation(line: 3, column: 33, scope: !13)
!26 = !DILabel(scope: !13, name: "label", file: !1, line: 4)
!27 = !DILocation(line: 4, column: 1, scope: !13)
!28 = !DILocalVariable(name: "value", scope: !13, file: !1, line: 5, type: !16)
!29 = !DILocation(line: 5, column: 7, scope: !13)
!30 = !DILocation(line: 5, column: 15, scope: !13)
!31 = !DILocation(line: 6, column: 8, scope: !13)
!32 = !DILocation(line: 6, column: 3, scope: !13)
!33 = !DILocation(line: 7, column: 3, scope: !13)
