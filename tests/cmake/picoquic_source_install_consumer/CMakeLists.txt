# Downstream project resolving libmoq from an installed prefix while building
# picoquic from source (MOQ_PICOQUIC_SOURCE_DIR). Configure-only: success
# means the installed FindPicoquic added picoquic and its project() hook ran.
cmake_minimum_required(VERSION 3.20)
project(moq_picoquic_source_install_consumer LANGUAGES C)

if(NOT DEFINED MOQ_CONSUMER_COMPONENT)
    message(FATAL_ERROR "MOQ_CONSUMER_COMPONENT is required")
endif()

# A caller hook already in CMAKE_PROJECT_picoquic_INCLUDE is handed over to
# the installed policy module, which runs it inside picoquic's project()
# after setting the link de-duplication policies. The probe records what it
# sees there, so the checks below observe the installed module itself.
set(CMAKE_PROJECT_picoquic_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/project_probe.cmake")

find_package(libmoq REQUIRED COMPONENTS ${MOQ_CONSUMER_COMPONENT})

string(FIND "${libmoq_DIR}" "${MOQ_CONSUMER_PREFIX}/" prefix_at)
if(NOT prefix_at EQUAL 0)
    message(FATAL_ERROR "libmoq resolved from ${libmoq_DIR}, not the installed prefix ${MOQ_CONSUMER_PREFIX}")
endif()
if(NOT TARGET picoquic-core)
    message(FATAL_ERROR "source-mode FindPicoquic did not add picoquic-core")
endif()
get_property(probe_ran GLOBAL PROPERTY MOQ_PICOQUIC_PROJECT_PROBE SET)
if(NOT probe_ran)
    message(FATAL_ERROR "picoquic's project() did not run the caller hook through the installed policy module")
endif()
foreach(policy CMP0156 CMP0179)
    if(POLICY ${policy})
        get_property(setting GLOBAL PROPERTY MOQ_PICOQUIC_PROJECT_${policy})
        if(NOT setting STREQUAL "NEW")
            message(FATAL_ERROR "${policy} inside picoquic's project() is '${setting}', expected NEW")
        endif()
    endif()
endforeach()
if(NOT CMAKE_PROJECT_picoquic_INCLUDE STREQUAL "${CMAKE_CURRENT_SOURCE_DIR}/project_probe.cmake")
    message(FATAL_ERROR "CMAKE_PROJECT_picoquic_INCLUDE was not restored after find_package")
endif()

# picotls comes from MOQ_PICOTLS_PREFIX only: a picotls build directory
# (headers in ../include) or an installed prefix (include/, lib/).
get_filename_component(ptls_tree "${MOQ_PICOTLS_PREFIX}/.." ABSOLUTE)
get_filename_component(ptls_include "${PTLS_INCLUDE_DIR}" ABSOLUTE)
if(NOT ptls_include STREQUAL "${ptls_tree}/include" AND
   NOT ptls_include STREQUAL "${MOQ_PICOTLS_PREFIX}/include")
    message(FATAL_ERROR "PTLS_INCLUDE_DIR is ${PTLS_INCLUDE_DIR}, not the headers beside MOQ_PICOTLS_PREFIX=${MOQ_PICOTLS_PREFIX}")
endif()
foreach(lib PTLS_CORE_LIBRARY PTLS_MINICRYPTO_LIBRARY)
    string(FIND "${${lib}}" "${MOQ_PICOTLS_PREFIX}/" at)
    if(NOT at EQUAL 0)
        message(FATAL_ERROR "${lib} is ${${lib}}, not under MOQ_PICOTLS_PREFIX=${MOQ_PICOTLS_PREFIX}")
    endif()
endforeach()
