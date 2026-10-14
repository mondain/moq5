# Register before project()/enable_language(); leave caller-owned hooks alone.
foreach(_moq_abi_lang C CXX)
    if(NOT DEFINED CMAKE_USER_MAKE_RULES_OVERRIDE_${_moq_abi_lang})
        set(CMAKE_USER_MAKE_RULES_OVERRIDE_${_moq_abi_lang}
            "${CMAKE_CURRENT_LIST_DIR}/MoqAppleAbiProbeRules.cmake")
    endif()
endforeach()
unset(_moq_abi_lang)
