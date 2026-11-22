#include "lwc/form.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, \
                    #cond);                                                  \
            failures++;                                                      \
        }                                                                    \
    } while (0)

static int get(const char *b, const char *k, char *o, size_t cap)
{
    return lwc_form_get(b, strlen(b), k, o, cap);
}

int main(void)
{
    char v[16];
    CHECK(get("user=admin&password=p%40ss+w%2Fd", "user", v, sizeof v) == 5 && strcmp(v, "admin") == 0);
    CHECK(get("user=admin&password=p%40ss+w%2Fd", "password", v, sizeof v) == 8 && strcmp(v, "p@ss w/d") == 0);
    CHECK(get("xuser=a&user=b", "user", v, sizeof v) == 1 && v[0] == 'b');
    CHECK(get("user=&x=1", "user", v, sizeof v) == 0 && v[0] == '\0');
    CHECK(get("user=a&user=b", "user", v, sizeof v) == 1 && v[0] == 'a'); /* first wins */
    CHECK(get("user", "user", v, sizeof v) == -1);
    CHECK(get("user=a", "pass", v, sizeof v) == -1);
    CHECK(get("p=%4", "p", v, sizeof v) == -1);      /* cut escape */
    CHECK(get("p=%zz", "p", v, sizeof v) == -1);     /* bad escape */
    CHECK(get("p=a%00b", "p", v, sizeof v) == -1);   /* NUL */
    CHECK(get("p=0123456789abcdef", "p", v, sizeof v) == -1); /* 16 > cap-1 */
    CHECK(get("p=0123456789abcde", "p", v, sizeof v) == 15);
    CHECK(lwc_form_get("p=1", 1, "p", v, sizeof v) == -1); /* length bounds the scan */
    CHECK(lwc_form_get(NULL, 1, "p", v, sizeof v) == -1);
    CHECK(lwc_form_get("p=1", 3, "p", v, 0) == -1);
    if (failures != 0) {
        fprintf(stderr, "test_form: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_form: all passed\n");
    return 0;
}
