# Stand-in for a test-owned child configure: resolves OpenSSL and records the
# selection it ended up with.
cmake_minimum_required(VERSION 3.20)
project(moq_provider_context_child C)

find_package(OpenSSL REQUIRED)
file(WRITE "${MOQ_RESULT}"
    "INCLUDE=${OPENSSL_INCLUDE_DIR}\n"
    "SSL=${OPENSSL_SSL_LIBRARY}\n"
    "CRYPTO=${OPENSSL_CRYPTO_LIBRARY}\n"
    "FRAMEWORK=${CMAKE_FIND_FRAMEWORK}\n")
