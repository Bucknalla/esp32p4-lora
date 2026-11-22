#include "lwc/netaddr.h"

#include <string.h>

static bool v4_lan(const uint8_t *a)
{
    return a[0] == 10U || (a[0] == 172U && (a[1] & 0xF0U) == 16U) ||
           (a[0] == 192U && a[1] == 168U) || (a[0] == 169U && a[1] == 254U) || a[0] == 127U;
}

bool lwc_ip_is_lan(const uint8_t *addr, size_t len)
{
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    static const uint8_t loop6[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    if (addr == NULL) {
        return false;
    }
    if (len == 4U) {
        return v4_lan(addr);
    }
    if (len != 16U) {
        return false;
    }
    if (memcmp(addr, mapped, sizeof mapped) == 0) {
        return v4_lan(addr + 12);
    }
    return (addr[0] & 0xFEU) == 0xFCU                       /* fc00::/7 */
           || (addr[0] == 0xFEU && (addr[1] & 0xC0U) == 0x80U) /* fe80::/10 */
           || memcmp(addr, loop6, sizeof loop6) == 0;      /* ::1 */
}
