enable_language(CXX)
foreach(lang C CXX)
    if(NOT CMAKE_${lang}_ABI_COMPILED)
        message(FATAL_ERROR "Child-local ${lang} ABI discovery failed")
    endif()
    if(APPLE AND CMAKE_${lang}_COMPILER_ID MATCHES "^(AppleClang|Clang)$"
       AND CMAKE_${lang}_VERBOSE_FLAG STREQUAL "-v -Wl,-v"
       AND NOT CMAKE_${lang}_VERBOSE_COMPILE_FLAG STREQUAL "-v")
        message(FATAL_ERROR "Root registrar did not correct child-local ${lang}")
    endif()
endforeach()
