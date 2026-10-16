# Installs the parent build into a scratch prefix and checks that every
# installed package module only reaches siblings that were installed with it.
#
# A module beside libmoqConfig.cmake may name another file through
# ${CMAKE_CURRENT_LIST_DIR} (include(), a CMAKE_PROJECT_<name>_INCLUDE hook,
# ...) or pull a Find module through find_dependency() after putting its own
# directory on CMAKE_MODULE_PATH. Both resolve only at the consumer's
# configure time, so a missing install rule surfaces downstream and never in
# this build tree, whose modules still sit next to their sources.
#
# A reference is required unless an enclosing if() chain probes
# EXISTS "${CMAKE_CURRENT_LIST_DIR}/<file>" for a file this install did not
# ship: such blocks belong to components that were not built (their export
# files are the missing probe targets). References inside if()/elseif()
# conditions are probes, not requirements. Macro and function bodies are
# charged to their call sites, which carry the gating of the caller.
#
# Inputs: MOQ_BUILD (build tree), MOQ_PREFIX (scratch prefix, recreated),
# MOQ_LIBDIR (CMAKE_INSTALL_LIBDIR), optional MOQ_CONFIG (multi-config).
cmake_minimum_required(VERSION 3.20)

foreach(var MOQ_BUILD MOQ_PREFIX MOQ_LIBDIR)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "${var} is required")
    endif()
endforeach()

file(REMOVE_RECURSE "${MOQ_PREFIX}")
set(config_args "")
if(DEFINED MOQ_CONFIG AND NOT MOQ_CONFIG STREQUAL "")
    set(config_args --config "${MOQ_CONFIG}")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --install "${MOQ_BUILD}" --prefix "${MOQ_PREFIX}"
        ${config_args}
    RESULT_VARIABLE install_rc
    OUTPUT_VARIABLE install_out
    ERROR_VARIABLE install_out)
if(NOT install_rc EQUAL 0)
    message(FATAL_ERROR "install into ${MOQ_PREFIX} failed (${install_rc}):\n${install_out}")
endif()

set(pkg_dir "${MOQ_PREFIX}/${MOQ_LIBDIR}/cmake/libmoq")
if(NOT EXISTS "${pkg_dir}/libmoqConfig.cmake")
    message(FATAL_ERROR "no libmoqConfig.cmake under ${pkg_dir}")
endif()
file(GLOB modules LIST_DIRECTORIES false "${pkg_dir}/*.cmake")

set(cld_re [[\$\{CMAKE_CURRENT_LIST_DIR\}/]])
set(sibling_re "${cld_re}[A-Za-z0-9_+-][A-Za-z0-9_.+-]*\\.cmake")
set(probe_re "EXISTS[ \t]+\"?${cld_re}[A-Za-z0-9_+-][A-Za-z0-9_.+-]*")

set(failures "")
set(checked 0)

# Appends the sibling references and module-mode find_dependency() packages
# named in a command to the requirement lists.
function(_moq_collect_requirements args name refs_var deps_var)
    set(refs "${${refs_var}}")
    set(deps "${${deps_var}}")
    string(REGEX MATCHALL "${sibling_re}" hits "${args}")
    foreach(hit IN LISTS hits)
        string(REGEX REPLACE "^${cld_re}" "" hit "${hit}")
        list(APPEND refs "${hit}")
    endforeach()
    if(name STREQUAL "find_dependency" AND
       NOT args MATCHES "(^|[ \t])(CONFIG|NO_MODULE)([ \t]|$)")
        if(args MATCHES "^[ \t]*([A-Za-z0-9_.+-]+)")
            list(APPEND deps "${CMAKE_MATCH_1}")
        endif()
    endif()
    set(${refs_var} "${refs}" PARENT_SCOPE)
    set(${deps_var} "${deps}" PARENT_SCOPE)
endfunction()

foreach(module IN LISTS modules)
    get_filename_component(module_name "${module}" NAME)
    file(READ "${module}" text)
    # Keep CMake list syntax from reinterpreting the file's own brackets
    # and semicolons while it is split into lines.
    string(REPLACE "[" "<moq-lb>" text "${text}")
    string(REPLACE "]" "<moq-rb>" text "${text}")
    string(REPLACE ";" "<moq-sc>" text "${text}")
    string(REPLACE "\n" ";" lines "${text}")

    set(modpath FALSE)
    if(text MATCHES "list\\(APPEND[ \t]+CMAKE_MODULE_PATH[ \t]+\"?\\$\\{CMAKE_CURRENT_LIST_DIR\\}\"?\\)")
        set(modpath TRUE)
    endif()

    set(stack "")
    set(defs "")
    set(in_def "")
    set(cmd "")
    set(cmd_line 0)
    set(line_no 0)
    foreach(line IN LISTS lines)
        math(EXPR line_no "${line_no} + 1")
        if(line MATCHES "^[ \t]*#")
            continue()
        endif()
        string(REGEX REPLACE "\\)[ \t]*#.*$" ")" line "${line}")
        if(cmd STREQUAL "")
            set(cmd_line ${line_no})
        endif()
        string(APPEND cmd " ${line}")
        string(REGEX REPLACE "[^(]" "" opens "${cmd}")
        string(REGEX REPLACE "[^)]" "" closes "${cmd}")
        string(LENGTH "${opens}" n_open)
        string(LENGTH "${closes}" n_close)
        if(n_open EQUAL 0)
            set(cmd "")
            continue()
        endif()
        if(n_open GREATER n_close)
            continue()
        endif()
        if(NOT cmd MATCHES "^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*\\((.*)\\)[ \t]*$")
            set(cmd "")
            continue()
        endif()
        string(TOLOWER "${CMAKE_MATCH_1}" name)
        set(args "${CMAKE_MATCH_2}")
        set(cmd "")

        if(NOT in_def STREQUAL "")
            if(name STREQUAL "endfunction" OR name STREQUAL "endmacro")
                set(in_def "")
            else()
                _moq_collect_requirements("${args}" "${name}"
                    def_refs_${in_def} def_deps_${in_def})
            endif()
            continue()
        endif()
        if(name STREQUAL "function" OR name STREQUAL "macro")
            string(REGEX MATCH "^[ \t]*([A-Za-z0-9_]+)" def_name "${args}")
            string(STRIP "${def_name}" def_name)
            string(TOLOWER "${def_name}" in_def)
            set(def_refs_${in_def} "")
            set(def_deps_${in_def} "")
            list(APPEND defs "${in_def}")
            continue()
        endif()

        if(name STREQUAL "if" OR name STREQUAL "elseif")
            string(REGEX MATCHALL "${probe_re}" probes "${args}")
            set(frame "")
            foreach(probe IN LISTS probes)
                string(REGEX REPLACE "^EXISTS[ \t]+\"?${cld_re}" "" probe "${probe}")
                string(APPEND frame "|${probe}")
            endforeach()
            if(name STREQUAL "if")
                list(APPEND stack "-${frame}")
            else()
                list(POP_BACK stack top)
                list(APPEND stack "${top}${frame}")
            endif()
            continue()
        endif()
        if(name STREQUAL "endif")
            list(POP_BACK stack)
            continue()
        endif()
        if(name STREQUAL "else")
            continue()
        endif()

        # Skip blocks gated on a component file this install does not carry.
        set(gated FALSE)
        foreach(frame IN LISTS stack)
            string(REPLACE "|" ";" frame_probes "${frame}")
            foreach(probe IN LISTS frame_probes)
                if(NOT probe STREQUAL "-" AND NOT EXISTS "${pkg_dir}/${probe}")
                    set(gated TRUE)
                endif()
            endforeach()
        endforeach()
        if(gated)
            continue()
        endif()

        set(refs "")
        set(deps "")
        _moq_collect_requirements("${args}" "${name}" refs deps)
        list(FIND defs "${name}" def_index)
        if(NOT def_index EQUAL -1)
            list(APPEND refs ${def_refs_${name}})
            list(APPEND deps ${def_deps_${name}})
        endif()
        foreach(ref IN LISTS refs)
            math(EXPR checked "${checked} + 1")
            if(NOT EXISTS "${pkg_dir}/${ref}")
                list(APPEND failures
                    "${module_name}:${cmd_line}: ${name}() references ${ref}, which is not installed beside it")
            endif()
        endforeach()
        if(modpath)
            foreach(dep IN LISTS deps)
                math(EXPR checked "${checked} + 1")
                if(NOT EXISTS "${pkg_dir}/Find${dep}.cmake" AND
                   NOT EXISTS "${CMAKE_ROOT}/Modules/Find${dep}.cmake")
                    list(APPEND failures
                        "${module_name}:${cmd_line}: find_dependency(${dep}) needs Find${dep}.cmake, which is not installed beside it")
                endif()
            endforeach()
        endif()
    endforeach()
    if(NOT stack STREQUAL "")
        list(APPEND failures "${module_name}: unbalanced if()/endif() while scanning")
    endif()
endforeach()

list(LENGTH modules module_count)
if(failures)
    list(JOIN failures "\n  " failure_text)
    message(FATAL_ERROR "installed package modules reach missing siblings:\n  ${failure_text}")
endif()
if(checked EQUAL 0)
    message(FATAL_ERROR "no sibling references found in ${module_count} installed modules; the scan matched nothing")
endif()
message(STATUS "installed module siblings: ${checked} references across ${module_count} modules resolve")
