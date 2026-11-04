add_library(Annotation STATIC
    ModRef/ExternalModRefTable.cpp
    ModRef/ExternalModRefTablePrinter.cpp
    Pointer/ExternalPointerTable.cpp
    Pointer/ExternalPointerTablePrinter.cpp
    Taint/TaintConfigParser.cpp
    Taint/TaintConfigManager.cpp
    APISpec.cpp
    APISpecJSON.cpp
    Taint/TaintConfigJSON.cpp
)

# Create alias for backward compatibility
add_library(LotusAnnotation ALIAS Annotation)
