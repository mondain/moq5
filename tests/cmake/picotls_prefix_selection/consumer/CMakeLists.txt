# Resolves picoquic through libmoq's FindPicoquic, either in source-tree mode
# against the stand-in provider or through a stand-in installed CONFIG
# package, then records the header and library directories FindPicoquic
# publishes for libmoq's own use.
cmake_minimum_required(VERSION 3.20)
project(moq_picotls_prefix_selection LANGUAGES C)

list(APPEND CMAKE_MODULE_PATH "${MOQ_TEST_MODULE_DIR}")
find_package(Picoquic REQUIRED)
file(APPEND "${MOQ_TEST_RESULT}"
    "SELINC=${PTLS_INCLUDE_DIR}\n"
    "LIBDIR=${MOQ_PTLS_LIB_DIR}\n")
