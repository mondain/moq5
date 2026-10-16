# Stand-in for a LibMoQ build that writes the OpenSSL context its test-owned
# children receive. FIND_OPENSSL=OFF models a build that selected nothing.
cmake_minimum_required(VERSION 3.20)
project(moq_provider_context_parent C)

if(FIND_OPENSSL)
    find_package(OpenSSL REQUIRED)
endif()
include("${MOQ_SOURCE}/tests/cmake/ConsumerProviderContext.cmake")
moq_write_consumer_openssl_context("${MOQ_CONTEXT_OUT}")
