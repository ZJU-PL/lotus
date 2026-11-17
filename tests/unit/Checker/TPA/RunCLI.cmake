file(MAKE_DIRECTORY "${WORK_DIR}")
set(specs --taint-tpa.ptr-config=${INPUTS}/ptr.spec
  --taint-tpa.modref-config=${INPUTS}/modref.spec --taint-tpa.taint-config=${INPUTS}/taint.spec)
function(check_run expected pattern)
  execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa --analysis-stats --fail-on-findings ${ARGN}
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL expected OR NOT output MATCHES "${pattern}")
    message(FATAL_ERROR "Unexpected TPA CLI result ${result}, expected ${expected}:\n${output}\n${errors}")
  endif()
endfunction()
check_run(1 "raw_sink_violations=2" ${INPUTS}/context.ll ${specs}
  --taint-tpa.k-limit=0 --taint-tpa.track-precision-loss --taint-tpa.def-use-dot-dir=${WORK_DIR}/graphs)
check_run(1 "raw_sink_violations=1" ${INPUTS}/context.ll ${specs} --taint-tpa.k-limit=1)
file(GLOB graphs "${WORK_DIR}/graphs/*.dot")
if(NOT graphs)
  message(FATAL_ERROR "No def-use graph files emitted")
endif()
file(WRITE "${WORK_DIR}/clean.ll" "define i32 @main() { ret i32 0 }\n")
check_run(0 "raw_sink_violations=0" ${WORK_DIR}/clean.ll ${specs})
# Default configurations must be discoverable independently of the cwd.
check_run(1 "raw_sink_violations=1" ${INPUTS}/default.ll)
check_run(1 "raw_sink_violations=1" ${INPUTS}/intrinsic.ll)
file(WRITE "${WORK_DIR}/missing.ll" "declare void @unmodeled()\ndefine i32 @main() { call void @unmodeled() call void @unmodeled() ret i32 0 }\n")
execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa --taint-tpa.unmodeled=error ${WORK_DIR}/missing.ll ${specs}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT errors MATCHES "Missing mod/ref model for unmodeled")
  message(FATAL_ERROR "Missing annotation was not reported as an input error: ${result}\n${output}\n${errors}")
endif()
file(WRITE "${WORK_DIR}/unsupported.ll" "define i32 @main() { %p = alloca i8 %x = va_arg i8* %p, i32 ret i32 0 }\n")
execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa ${WORK_DIR}/unsupported.ll ${specs}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT errors MATCHES "Unsupported TPA taint instruction")
  message(FATAL_ERROR "Unsupported IR was not reported as an input error: ${result}\n${output}\n${errors}")
endif()

# The unified engine uses the shared report policy and output formats.
check_run(1 "JSON report written to" ${INPUTS}/context.ll ${specs}
  --taint-tpa.k-limit=1 --report-json=${WORK_DIR}/report.json)
file(READ "${WORK_DIR}/report.json" report)
if(NOT report MATCHES "taint-tpa" OR NOT report MATCHES "TPA Taint Violation")
  message(FATAL_ERROR "TPA findings or engine metadata missing from JSON report")
endif()
check_run(1 "SARIF report written to" ${INPUTS}/context.ll ${specs}
  --taint-tpa.k-limit=1 --report-sarif=${WORK_DIR}/report.sarif)
check_run(0 "No findings" ${INPUTS}/context.ll ${specs} --report-min-score=81)
execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa ${INPUTS}/context.ll ${specs}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 0 OR NOT output MATCHES "TPA Taint Violation")
  message(FATAL_ERROR "Default finding exit policy differs from other engines: ${result}\n${output}\n${errors}")
endif()

# Ignore is the default; counts describe static call sites, not solver visits.
check_run(0 "unmodeled_policy=ignore ignored_external_symbols=1 ignored_external_calls=2"
  ${WORK_DIR}/missing.ll ${specs})
check_run(0 "ignored_external symbol=unmodeled call_sites=2 missing_modref=2 missing_taint=2"
  ${WORK_DIR}/missing.ll ${specs} --taint-tpa.unmodeled=ignore)
# Debug declarations, values and labels are known no-ops, even in strict mode.
check_run(1 "ignored_external_symbols=0 ignored_external_calls=0"
  ${INPUTS}/debug.ll ${specs} --taint-tpa.unmodeled=error)

file(WRITE "${WORK_DIR}/unresolved.ll" "declare void @sink(i32)\ndefine i32 @main(i32 %argc) { %x = call i32 undef() %y = call i32 asm sideeffect \"\", \"=r\"() call void @sink(i32 %argc) ret i32 0 }\n")
check_run(1 "ignored_unresolved_calls=2 ignored_inline_asm_calls=1"
  ${WORK_DIR}/unresolved.ll ${specs})
check_run(1 "ignored_unresolved caller=main call_sites=2"
  ${WORK_DIR}/unresolved.ll ${specs})
execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa --taint-tpa.unmodeled=error
  ${WORK_DIR}/unresolved.ll ${specs}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT errors MATCHES "cannot resolve call in main")
  message(FATAL_ERROR "Strict unresolved-call policy was not enforced: ${result}\n${output}\n${errors}")
endif()

# Mixed scalar and pointer variadic calls work with the shipped specifications.
file(WRITE "${WORK_DIR}/formatted.ll" [=[
@decimal = private constant [3 x i8] c"%d\00"
@string = private constant [3 x i8] c"%s\00"
@wide = private constant [4 x i8] c"%Lf\00"
declare i32 @snprintf(i8*, i64, i8*, ...)
declare i32 @sprintf(i8*, i8*, ...)
declare i32 @system(i8*)
define i32 @main(i32 %argc) {
  %a = alloca [32 x i8]
  %b = alloca [32 x i8]
  %c = alloca [32 x i8]
  %input = alloca i8
  %da = getelementptr [32 x i8], [32 x i8]* %a, i64 0, i64 0
  %db = getelementptr [32 x i8], [32 x i8]* %b, i64 0, i64 0
  %dc = getelementptr [32 x i8], [32 x i8]* %c, i64 0, i64 0
  %fd = getelementptr [3 x i8], [3 x i8]* @decimal, i64 0, i64 0
  %fs = getelementptr [3 x i8], [3 x i8]* @string, i64 0, i64 0
  %fw = getelementptr [4 x i8], [4 x i8]* @wide, i64 0, i64 0
  %ch = trunc i32 %argc to i8
  store i8 %ch, i8* %input
  %value = sitofp i32 %argc to x86_fp80
  %r1 = call i32 (i8*, i64, i8*, ...) @snprintf(i8* %da, i64 32, i8* %fd, i32 %argc)
  %r2 = call i32 (i8*, i64, i8*, ...) @snprintf(i8* %db, i64 32, i8* %fs, i8* %input)
  %r3 = call i32 (i8*, i8*, ...) @sprintf(i8* %dc, i8* %fw, x86_fp80 %value)
  %s1 = call i32 @system(i8* %da)
  %s2 = call i32 @system(i8* %db)
  %s3 = call i32 @system(i8* %dc)
  ret i32 0
}
]=])
check_run(1 "raw_sink_violations=3" ${WORK_DIR}/formatted.ll)

# A broad points-to set must not apply malloc's model to a void callback.
file(WRITE "${WORK_DIR}/callback.ll" [=[
@alloc_address = global i8* (i64)* @malloc
@known_address = global void (i8*)* @known
@callback = external global void (i8*)*
declare i8* @malloc(i64)
define void @known(i8* %unused) { ret void }
define i32 @main() {
  %fp = load void (i8*)*, void (i8*)** @callback
  call void %fp(i8* null)
  ret i32 0
}
]=])
check_run(0 "filtered_indirect_targets=1" ${WORK_DIR}/callback.ll ${specs}
  --taint-tpa.unmodeled=error)

# Explicit memory positions remain strict; only ranges select by argument type.
file(WRITE "${WORK_DIR}/bad-model.ll" "declare i32 @sum_bytes(i32)\ndefine i32 @main() { %r = call i32 @sum_bytes(i32 42) ret i32 0 }\n")
execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa ${WORK_DIR}/bad-model.ll ${specs}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT errors MATCHES "selects non-pointer argument 0 of type i32")
  message(FATAL_ERROR "An invalid fixed memory argument was accepted: ${result}\n${output}\n${errors}")
endif()
