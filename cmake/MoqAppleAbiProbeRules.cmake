# Re-enter for delayed languages. Keep the linker's verbose discovery flag.
foreach(_moq_abi_lang C CXX)
    if(APPLE
       AND CMAKE_${_moq_abi_lang}_COMPILER_ID MATCHES "^(AppleClang|Clang)$"
       AND CMAKE_${_moq_abi_lang}_VERBOSE_FLAG STREQUAL "-v -Wl,-v"
       AND NOT DEFINED CMAKE_${_moq_abi_lang}_VERBOSE_COMPILE_FLAG)
        set(CMAKE_${_moq_abi_lang}_VERBOSE_COMPILE_FLAG "-v")
    endif()
endforeach()
unset(_moq_abi_lang)
