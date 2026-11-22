/*
 * Software watchdog: each long-lived task registers with its own time budget
 * and beats while it makes progress. One supervisor task checks the entries
 * and itself feeds the hardware task watchdog, so a task may block longer
 * than the hardware timeout (DNS, TLS, concentrator start) and still be
 * caught when it truly stops. Times are wrapping uint32 milliseconds.
 */
#ifndef LWC_SUPERVISOR_H
#define LWC_SUPERVISOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LWC_SV_MAX 8

typedef struct {
    const char *name;
    uint32_t budget_ms;
    volatile uint32_t last_ms; /* written by the owning task */
    volatile bool active;
} lwc_sv_entry_t;

typedef struct {
    lwc_sv_entry_t e[LWC_SV_MAX];
} lwc_sv_t;

void lwc_sv_init(lwc_sv_t *sv);

/** Add an entry, active and beaten at `now_ms`. Returns its id, or -1 when
 *  full or the arguments are invalid (NULL name, zero budget). */
int lwc_sv_register(lwc_sv_t *sv, const char *name, uint32_t budget_ms, uint32_t now_ms);

void lwc_sv_beat(lwc_sv_t *sv, int id, uint32_t now_ms);

/** Stop watching an entry (e.g. while its task waits on purpose). A later
 *  beat resumes it. */
void lwc_sv_pause(lwc_sv_t *sv, int id);

/** The first active entry whose last beat is older than its budget, or -1.
 *  `*late_ms` (optional) gets how long ago it last beat. */
int lwc_sv_expired(const lwc_sv_t *sv, uint32_t now_ms, uint32_t *late_ms);

#ifdef __cplusplus
}
#endif

#endif /* LWC_SUPERVISOR_H */
