# Deliberately use undefined-only defaults: pre-project hooks must not preempt us.
foreach(hook CMAKE_USER_MAKE_RULES_OVERRIDE CMAKE_USER_MAKE_RULES_OVERRIDE_C
        CMAKE_USER_MAKE_RULES_OVERRIDE_CXX)
    if(NOT DEFINED ${hook})
        set(${hook} "${CMAKE_CURRENT_LIST_DIR}/CallerRules.cmake")
        set(toolchain_rules_selected TRUE)
    endif()
endforeach()
unset(hook)
