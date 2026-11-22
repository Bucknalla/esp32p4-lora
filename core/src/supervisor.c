#include "lwc/supervisor.h"

#include <stddef.h>
#include <string.h>

void lwc_sv_init(lwc_sv_t *sv)
{
    if (sv != NULL) {
        memset(sv, 0, sizeof *sv);
    }
}

int lwc_sv_register(lwc_sv_t *sv, const char *name, uint32_t budget_ms, uint32_t now_ms)
{
    if (sv == NULL || name == NULL || budget_ms == 0U) {
        return -1;
    }
    for (int i = 0; i < LWC_SV_MAX; i++) {
        if (sv->e[i].name == NULL) {
            sv->e[i].budget_ms = budget_ms;
            sv->e[i].last_ms = now_ms;
            sv->e[i].active = true;
            sv->e[i].name = name; /* last: marks the slot taken */
            return i;
        }
    }
    return -1;
}

void lwc_sv_beat(lwc_sv_t *sv, int id, uint32_t now_ms)
{
    if (sv != NULL && id >= 0 && id < LWC_SV_MAX && sv->e[id].name != NULL) {
        sv->e[id].last_ms = now_ms;
        sv->e[id].active = true;
    }
}

void lwc_sv_pause(lwc_sv_t *sv, int id)
{
    if (sv != NULL && id >= 0 && id < LWC_SV_MAX) {
        sv->e[id].active = false;
    }
}

int lwc_sv_expired(const lwc_sv_t *sv, uint32_t now_ms, uint32_t *late_ms)
{
    if (sv == NULL) {
        return -1;
    }
    for (int i = 0; i < LWC_SV_MAX; i++) {
        const lwc_sv_entry_t *e = &sv->e[i];
        if (e->name == NULL || !e->active) {
            continue;
        }
        const uint32_t age = now_ms - e->last_ms; /* wraps correctly */
        if (age > e->budget_ms) {
            if (late_ms != NULL) {
                *late_ms = age;
            }
            return i;
        }
    }
    return -1;
}
