/*
 * config.h - persistent settings, stored in the last flash sector.
 */
#ifndef VKC_CONFIG_H
#define VKC_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t magic;
    uint32_t version;
    char ssid[33];
    char pass[65];
    char server[64];      /* host name or IPv4 address of the tape daemon */
    uint16_t port;
    uint16_t baud;        /* KCS baud: 300, 600, 1200 */
    uint8_t stop_bits;    /* 1 or 2 */
    uint8_t uart_echo;    /* echo characters typed on the command UART */
    uint16_t leader_ms;   /* mark tone before data on playback */
    uint16_t trailer_ms;  /* mark tone after data on playback */
    uint16_t idle_ms;     /* recording ends after this long without a byte */
    uint32_t uart_baud;   /* command UART speed */
    /* --- version 2 --- */
    uint8_t remote;       /* remote_mode_t: motor-control input off / on / inv */
    uint8_t reserved[3];
    char tape[64];        /* tape the PLAY button plays (VKC_NAME_MAX + 1) */
    uint32_t crc;         /* CRC-32 of everything above */
} vkc_config_t;

extern vkc_config_t g_cfg;

void config_defaults(vkc_config_t *c);
/* Load from flash; falls back to defaults.  Returns true if flash was valid. */
bool config_load(void);
bool config_save(void);

#endif
