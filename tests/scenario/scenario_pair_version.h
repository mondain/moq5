#ifndef SCENARIO_PAIR_VERSION_H
#define SCENARIO_PAIR_VERSION_H

#include <moq/session.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Scenario CLI selection, never a SimPair library default. Validate in main
 * before running seeds, then put this value into every pair configuration. */
static inline moq_version_t scenario_selected_version(moq_version_t fallback)
{
    const char *selected = getenv("MOQ_SIM_VERSION");
    if (!selected || !*selected) return fallback;
    if (strcmp(selected, "16") == 0) return MOQ_VERSION_DRAFT_16;
    if (strcmp(selected, "18") == 0) return MOQ_VERSION_DRAFT_18;
    if (strcmp(selected, "22") == 0) return MOQ_VERSION_DRAFT_22;
    fprintf(stderr, "FAIL scenario.version: unsupported MOQ_SIM_VERSION=%s; "
                    "require 16, 18 or 22\n", selected);
    return (moq_version_t)0;
}

#endif
