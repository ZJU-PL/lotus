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
file(WRITE "${WORK_DIR}/missing.ll" "declare void @unmodeled()\ndefine i32 @main() { call void @unmodeled() ret i32 0 }\n")
execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa ${WORK_DIR}/missing.ll ${specs}
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(NOT result EQUAL 2 OR NOT errors MATCHES "Missing mod/ref model for unmodeled")
  message(FATAL_ERROR "Missing annotation was not reported as an input error: ${result}\n${output}\n${errors}")
endif()
file(WRITE "${WORK_DIR}/atomic.ll" "define i32 @main() { %p = alloca i32 %x = atomicrmw add i32* %p, i32 1 seq_cst ret i32 0 }\n")
execute_process(COMMAND "${PROGRAM}" --engine=taint-tpa ${WORK_DIR}/atomic.ll ${specs}
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
