# Explicit opt-in after the toolchain has selected its own hooks.
if(NOT DEFINED CMAKE_USER_MAKE_RULES_OVERRIDE)
    set(CMAKE_USER_MAKE_RULES_OVERRIDE "")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/MoqAppleAbiProbe.cmake")
