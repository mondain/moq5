# Stand-in for a picoquic source tree. Its picotls lookups have the same
# shape as picoquic's cmake/FindPTLS.cmake (header hint one level too deep,
# fallbacks beside the top-level source and build trees, cached results kept),
# and it records what the provider ends up with.
cmake_minimum_required(VERSION 3.20)
project(picoquic C)

find_path(PTLS_INCLUDE_DIR
    NAMES picotls/minicrypto.h
    HINTS ${PTLS_PREFIX}/include/picotls
        ${CMAKE_SOURCE_DIR}/../picotls/include
        ${CMAKE_BINARY_DIR}/../picotls/include
        ../picotls/include/)
set(PTLS_HINTS ${PTLS_PREFIX}/lib ${CMAKE_BINARY_DIR}/../picotls ../picotls)
find_library(PTLS_CORE_LIBRARY picotls-core HINTS ${PTLS_HINTS})
find_library(PTLS_MINICRYPTO_LIBRARY picotls-minicrypto HINTS ${PTLS_HINTS})
find_library(PTLS_OPENSSL_LIBRARY picotls-openssl HINTS ${PTLS_HINTS})
find_library(PTLS_FUSION_LIBRARY picotls-fusion HINTS ${PTLS_HINTS})

file(WRITE "${MOQ_TEST_RESULT}"
    "INC=${PTLS_INCLUDE_DIR}\n"
    "CORE=${PTLS_CORE_LIBRARY}\n"
    "MINICRYPTO=${PTLS_MINICRYPTO_LIBRARY}\n"
    "OPENSSL=${PTLS_OPENSSL_LIBRARY}\n"
    "FUSION=${PTLS_FUSION_LIBRARY}\n")

add_library(picoquic-core INTERFACE)
