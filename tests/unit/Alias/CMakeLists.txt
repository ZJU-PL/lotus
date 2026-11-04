# Preserve one alias_tests binary, with sources/dependencies from selected groups.
lotus_add_test_group(alias dda DemandDriven/DDA alias_tests LINK_LIBS DDA)
lotus_add_test_group(alias aserpta InclusionBased/AserPTA alias_tests LINK_LIBS AserPTA)
lotus_add_test_group(alias bootstrapaa InclusionBased/BootstrapAA alias_tests
  LINK_LIBS CanaryBootstrapAA)
lotus_add_test_group(alias flowsensitive InclusionBased/FlowSensitive alias_tests
  LINK_LIBS FlowSensitivePTA)
lotus_add_test_group(alias lotusaa InclusionBased/LotusAA alias_tests
  LINK_LIBS LotusAA CanaryGuardedValueFlow CanaryAliasAnalysisWrapper
    LLVMAnalysis LLVMTransformUtils)
lotus_add_test_group(alias sparrowaa InclusionBased/SparrowAA alias_tests LINK_LIBS Andersen)
lotus_add_test_group(alias tpa InclusionBased/TPA alias_tests
  LINK_LIBS TPATransforms FSCSPointerAnalysis)
lotus_add_test_group(alias aliaswrapper Infrastructure/AliasAnalysisWrapper alias_tests
  LINK_LIBS CanaryAliasAnalysisWrapper)
lotus_add_test_group(alias cclyzeraa InclusionBased/CclyzerAA alias_tests
  LINK_LIBS LotusCclyzerAA CanaryAliasAnalysisWrapper)
lotus_add_test_group(alias dyckaa UnificationBased/DyckAA alias_tests LINK_LIBS CanaryDyckAA)
lotus_add_test_group(alias seadsa UnificationBased/SeaDsa alias_tests LINK_LIBS SeaDsaAnalysis)
lotus_add_test_group(alias allocaa Specialized/AllocAA alias_tests
  LINK_LIBS CanaryAllocAA LLVMAnalysis)

lotus_add_test_group(alias underapproxaa Specialized/UnderApproxAA alias_tests
  LINK_LIBS CanaryUnderApproxAA)
lotus_add_test_group(alias ptsset Infrastructure/PtsSet alias_tests LINK_LIBS CanaryAliasPtsSet)

get_property(alias_test_libs GLOBAL PROPERTY LOTUS_TEST_SUITE_alias_tests_LINK_LIBS)
list(REMOVE_DUPLICATES alias_test_libs)
add_lotus_collected_test_suite(alias_tests LINK_LIBS ${alias_test_libs})
