# Runs inside picoquic's project() scope; records the policy settings there.
set_property(GLOBAL PROPERTY MOQ_PICOQUIC_PROJECT_PROBE TRUE)
foreach(_moq_probe_policy CMP0156 CMP0179)
    if(POLICY ${_moq_probe_policy})
        cmake_policy(GET ${_moq_probe_policy} _moq_probe_setting)
        set_property(GLOBAL PROPERTY MOQ_PICOQUIC_PROJECT_${_moq_probe_policy}
            "${_moq_probe_setting}")
    endif()
endforeach()
unset(_moq_probe_policy)
unset(_moq_probe_setting)
