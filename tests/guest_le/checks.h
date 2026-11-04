#ifndef XR_GUEST_LE_CHECKS_H
#define XR_GUEST_LE_CHECKS_H
#ifdef __cplusplus
extern "C" {
#endif
/* Returns failed checks; reports every case to the caller's logger. */
unsigned xr_guest_le_checks(void (*report)(const char *, int, void *),
                            void *context, unsigned *total);
#ifdef __cplusplus
}
#endif
#endif
