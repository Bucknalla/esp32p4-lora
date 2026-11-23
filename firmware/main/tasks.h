/* Every FreeRTOS task, for the System tab: core, priority, stack size and
 * least free stack, run time. */
#pragma once

#include <stddef.h>

/** {"now":µs,"cores":n,"tasks":[{"name","core","prio","state","stack","free",
 *  "run","idle"}...]} with run time in µs since boot (the page takes the
 *  difference between two reads for CPU %). core -1: either core. stack 0:
 *  size unknown. Returns the length, 0 if `cap` is too small or no memory. */
size_t tasks_json(char *out, size_t cap);
