# The WPDS implementaton is very likely unfinished.
set(WPDS_SOURCES
  GenKillTransformer.cpp
  InterProceduralDataFlowEngine.cpp
)
add_library(WPDSDataFlow STATIC ${WPDS_SOURCES})
