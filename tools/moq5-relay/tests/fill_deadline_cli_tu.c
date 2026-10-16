/* The production CLI translation unit, compiled into the fill-deadline test
 * with its entry point renamed away -- the test provides main. Including the
 * .c keeps this target-scoped: the shipping moq5-relay binaries are unchanged,
 * and the serve compositions the test runs are the exact production ones,
 * facade creation and its configuration included. */
#define main moqr_cli_disabled_main
#include "../cli/main.c"
#undef main
