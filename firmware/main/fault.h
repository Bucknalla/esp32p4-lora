/*
 * Fault record (ADR-0009). Survives resets:
 *   - a "why" note for deliberate restarts and watchdog aborts, in RTC
 *     no-init memory (no flash write on the fault path);
 *   - counters per reset category, in NVS (one write per boot);
 *   - the run of abnormal resets in a row (RTC memory; a power cycle clears
 *     it), used to stop a watchdog reboot loop.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Report the previous reset and update the counters. Call after NVS init. */
void fault_boot(void);

/** True when the last reset was a panic, watchdog or brownout. */
bool fault_last_reset_abnormal(void);

/** Abnormal resets in a row without a stable run in between. */
uint32_t fault_streak(void);

/** The system has run long enough: clear the abnormal-reset run. */
void fault_mark_stable(void);

/** Remember why we are about to reset (reported on the next boot). */
void fault_note(const char *why);

/** fault_note(why), then restart. */
void fault_restart(const char *why) __attribute__((noreturn));

/** One-line counters for the console STATUS command. */
void fault_status_line(char *buf, size_t len);
