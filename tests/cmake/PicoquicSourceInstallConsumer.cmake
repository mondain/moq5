# Installs the parent build into a scratch prefix (libmoq only; picoquic stays
# a source checkout) and configures a downstream project that resolves libmoq
# from that prefix with MOQ_PICOQUIC_SOURCE_DIR set. The installed
# FindPicoquic then adds picoquic itself, so the configure passes only if the
# installed package carries everything picoquic's project() hook loads.
#
# Inputs: MOQ_BUILD, MOQ_LIBDIR, MOQ_WORK (recreated), MOQ_CONSUMER_SRC,
# MOQ_CONSUMER_COMPONENT, MOQ_CONSUMER_CONTEXT (cache preload carrying the
# producer toolchain and provider hints), MOQ_PICOQUIC_SOURCE_DIR,
# MOQ_PICOTLS_PREFIX, optional MOQ_CONFIG and MOQ_GENERATOR.
cmake_minimum_required(VERSION 3.20)

foreach(var MOQ_BUILD MOQ_LIBDIR MOQ_WORK MOQ_CONSUMER_SRC MOQ_CONSUMER_COMPONENT
        MOQ_CONSUMER_CONTEXT MOQ_PICOQUIC_SOURCE_DIR MOQ_PICOTLS_PREFIX)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

set(prefix "${MOQ_WORK}/prefix")
set(consumer_src "${MOQ_WORK}/src/consumer")
set(consumer_build "${MOQ_WORK}/consumer-build")
file(REMOVE_RECURSE "${MOQ_WORK}")

# The consumer source is copied next to a decoy picotls checkout. picoquic's
# FindPTLS falls back to ${CMAKE_SOURCE_DIR}/../picotls/include, so the
# consumer's check that picotls resolves from MOQ_PICOTLS_PREFIX only holds
# if FindPicoquic settles it before picoquic looks.
file(COPY "${MOQ_CONSUMER_SRC}/" DESTINATION "${consumer_src}")
file(WRITE "${MOQ_WORK}/src/picotls/include/picotls.h" "#error decoy picotls\n")
file(WRITE "${MOQ_WORK}/src/picotls/include/picotls/minicrypto.h" "#error decoy picotls\n")

set(config_args "")
if(DEFINED MOQ_CONFIG AND NOT MOQ_CONFIG STREQUAL "")
    set(config_args --config "${MOQ_CONFIG}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${MOQ_BUILD}" --prefix "${prefix}"
        ${config_args}
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "install into ${prefix} failed (${rc}):\n${out}")
endif()

set(generator_args "")
if(DEFINED MOQ_GENERATOR AND NOT MOQ_GENERATOR STREQUAL "")
    set(generator_args -G "${MOQ_GENERATOR}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" ${generator_args}
        -C "${MOQ_CONSUMER_CONTEXT}"
        -S "${consumer_src}" -B "${consumer_build}"
        "-DCMAKE_PREFIX_PATH=${prefix}"
        "-Dlibmoq_DIR=${prefix}/${MOQ_LIBDIR}/cmake/libmoq"
        "-DMOQ_CONSUMER_PREFIX=${prefix}"
        "-DMOQ_CONSUMER_COMPONENT=${MOQ_CONSUMER_COMPONENT}"
        "-DMOQ_PICOQUIC_SOURCE_DIR=${MOQ_PICOQUIC_SOURCE_DIR}"
        "-DMOQ_PICOTLS_PREFIX=${MOQ_PICOTLS_PREFIX}"
    RESULT_VARIABLE rc
    OUTPUT_VARIABLE out
    ERROR_VARIABLE out)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "source-mode consumer configure against ${prefix} failed (${rc}):\n${out}")
endif()
message(STATUS "source-mode consumer configured against installed prefix ${prefix}")
