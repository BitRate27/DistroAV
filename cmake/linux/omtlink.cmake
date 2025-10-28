include_guard(GLOBAL)

# Path to the prebuilt libomt library for the selected architecture
set(omt_arch "linuxx64")
set(OMT_LIB_PATH "${CMAKE_SOURCE_DIR}/lib/omt/Libraries/${omt_arch}/libomt.so")

# Expose include directory so consumers can find headers
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_SOURCE_DIR}/lib/omt")

# Create an IMPORTED MODULE target for the prebuilt shared object so CMake
# can track it and install it with the rest of the plugin artifacts.
if(NOT TARGET libomt)
	# A MODULE library is intended to be dlopen()-ed at runtime and CMake
	# disallows linking to MODULE targets. The prebuilt libomt.so is a
	# regular shared object, so declare it as an IMPORTED SHARED target
	# so it can be linked into the plugin target at link time.
	add_library(libomt SHARED IMPORTED GLOBAL)
	set_target_properties(libomt PROPERTIES
		IMPORTED_LOCATION "${OMT_LIB_PATH}"
	)
endif()

# Link the imported libomt to the plugin target
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE libomt)

# Install the prebuilt library alongside the plugin so packaging/installation
# places it in the same location as other plugin libraries.
install(FILES "${OMT_LIB_PATH}"
	DESTINATION ${CMAKE_INSTALL_LIBDIR}/obs-plugins
)

# Also copy the library into the build 'rundir' directory after build so
# local run/test setups include the file (mirrors install behaviour).
add_custom_command(TARGET ${CMAKE_PROJECT_NAME}
	POST_BUILD
	COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
	COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OMT_LIB_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/rundir/$<CONFIG>"
	COMMENT "Copy libomt.so to rundir"
	VERBATIM
)