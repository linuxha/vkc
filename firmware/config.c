/*
 * config.c - persistent settings in the last 4 KiB sector of flash.
 */
#include "config.h"

#include <stddef.h>
#include <string.h>

#include "hardware/flash.h"
#include "pico/flash.h"
#include "pico/stdlib.h"
#include "vkc_proto.h"

#define CFG_MAGIC   0x564B4331u /* "VKC1" */
#define CFG_VERSION 2u
#define CFG_SIZE    (2u * FLASH_PAGE_SIZE)

/* Version 1 ended with the CRC where version 2 starts its new fields. */
#define CFG_V1_CRC  offsetof(vkc_config_t, remote)
#define CFG_OFFSET  (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

_Static_assert(sizeof(vkc_config_t) <= CFG_SIZE, "config too big");
_Static_assert(sizeof(((vkc_config_t *)0)->tape) == VKC_NAME_MAX + 1, "tape name size");

vkc_config_t g_cfg;

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFFu;
    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1u));
    }
    return ~crc;
}

void config_defaults(vkc_config_t *c)
{
    memset(c, 0, sizeof *c);
    c->magic = CFG_MAGIC;
    c->version = CFG_VERSION;
    c->port = VKC_DEFAULT_PORT;
    c->baud = 300;
    c->stop_bits = 2;
    c->uart_echo = 0;
    c->leader_ms = 5000;
    c->trailer_ms = 1000;
    c->idle_ms = 3000;
    c->uart_baud = 9600;
    c->remote = 0;
}

static void sanitize(vkc_config_t *c)
{
    /* never trust strings from flash to be terminated */
    c->ssid[sizeof c->ssid - 1] = 0;
    c->pass[sizeof c->pass - 1] = 0;
    c->server[sizeof c->server - 1] = 0;
    c->tape[sizeof c->tape - 1] = 0;
    if (c->tape[0] && !vkc_name_valid(c->tape))
        c->tape[0] = 0;
    if (c->remote > 2)
        c->remote = 0;
}

bool config_load(void)
{
    const vkc_config_t *f = (const vkc_config_t *)(XIP_BASE + CFG_OFFSET);
    if (f->magic == CFG_MAGIC && f->version == CFG_VERSION &&
        f->crc == crc32(f, offsetof(vkc_config_t, crc))) {
        g_cfg = *f;
        sanitize(&g_cfg);
        return true;
    }
    uint32_t v1crc;
    memcpy(&v1crc, (const uint8_t *)f + CFG_V1_CRC, sizeof v1crc);
    if (f->magic == CFG_MAGIC && f->version == 1 && v1crc == crc32(f, CFG_V1_CRC)) {
        /* older firmware's settings: keep them, default the new fields */
        config_defaults(&g_cfg);
        memcpy(&g_cfg, f, CFG_V1_CRC);
        g_cfg.version = CFG_VERSION;
        sanitize(&g_cfg);
        return true;
    }
    config_defaults(&g_cfg);
    return false;
}

static uint8_t page[CFG_SIZE];

static void flash_write(void *arg)
{
    (void)arg;
    flash_range_erase(CFG_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CFG_OFFSET, page, CFG_SIZE);
}

bool config_save(void)
{
    g_cfg.magic = CFG_MAGIC;
    g_cfg.version = CFG_VERSION;
    g_cfg.crc = crc32(&g_cfg, offsetof(vkc_config_t, crc));
    memset(page, 0xFF, sizeof page);
    memcpy(page, &g_cfg, sizeof g_cfg);
    if (flash_safe_execute(flash_write, NULL, 500) != PICO_OK)
        return false;
    return memcmp((const void *)(XIP_BASE + CFG_OFFSET), &g_cfg, sizeof g_cfg) == 0;
}
