# Pins how FindPicoquic selects picotls from MOQ_PICOTLS_PREFIX, across cold
# configures and reconfigures of one tree. Hermetic: picotls trees are
# placeholder files (find_library only needs the names). Source-tree mode uses
# a stand-in picoquic whose lookups have the shape of picoquic's own FindPTLS
# and records what the provider ends up with; installed mode uses a stand-in
# picoquic CONFIG package, where FindPicoquic selects only the header and
# library directories libmoq itself reads.
#
# Source-tree cases:
#   cold        build-directory prefix beside a decoy sibling checkout
#   a_to_b      reconfigure onto prefix B while A stays on disk
#   stale_hdr   prefix B with a find-module PTLS_INCLUDE_DIR from A cached
#   stale_lib   prefix B with find-module library selections from A cached
#   conflict    an explicit header or library from A with prefix B
#   explicit    a matching explicit header, kept; a later prefix change
#               refused while it stays, accepted once it is dropped (-U)
#   installed   installed-layout prefix nested under an unrelated include
#   invalid     a prefix that is not a picotls tree
# Installed-package cases (cfg_*): cold, A->B reconfigure, stale cached
# selections, a conflicting explicit library directory, and an installed
# prefix nested under an unrelated include.
# Adapter cases (adapter_*), run when MOQ_OPENSSL_ROOT is given: libmoq itself
# configured with the picoquic adapter and its OpenSSL verifier, reading the
# adapter's include directories from the File API codemodel.
#   adapter_incomplete   verifier headers cached from A, prefix B selected
#                        without <picotls/openssl.h>: refused
#   adapter_complete     verifier headers cached from A, complete prefix C:
#                        C's headers, never A's
#   adapter_system       installed picoquic, no prefix, and a picotls S on
#                        the default search path: S is selected and used
#   adapter_unselected   the same host, with picotls header discovery
#                        settled empty: no selection, so the explicit
#                        verifier header directory is used
#
# Inputs: MOQ_SOURCE (libmoq checkout), MOQ_WORK (recreated), optional
# MOQ_MODULE_DIR (FindPicoquic directory; defaults to MOQ_SOURCE/cmake),
# MOQ_GENERATOR, MOQ_CONTEXT (cache preload carrying the producer toolchain
# and its ABI probe hooks, as written by moq_write_consumer_context),
# MOQ_OPENSSL_ROOT (OpenSSL for the adapter cases).
cmake_minimum_required(VERSION 3.20)

foreach(var MOQ_SOURCE MOQ_WORK)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()
if(NOT DEFINED MOQ_MODULE_DIR OR MOQ_MODULE_DIR STREQUAL "")
    set(MOQ_MODULE_DIR "${MOQ_SOURCE}/cmake")
endif()

set(fixture "${MOQ_SOURCE}/tests/cmake/picotls_prefix_selection")
set(pt "${MOQ_WORK}/pt")
file(REMOVE_RECURSE "${MOQ_WORK}")

function(_pt_tree root libdir)
    file(WRITE "${root}/include/picotls.h" "")
    file(WRITE "${root}/include/picotls/minicrypto.h" "")
    foreach(lib IN LISTS ARGN)
        file(WRITE "${libdir}/libpicotls-${lib}.a" "")
    endforeach()
endfunction()
_pt_tree("${pt}/A" "${pt}/A/build" core minicrypto openssl fusion)
_pt_tree("${pt}/B" "${pt}/B/build" core minicrypto openssl)
_pt_tree("${pt}/nest/inst" "${pt}/nest/inst/lib" core minicrypto openssl)
_pt_tree("${pt}/C" "${pt}/C/build" core minicrypto openssl)
file(WRITE "${pt}/A/include/picotls/openssl.h" "")
file(WRITE "${pt}/C/include/picotls/openssl.h" "")
file(WRITE "${pt}/nest/include/picotls/minicrypto.h" "")
file(MAKE_DIRECTORY "${pt}/empty")

# The consumer's top-level source sits beside a decoy picotls checkout, which
# is where the provider's fallback header search looks.
file(COPY "${fixture}/consumer/" DESTINATION "${MOQ_WORK}/src/proj")
file(WRITE "${MOQ_WORK}/src/picotls/include/picotls/minicrypto.h" "")
file(COPY "${fixture}/fake_picoquic/" DESTINATION "${MOQ_WORK}/fakepq")
file(WRITE "${MOQ_WORK}/pqpkg/picoquic-config.cmake"
    "if(NOT TARGET picoquic::picoquic-core)\n"
    "    add_library(picoquic::picoquic-core INTERFACE IMPORTED)\n"
    "endif()\n")
set(SRC "-DMOQ_PICOQUIC_SOURCE_DIR=${MOQ_WORK}/fakepq" -DMOQ_BUILD_PQ_THREADED=ON)
set(CFG "-Dpicoquic_DIR=${MOQ_WORK}/pqpkg")

set(failures "")
set(generator_args "")
if(DEFINED MOQ_GENERATOR AND NOT MOQ_GENERATOR STREQUAL "")
    set(generator_args -G "${MOQ_GENERATOR}")
endif()
set(context_args "")
if(DEFINED MOQ_CONTEXT AND NOT MOQ_CONTEXT STREQUAL "")
    set(context_args -C "${MOQ_CONTEXT}")
endif()

# _pt_configure(<case> <build-dir> OK|<expected error text> <args>...)
# Leaves the provider's record in pt_<case>_<KEY> for the checks below.
function(_pt_configure name build expect)
    set(result "${MOQ_WORK}/result-${name}.txt")
    file(REMOVE "${result}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" ${generator_args} ${context_args}
            -S "${MOQ_WORK}/src/proj" -B "${MOQ_WORK}/${build}"
            "-DMOQ_TEST_MODULE_DIR=${MOQ_MODULE_DIR}"
            "-DMOQ_TEST_RESULT=${result}"
            ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE out)
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out}")
    if(expect STREQUAL "OK")
        if(NOT rc EQUAL 0)
            set(failures ${failures} "${name}: configure failed: ${out}" PARENT_SCOPE)
            return()
        endif()
        file(STRINGS "${result}" lines)
        foreach(line IN LISTS lines)
            if(line MATCHES "^([A-Z]+)=(.*)$")
                set(pt_${name}_${CMAKE_MATCH_1} "${CMAKE_MATCH_2}" PARENT_SCOPE)
            endif()
        endforeach()
    else()
        string(FIND "${flat}" "${expect}" at)
        if(rc EQUAL 0)
            set(failures ${failures} "${name}: configure succeeded; expected a refusal containing '${expect}'" PARENT_SCOPE)
        elseif(at EQUAL -1)
            set(failures ${failures} "${name}: refusal does not contain '${expect}': ${out}" PARENT_SCOPE)
        endif()
    endif()
endfunction()

# _pt_expect(<case> <KEY> <expected path or NONE>)
function(_pt_expect name key expected)
    set(actual "${pt_${name}_${key}}")
    if(expected STREQUAL "NONE")
        if(NOT actual STREQUAL "" AND NOT actual MATCHES "-NOTFOUND$")
            set(failures ${failures} "${name}: ${key} is ${actual}, expected none" PARENT_SCOPE)
        endif()
        return()
    endif()
    get_filename_component(a "${actual}" REALPATH)
    get_filename_component(e "${expected}" REALPATH)
    if(actual STREQUAL "" OR NOT a STREQUAL e)
        set(failures ${failures} "${name}: ${key} is '${actual}', expected ${expected}" PARENT_SCOPE)
    endif()
endfunction()

macro(_pt_expect_tree name inc libdir)
    _pt_expect(${name} INC "${inc}")
    _pt_expect(${name} CORE "${libdir}/libpicotls-core.a")
    _pt_expect(${name} MINICRYPTO "${libdir}/libpicotls-minicrypto.a")
    _pt_expect(${name} OPENSSL "${libdir}/libpicotls-openssl.a")
    _pt_expect(${name} LIBDIR "${libdir}")
endmacro()

set(A "-DMOQ_PICOTLS_PREFIX=${pt}/A/build")
set(B "-DMOQ_PICOTLS_PREFIX=${pt}/B/build")

_pt_configure(cold b-reconf OK "${A}" ${SRC})
_pt_expect_tree(cold "${pt}/A/include" "${pt}/A/build")
_pt_expect(cold FUSION "${pt}/A/build/libpicotls-fusion.a")

_pt_configure(a_to_b b-reconf OK "${B}" ${SRC})
_pt_expect_tree(a_to_b "${pt}/B/include" "${pt}/B/build")
_pt_expect(a_to_b FUSION NONE)

file(WRITE "${MOQ_WORK}/stale-hdr.cmake"
    "set(PTLS_INCLUDE_DIR [==[${pt}/A/include]==] CACHE PATH \"Path to a file.\")\n")
_pt_configure(stale_hdr b-stale-hdr OK -C "${MOQ_WORK}/stale-hdr.cmake" "${B}" ${SRC})
_pt_expect_tree(stale_hdr "${pt}/B/include" "${pt}/B/build")

file(WRITE "${MOQ_WORK}/stale-lib.cmake"
    "set(PTLS_CORE_LIBRARY [==[${pt}/A/build/libpicotls-core.a]==] CACHE FILEPATH \"Path to a library.\")\n"
    "set(PTLS_FUSION_LIBRARY [==[${pt}/A/build/libpicotls-fusion.a]==] CACHE FILEPATH \"Path to a library.\")\n"
    "set(MOQ_PTLS_LIB_DIR [==[${pt}/A/build]==] CACHE PATH \"picotls library directory (libpicotls-*.a)\")\n")
_pt_configure(stale_lib b-stale-lib OK -C "${MOQ_WORK}/stale-lib.cmake" "${B}" ${SRC})
_pt_expect_tree(stale_lib "${pt}/B/include" "${pt}/B/build")
_pt_expect(stale_lib FUSION NONE)

_pt_configure(conflict_hdr b-conflict-hdr
    "PTLS_INCLUDE_DIR=${pt}/A/include conflicts with MOQ_PICOTLS_PREFIX"
    "-DPTLS_INCLUDE_DIR=${pt}/A/include" "${B}" ${SRC})
_pt_configure(conflict_lib b-conflict-lib
    "PTLS_CORE_LIBRARY=${pt}/A/build/libpicotls-core.a conflicts with MOQ_PICOTLS_PREFIX"
    "-DPTLS_CORE_LIBRARY=${pt}/A/build/libpicotls-core.a" "${B}" ${SRC})

_pt_configure(explicit b-explicit OK "-DPTLS_INCLUDE_DIR=${pt}/B/include" "${B}" ${SRC})
_pt_expect_tree(explicit "${pt}/B/include" "${pt}/B/build")
_pt_configure(explicit_kept b-explicit
    "PTLS_INCLUDE_DIR=${pt}/B/include conflicts with MOQ_PICOTLS_PREFIX" "${A}" ${SRC})
_pt_configure(explicit_dropped b-explicit OK -UPTLS_INCLUDE_DIR "${A}" ${SRC})
_pt_expect_tree(explicit_dropped "${pt}/A/include" "${pt}/A/build")

_pt_configure(installed b-installed OK "-DMOQ_PICOTLS_PREFIX=${pt}/nest/inst" ${SRC})
_pt_expect_tree(installed "${pt}/nest/inst/include" "${pt}/nest/inst/lib")

_pt_configure(invalid b-invalid "is not a picotls tree" "-DMOQ_PICOTLS_PREFIX=${pt}/empty" ${SRC})

# Installed picoquic CONFIG package: only the directories libmoq reads.
macro(_pt_expect_dirs name inc libdir)
    _pt_expect(${name} SELINC "${inc}")
    _pt_expect(${name} LIBDIR "${libdir}")
endmacro()

_pt_configure(cfg_cold b-cfg-reconf OK "${A}" ${CFG})
_pt_expect_dirs(cfg_cold "${pt}/A/include" "${pt}/A/build")

_pt_configure(cfg_a_to_b b-cfg-reconf OK "${B}" ${CFG})
_pt_expect_dirs(cfg_a_to_b "${pt}/B/include" "${pt}/B/build")

file(WRITE "${MOQ_WORK}/cfg-stale.cmake"
    "set(PTLS_INCLUDE_DIR [==[${pt}/A/include]==] CACHE PATH \"picotls include directory (picotls.h)\")\n"
    "set(MOQ_PTLS_LIB_DIR [==[${pt}/A/build]==] CACHE PATH \"picotls library directory (libpicotls-*.a)\")\n")
_pt_configure(cfg_stale b-cfg-stale OK -C "${MOQ_WORK}/cfg-stale.cmake" "${B}" ${CFG})
_pt_expect_dirs(cfg_stale "${pt}/B/include" "${pt}/B/build")

_pt_configure(cfg_conflict b-cfg-conflict
    "MOQ_PTLS_LIB_DIR=${pt}/A/build conflicts with MOQ_PICOTLS_PREFIX"
    "-DMOQ_PTLS_LIB_DIR=${pt}/A/build" "${B}" ${CFG})

_pt_configure(cfg_installed b-cfg-installed OK "-DMOQ_PICOTLS_PREFIX=${pt}/nest/inst" ${CFG})
_pt_expect_dirs(cfg_installed "${pt}/nest/inst/include" "${pt}/nest/inst/lib")

# Adapter: the verifier's picotls headers follow the selection.
if(DEFINED MOQ_OPENSSL_ROOT AND NOT MOQ_OPENSSL_ROOT STREQUAL "")
    # _pt_adapter(<case> <build-dir> OK|<expected error text> <args>...)
    # Leaves the adapter's include directories under the picotls trees in
    # pt_<case>_ADAPTER_INC.
    function(_pt_adapter name build expect)
        set(dir "${MOQ_WORK}/${build}")
        file(REMOVE_RECURSE "${dir}")
        file(MAKE_DIRECTORY "${dir}/.cmake/api/v1/query")
        file(WRITE "${dir}/.cmake/api/v1/query/codemodel-v2" "")
        execute_process(
            COMMAND "${CMAKE_COMMAND}" ${generator_args} ${context_args}
                -S "${MOQ_SOURCE}" -B "${dir}"
                -DMOQ_BUILD_TESTS=OFF -DMOQ_BUILD_SIM=OFF
                -DMOQ_BUILD_EXAMPLES=OFF
                -DMOQ_BUILD_ADAPTER_PICOQUIC=ON
                -DMOQ_PICOQUIC_VERIFY_BACKEND=openssl
                "-DOPENSSL_ROOT_DIR=${MOQ_OPENSSL_ROOT}"
                "-DMOQ_TEST_RESULT=${MOQ_WORK}/result-${name}.txt"
                ${ARGN}
            RESULT_VARIABLE rc
            OUTPUT_VARIABLE out
            ERROR_VARIABLE out)
        string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out}")
        if(NOT expect STREQUAL "OK")
            string(FIND "${flat}" "${expect}" at)
            if(rc EQUAL 0)
                set(failures ${failures} "${name}: configure succeeded; expected a refusal containing '${expect}'" PARENT_SCOPE)
            elseif(at EQUAL -1)
                set(failures ${failures} "${name}: refusal does not contain '${expect}': ${out}" PARENT_SCOPE)
            endif()
            return()
        endif()
        if(NOT rc EQUAL 0)
            set(failures ${failures} "${name}: configure failed: ${out}" PARENT_SCOPE)
            return()
        endif()
        set(reply "${dir}/.cmake/api/v1/reply")
        file(GLOB candidates "${reply}/target-moq-adapter-picoquic-*.json")
        set(target "")
        foreach(candidate IN LISTS candidates)
            file(READ "${candidate}" json)
            string(JSON target_name GET "${json}" name)
            if(target_name STREQUAL "moq-adapter-picoquic")
                set(target "${json}")
            endif()
        endforeach()
        if(target STREQUAL "")
            set(failures ${failures} "${name}: no moq-adapter-picoquic codemodel" PARENT_SCOPE)
            return()
        endif()
        set(found "")
        string(JSON groups LENGTH "${target}" compileGroups)
        math(EXPR last_group "${groups} - 1")
        foreach(g RANGE ${last_group})
            string(JSON n ERROR_VARIABLE err LENGTH "${target}" compileGroups ${g} includes)
            if(err OR n EQUAL 0)
                continue()
            endif()
            math(EXPR last "${n} - 1")
            foreach(i RANGE ${last})
                string(JSON path GET "${target}" compileGroups ${g} includes ${i} path)
                string(FIND "${path}" "${pt}/" in_trees)
                if(in_trees EQUAL 0)
                    list(APPEND found "${path}")
                endif()
            endforeach()
        endforeach()
        list(REMOVE_DUPLICATES found)
        set(pt_${name}_ADAPTER_INC "${found}" PARENT_SCOPE)
    endfunction()

    file(WRITE "${MOQ_WORK}/verifier-a.cmake"
        "set(MOQ_PTLS_OPENSSL_INCLUDE_DIR [==[${pt}/A/include]==] CACHE PATH \"Directory containing <picotls/openssl.h> for the cert verifier\")\n")

    _pt_adapter(adapter_incomplete b-adapter-incomplete
        "have no <picotls/openssl.h>"
        -C "${MOQ_WORK}/verifier-a.cmake" "${B}" ${SRC})

    _pt_adapter(adapter_complete b-adapter-complete OK
        -C "${MOQ_WORK}/verifier-a.cmake"
        "-DMOQ_PICOTLS_PREFIX=${pt}/C/build" ${SRC})
    if(NOT pt_adapter_complete_ADAPTER_INC STREQUAL "${pt}/C/include")
        list(APPEND failures
            "adapter_complete: adapter picotls includes are '${pt_adapter_complete_ADAPTER_INC}', expected ${pt}/C/include")
    endif()

    # A picotls on the default search path (CMAKE_PREFIX_PATH), as an
    # installed system picotls would be. With no prefix it is the selection.
    file(WRITE "${pt}/sys/include/picotls.h" "")
    file(WRITE "${pt}/sys/include/picotls/minicrypto.h" "")
    file(WRITE "${pt}/sys/include/picotls/openssl.h" "")
    _pt_adapter(adapter_system b-adapter-system OK
        "-DCMAKE_PREFIX_PATH=${pt}/sys"
        "-DMOQ_PTLS_OPENSSL_INCLUDE_DIR=${pt}/A/include" ${CFG})
    if(NOT pt_adapter_system_ADAPTER_INC STREQUAL "${pt}/sys/include")
        list(APPEND failures
            "adapter_system: adapter picotls includes are '${pt_adapter_system_ADAPTER_INC}', expected ${pt}/sys/include")
    endif()

    # No picotls selection on the same host. A defined, empty
    # PTLS_INCLUDE_DIR is a settled find result, so FindPicoquic's lookup
    # does not search and no header directory is selected; compiler and
    # build-tool discovery are untouched.
    _pt_adapter(adapter_unselected b-adapter-unselected OK
        "-DCMAKE_PREFIX_PATH=${pt}/sys"
        "-DPTLS_INCLUDE_DIR:PATH="
        "-DMOQ_PTLS_OPENSSL_INCLUDE_DIR=${pt}/A/include" ${CFG})
    if(NOT pt_adapter_unselected_ADAPTER_INC STREQUAL "${pt}/A/include")
        list(APPEND failures
            "adapter_unselected: adapter picotls includes are '${pt_adapter_unselected_ADAPTER_INC}', expected ${pt}/A/include")
    endif()
else()
    message(STATUS "picotls prefix selection: adapter cases need MOQ_OPENSSL_ROOT; not run")
endif()

if(failures)
    list(JOIN failures "\n  " text)
    message(FATAL_ERROR "picotls prefix selection:\n  ${text}")
endif()
message(STATUS "picotls prefix selection: all cases hold")
