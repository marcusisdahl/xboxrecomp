#include "bus_checks.h"
#include "platform/recomp_guest_bus.h"
#include <stdio.h>
#include <stdlib.h>

static void report(const char *name, int passed, void *context)
{
    (void)context;
    if (!passed) fprintf(stderr, "FAIL: %s\n", name);
}

int main(void)
{
    unsigned total, failed, retail_total, retail_failed;
    void *ram = calloc(1, XR_GUEST_RETAIL_RAM);
    void *contiguous = calloc(1, XR_GUEST_RETAIL_RAM);
    if (!ram || !contiguous) { free(ram); free(contiguous); return 2; }
    failed = xr_guest_bus_checks(report, NULL, &total);
    retail_failed = xr_guest_retail_checks(ram, XR_GUEST_RETAIL_RAM, contiguous,
        XR_GUEST_RETAIL_RAM, report, NULL, &retail_total);
    printf("Guest bus: %u/%u passed; retail layout: %u/%u passed\n",
        total - failed, total, retail_total - retail_failed, retail_total);
    free(contiguous); free(ram);
    return failed || retail_failed ? 1 : 0;
}
