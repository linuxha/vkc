/*
 * net.h - Wi-Fi management and the VKC/1 client (docs/protocol.md).
 *
 * The firmware uses pico_cyw43_arch_lwip_poll: everything network-related
 * runs from the main loop, so the calls below simply block (while polling
 * the Wi-Fi chip) until they finish or time out.
 */
#ifndef VKC_NET_H
#define VKC_NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kcs.h"

typedef void (*net_log_fn)(const char *msg);

bool net_init(net_log_fn log);
void net_wifi_connect(void);      /* (re)connect with the current g_cfg */
void net_service(void);           /* call from the main loop */
bool net_up(void);
const char *net_status(void);     /* human readable link state */
const char *net_ip(void);

/* Each call opens a connection, performs one command and closes it.
 * On failure they return false and put a reason in err. */
typedef void (*vkc_dir_cb)(void *ctx, const char *line);
bool vkc_dir(vkc_dir_cb cb, void *ctx, unsigned *count, char *err, size_t errlen);
bool vkc_get(const char *name, uint8_t *buf, size_t max, size_t *len,
             uint16_t *baud, uint8_t *stop_bits, char *err, size_t errlen);
bool vkc_put(const char *name, const uint8_t *buf, size_t len, const kcs_params_t *p,
             char *err, size_t errlen);

#endif
