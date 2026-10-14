set(MOQ_BUILD_TESTS OFF)
set(MOQ_BUILD_SIM OFF)
set(MOQ_BUILD_EXAMPLES OFF)
# The test-owned post-project hook enables CXX in the actual library directory.
set(CMAKE_PROJECT_libmoq_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/CheckDelayed.cmake")
add_subdirectory("${moq_source}" libmoq)
