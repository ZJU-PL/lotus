# SVF configuration
if (SVF_DIR)
	set(HAVE_SVF ON)
	add_definitions(-DHAVE_SVF)
	set(SVF_LIBDIR ${SVF_DIR}/lib)

	if (NOT SVF_INCLUDE)
		if (EXISTS "${SVF_DIR}/include/WPA/Andersen.h")
			set(SVF_INCLUDE ${SVF_DIR}/include)
		elseif (EXISTS "${SVF_DIR}/../include/WPA/Andersen.h")
			set(SVF_INCLUDE ${SVF_DIR}/../include)
		else()
			message(FATAL_ERROR "Did not find the directory with SVF headers")
		endif()
	endif()

	set(SVF_LIBS Svf Cudd)

	include_directories(SYSTEM ${SVF_INCLUDE})
	link_directories(${SVF_LIBDIR} ${SVF_LIBDIR}/CUDD)

	if (NOT LLVM_LINK_DYLIB)
		if (${LLVM_PACKAGE_VERSION} VERSION_GREATER "3.4")
			llvm_map_components_to_libnames(llvm_transformutils transformutils)
		else()
			llvm_map_components_to_libraries(llvm_transformutils transformutils)
		endif()
	endif()

	message(STATUS "SVF dir: ${SVF_DIR}")
	message(STATUS "SVF libraries dir: ${SVF_LIBDIR}")
	message(STATUS "SVF include dir: ${SVF_INCLUDE}")
	message(STATUS "SVF libs: ${SVF_LIBS}")
endif(SVF_DIR)

