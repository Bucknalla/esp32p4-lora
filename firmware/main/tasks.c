#include "tasks.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/freertos_debug.h"
#include "freertos/task.h"

#define TASKS_MAX 48

static const char *state_name(eTaskState s)
{
    switch (s) {
    case eRunning:
        return "running";
    case eReady:
        return "ready";
    case eBlocked:
        return "blocked";
    case eSuspended:
        return "suspended";
    default:
        return "deleted";
    }
}

size_t tasks_json(char *out, size_t cap)
{
    TaskStatus_t *st = calloc(TASKS_MAX, sizeof *st);
    uint32_t *size = calloc(TASKS_MAX, sizeof *size);
    int8_t *core = calloc(TASKS_MAX, sizeof *core);
    if (st == NULL || size == NULL || core == NULL) {
        free(st);
        free(size);
        free(core);
        return 0;
    }
    configRUN_TIME_COUNTER_TYPE total = 0;
    /* Stack size: pxStackBase is the low end; the snapshot's pxEndOfStack the
     * high end, aligned down (stacks grow down on RISC-V). Both reads with the
     * scheduler held so a task cannot be deleted in between on this core.
     * Returns 0 if there are more than TASKS_MAX tasks (27 today). When a task
     * first uses the FPU, the port takes the FPU save area from the low end
     * of its stack and moves pxStack up: the size (and free) shrink by about
     * 128 B and stay the usable stack. */
    vTaskSuspendAll();
    const UBaseType_t n = uxTaskGetSystemState(st, TASKS_MAX, &total);
    for (UBaseType_t i = 0; i < n; i++) {
        const BaseType_t c = xTaskGetCoreID(st[i].xHandle);
        core[i] = c == tskNO_AFFINITY ? -1 : (int8_t)c;
        TaskSnapshot_t snap;
        if (vTaskGetSnapshot(st[i].xHandle, &snap) == pdTRUE && st[i].pxStackBase != NULL) {
            const uintptr_t lo = (uintptr_t)st[i].pxStackBase, hi = (uintptr_t)snap.pxEndOfStack;
            size[i] = hi > lo && hi - lo < 1048576U ? (uint32_t)((hi - lo + 16U) & ~(uintptr_t)15U) : 0U;
        }
    }
    (void)xTaskResumeAll();

    TaskHandle_t idle[portNUM_PROCESSORS];
    for (int c = 0; c < portNUM_PROCESSORS; c++) {
        idle[c] = xTaskGetIdleTaskHandleForCore(c);
    }
    size_t o = 0;
    int w = snprintf(out, cap, "{\"now\":%" PRIu64 ",\"cores\":%d,\"tasks\":[", (uint64_t)total, portNUM_PROCESSORS);
    o = w > 0 ? (size_t)w : cap;
    for (UBaseType_t i = 0; i < n && o < cap; i++) {
        bool is_idle = false;
        for (int c = 0; c < portNUM_PROCESSORS; c++) {
            is_idle |= st[i].xHandle == idle[c];
        }
        /* Task names come from code (no quotes or backslashes), at most
         * configMAX_TASK_NAME_LEN characters. */
        w = snprintf(out + o, cap - o,
                     "%s{\"name\":\"%s\",\"core\":%d,\"prio\":%u,\"state\":\"%s\",\"stack\":%" PRIu32
                     ",\"free\":%" PRIu32 ",\"run\":%" PRIu64 ",\"idle\":%s}",
                     i ? "," : "", st[i].pcTaskName, core[i],
                     (unsigned)st[i].uxCurrentPriority, state_name(st[i].eCurrentState), size[i],
                     (uint32_t)st[i].usStackHighWaterMark, (uint64_t)st[i].ulRunTimeCounter,
                     is_idle ? "true" : "false");
        o = w > 0 ? o + (size_t)w : cap;
    }
    free(st);
    free(size);
    free(core);
    if (o + 3U > cap) {
        return 0;
    }
    out[o++] = ']';
    out[o++] = '}';
    out[o] = '\0';
    return o;
}
