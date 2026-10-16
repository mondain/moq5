#ifndef SCENARIO_VERSION_H
#define SCENARIO_VERSION_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* NULL means a codec-only fixture, not a protocol-version lane. */
static inline int scenario_require_version(const char *runner,
                                          const char *supported)
{
    const char *version = getenv("MOQ_SIM_VERSION");
    if (!version || (supported && strcmp(version, supported) == 0))
        return 0;

    if (supported)
        fprintf(stderr, "FAIL: %s: MOQ_SIM_VERSION=%s unsupported; "
                "this fixture requires draft-%s\n", runner, version, supported);
    else
        fprintf(stderr, "FAIL: %s: MOQ_SIM_VERSION=%s unsupported; "
                "this codec-only fixture has no protocol-version lane\n",
                runner, version);
    return 1;
}

#endif
