#include "checks.h"
#include <stdio.h>

static void report(const char *name, int passed, void *context)
{
    (void)context;
    if (!passed) fprintf(stderr, "FAIL: %s\n", name);
}

int main(void)
{
    unsigned count, failed = xr_guest_le_checks(report, NULL, &count);
    printf("Guest LE memory: %u/%u passed\n", count - failed, count);
    return failed ? 1 : 0;
}
