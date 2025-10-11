set(omt_arch "Winx64")
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/lib/omt/Libraries/${omt_arch}/libomt.lib)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/lib/omt/Libraries/${omt_arch})