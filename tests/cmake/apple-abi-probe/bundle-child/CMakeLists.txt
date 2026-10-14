cmake_minimum_required(VERSION 3.20)
project(ConsumerAbiBundleFixture LANGUAGES NONE)
foreach(lang C CXX)
    if(NOT DEFINED CMAKE_USER_MAKE_RULES_OVERRIDE_${lang}
       OR NOT CMAKE_USER_MAKE_RULES_OVERRIDE_${lang} STREQUAL "")
        message(FATAL_ERROR "Bundle fixture lost explicitly empty ${lang} hook")
    endif()
endforeach()
