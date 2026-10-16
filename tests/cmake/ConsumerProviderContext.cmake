include_guard(GLOBAL)

# Dependency inputs for test-owned child configures, kept apart from the
# toolchain/rules context (ConsumerBuildContext.cmake): a child that links this
# build's archives must resolve the same OpenSSL this build selected, so its
# headers and libraries match what the archives were compiled against.
#
# moq_write_consumer_openssl_context(<path>) writes a cache preload (-C) with
# the OpenSSL selection of the calling build:
#   OPENSSL_ROOT_DIR                 when set,
#   OPENSSL_INCLUDE_DIR, OPENSSL_SSL_LIBRARY, OPENSSL_CRYPTO_LIBRARY
#                                    when resolved (not *-NOTFOUND),
#   CMAKE_FIND_FRAMEWORK             when the build set it (a cache entry;
#                                    the platform default is not a selection).
# Anything absent is left out, so a child of a build that selected nothing
# keeps its normal discovery. Entries are plain cache defaults: a child's own
# -D still takes precedence.
function(moq_write_consumer_openssl_context path)
    set(content "# OpenSSL selection of the parent build.\n")
    foreach(spec
            "OPENSSL_ROOT_DIR|PATH"
            "OPENSSL_INCLUDE_DIR|PATH"
            "OPENSSL_SSL_LIBRARY|FILEPATH"
            "OPENSSL_CRYPTO_LIBRARY|FILEPATH"
            "CMAKE_FIND_FRAMEWORK|STRING")
        string(REPLACE "|" ";" spec "${spec}")
        list(GET spec 0 var)
        list(GET spec 1 type)
        if(NOT DEFINED ${var})
            continue()
        endif()
        if(var STREQUAL "CMAKE_FIND_FRAMEWORK" AND NOT DEFINED CACHE{CMAKE_FIND_FRAMEWORK})
            continue()
        endif()
        set(value "${${var}}")
        if(value STREQUAL "" OR value MATCHES "-NOTFOUND$")
            continue()
        endif()
        set(eq "=")
        string(FIND "${value}" "]${eq}]" found)
        while(NOT found EQUAL -1)
            string(APPEND eq "=")
            string(FIND "${value}" "]${eq}]" found)
        endwhile()
        string(APPEND content
            "set(${var} [${eq}[${value}]${eq}] CACHE ${type} \"Parent build OpenSSL selection\")\n")
    endforeach()
    file(WRITE "${path}" "${content}")
endfunction()
