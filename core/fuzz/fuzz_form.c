/* libFuzzer target: web form bodies are untrusted. */
#include "lwc/form.h"

#include <stdint.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char v[65];
    const int n = lwc_form_get((const char *)data, size, "password", v, sizeof v);
    if (n >= (int)sizeof v || (n >= 0 && (v[n] != '\0' || memchr(v, '\0', (size_t)n) != NULL))) {
        __builtin_trap();
    }
    (void)lwc_form_get((const char *)data, size, "user", v, 8);
    return 0;
}
