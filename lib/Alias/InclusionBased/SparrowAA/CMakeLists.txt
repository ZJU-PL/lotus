add_library(Andersen STATIC
	Andersen.cpp
	AndersenAA.cpp
	ConstraintCollect.cpp
	ConstraintOptimize.cpp
	ConstraintSolving.cpp
	ExternalLibrary.cpp
	NodeFactory.cpp
	ResultUtils.cpp
)

target_link_libraries(Andersen PRIVATE CanaryAliasPtsSet AliasSpecManager)
add_library(Lotus::SparrowAA ALIAS Andersen)
