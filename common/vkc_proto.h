/*
 * vkc_proto.h - constants shared by the Pico W firmware and kctool for the
 * VKC/1 TCP protocol.  The protocol itself is specified in docs/protocol.md.
 */
#ifndef VKC_PROTO_H
#define VKC_PROTO_H

#include <stdbool.h>
#include <stddef.h>

#define VKC_PROTO_VERSION "VKC/1"
#define VKC_DEFAULT_PORT  8300
#define VKC_NAME_MAX      63      /* longest tape name, excluding NUL */
#define VKC_LINE_MAX      256     /* longest protocol line incl. "\n" */

/* Tape names: 1..63 chars of [A-Za-z0-9._-], not starting with '.' */
static inline bool vkc_name_valid(const char *s)
{
    size_t n = 0;
    if (!s || !*s || *s == '.')
        return false;
    for (; s[n]; n++) {
        char c = s[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok || n >= VKC_NAME_MAX)
            return false;
    }
    return true;
}

#endif
