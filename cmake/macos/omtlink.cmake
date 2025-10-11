include_guard(GLOBAL)
set(omt_arch "MacOS")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/lib/omt/Libraries/${omt_arch}/libomt.dylib)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/lib/omt/Libraries/Windows)