set (AnnotationSourceCodes
	ExternalModRefTable.cpp
	ExternalPointerTable.cpp
	ExternalPointerTablePrinter.cpp
)
add_library (Annotation STATIC ${AnnotationSourceCodes})
