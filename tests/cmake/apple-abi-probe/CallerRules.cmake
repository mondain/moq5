# A caller-owned hook that already supplies the correct compile verbosity.
set(caller_rules_seen TRUE)
foreach(lang C CXX)
    if(APPLE AND CMAKE_${lang}_COMPILER_ID MATCHES "^(AppleClang|Clang)$"
       AND CMAKE_${lang}_VERBOSE_FLAG STREQUAL "-v -Wl,-v"
       AND NOT DEFINED CMAKE_${lang}_VERBOSE_COMPILE_FLAG)
        set(CMAKE_${lang}_VERBOSE_COMPILE_FLAG "-v")
    endif()
endforeach()
unset(lang)
