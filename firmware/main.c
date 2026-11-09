/*
 * main.c - VKC: a Kansas City Standard cassette emulator on a Pico W.
 *
 * Two command ports:
 *   - the USB console: all commands, including configuration;
 *   - the command UART (GP0/GP1): tape commands only (play, receive, dir,
 *     stop, status, help), for the vintage computer or its terminal.
 *
 * Every command ends with exactly one line starting "OK" or "ERR".
 * Other lines are informational.
 *
 * Front panel (panel.c): PLAY plays the current tape, RECORD records a new
 * one (recNNN), MOTOR toggles a manual motor override.  Pressing PLAY or
 * RECORD again stops.  Results from the buttons go to the USB console.
 */
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "hardware/uart.h"
#include "hardware/watchdog.h"
#include "kcs.h"
#include "net.h"
#include "panel.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "tape.h"
#include "vkc_proto.h"

#define VKC_VERSION "0.2.0"

/* ------------------------------------------------------------------ */
/* Command ports                                                       */
/* ------------------------------------------------------------------ */

typedef struct port {
    const char *name;
    int (*get)(void);
    void (*put)(const char *s);
    bool admin;
    char line[160];
    size_t n;
    bool last_cr;
} port_t;

static int usb_get(void)
{
    int c = getchar_timeout_us(0);
    return c < 0 ? -1 : c;
}

static void usb_put(const char *s)
{
    fputs(s, stdout); /* stdio converts \n to \r\n */
    fflush(stdout);
}

static int uart_get_(void)
{
    return uart_is_readable(VKC_CMD_UART) ? uart_getc(VKC_CMD_UART) : -1;
}

static void uart_put_(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n')
            uart_putc_raw(VKC_CMD_UART, '\r');
        uart_putc_raw(VKC_CMD_UART, *s);
    }
}

static port_t port_usb = { .name = "usb", .get = usb_get, .put = usb_put, .admin = true };
static port_t port_uart = { .name = "uart", .get = uart_get_, .put = uart_put_, .admin = false };

static void reply(port_t *p, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void reply(port_t *p, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 1, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof buf - 2)
        n = sizeof buf - 2;
    buf[n] = '\n';
    buf[n + 1] = 0;
    p->put(buf);
}

/* Log to the USB console only. */
static void log_usb(const char *msg)
{
    reply(&port_usb, "# %s", msg);
}

/* ------------------------------------------------------------------ */
/* Tape jobs                                                           */
/* ------------------------------------------------------------------ */

typedef enum { JOB_IDLE, JOB_PLAY, JOB_RECEIVE } job_t;

static uint8_t tape_buf[VKC_TAPE_MAX];
static job_t job = JOB_IDLE;
static port_t *job_port;
static char job_name[VKC_NAME_MAX + 1];
static size_t job_len;
static kcs_params_t job_params;
static char cur_tape[VKC_NAME_MAX + 1]; /* what the PLAY button plays */

static void led(bool on)
{
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
}

static kcs_params_t cfg_params(void)
{
    kcs_params_t p = { .baud = g_cfg.baud, .stop_bits = g_cfg.stop_bits };
    return p;
}

static const char *motor_desc(void)
{
    static const char *const ov[] = { "", ", forced on", ", forced off" };
    static const char *const rm[] = { "remote off", "remote on", "remote inv" };
    static char buf[64];
    snprintf(buf, sizeof buf, "motor %s (%s%s%s)", panel_motor() ? "running" : "stopped",
             rm[g_cfg.remote], g_cfg.remote && panel_remote_asserted() ? ", asserted" : "",
             ov[panel_override()]);
    return buf;
}

/* Job LEDs: steady while the tape moves, blinking while it waits for the motor. */
static void leds_update(void)
{
    led_mode_t m = panel_motor() ? LED_ON : LED_BLINK;
    panel_led(PANEL_LED_PLAY, job == JOB_PLAY ? m : LED_OFF);
    panel_led(PANEL_LED_REC, job == JOB_RECEIVE ? m : LED_OFF);
}

static void play_finish(bool stopped)
{
    tape_play_stop();
    led(false);
    panel_led(PANEL_LED_PLAY, LED_OFF);
    if (stopped)
        reply(job_port, "ERR stopped after %u of %u bytes", (unsigned)tape_play_pos(),
              (unsigned)job_len);
    else
        reply(job_port, "OK played %s, %u bytes", job_name, (unsigned)job_len);
    job = JOB_IDLE;
}

static void receive_finish(void)
{
    char err[96];
    tape_rec_stop();
    led(false);
    job = JOB_IDLE;

    size_t n = tape_rec_len();
    const kcs_dec_t *st = tape_rec_stats();
    if (st->framing_errors)
        reply(job_port, "warning: %u framing errors", (unsigned)st->framing_errors);
    if (tape_rec_overflow())
        reply(job_port, "warning: tape longer than %u bytes, truncated", (unsigned)VKC_TAPE_MAX);
    if (n == 0) {
        panel_led(PANEL_LED_REC, LED_OFF);
        panel_led_error(PANEL_LED_REC);
        reply(job_port, "ERR nothing received");
        return;
    }
    reply(job_port, "uploading %s, %u bytes", job_name, (unsigned)n);
    panel_led(PANEL_LED_REC, LED_BLINK);
    bool ok = vkc_put(job_name, tape_buf, n, &job_params, err, sizeof err);
    panel_led(PANEL_LED_REC, LED_OFF);
    if (ok) {
        snprintf(cur_tape, sizeof cur_tape, "%s", job_name);
        reply(job_port, "OK received %s, %u bytes", job_name, (unsigned)n);
    } else {
        panel_led_error(PANEL_LED_REC);
        reply(job_port, "ERR upload failed: %s", err);
    }
}

static void jobs_service(void)
{
    switch (job) {
    case JOB_PLAY:
        if (!tape_play_busy())
            play_finish(false);
        break;
    case JOB_RECEIVE: {
        tape_rec_poll();
        uint32_t idle = tape_rec_ms_since_byte();
        if (idle != UINT32_MAX && idle >= g_cfg.idle_ms)
            receive_finish();
        break;
    }
    default:
        break;
    }
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void dir_line(void *ctx, const char *line)
{
    reply((port_t *)ctx, "%s", line);
}

static bool check_idle(port_t *p)
{
    if (job == JOB_IDLE)
        return true;
    reply(p, "ERR busy (%s); use stop", job == JOB_PLAY ? "playing" : "receiving");
    return false;
}

static void cmd_dir(port_t *p)
{
    char err[96];
    unsigned n = 0;
    if (!check_idle(p))
        return;
    if (vkc_dir(dir_line, p, &n, err, sizeof err))
        reply(p, "OK %u tapes", n);
    else
        reply(p, "ERR %s", err);
}

static bool cmd_play(port_t *p, const char *name)
{
    char err[96];
    if (!check_idle(p))
        return false;
    if (!vkc_name_valid(name)) {
        reply(p, "ERR bad name");
        return false;
    }
    kcs_params_t kp = cfg_params();
    size_t len = 0;
    panel_led(PANEL_LED_PLAY, LED_BLINK);
    bool ok = vkc_get(name, tape_buf, sizeof tape_buf, &len, &kp.baud, &kp.stop_bits, err,
                      sizeof err);
    panel_led(PANEL_LED_PLAY, LED_OFF);
    if (!ok) {
        reply(p, "ERR %s", err);
        return false;
    }
    if (!kcs_params_valid(&kp))
        kp = cfg_params();
    if (!tape_play_start(tape_buf, len, &kp, g_cfg.leader_ms, g_cfg.trailer_ms, true)) {
        reply(p, "ERR cannot start playback");
        return false;
    }
    uint32_t secs = (uint32_t)(((uint64_t)len * (9u + kp.stop_bits) / kp.baud) +
                               (g_cfg.leader_ms + g_cfg.trailer_ms) / 1000u);
    job = JOB_PLAY;
    job_port = p;
    job_len = len;
    snprintf(job_name, sizeof job_name, "%s", name);
    snprintf(cur_tape, sizeof cur_tape, "%s", name);
    led(true);
    leds_update();
    reply(p, "playing %s: %u bytes at %u baud, about %u s%s", name, (unsigned)len, kp.baud,
          (unsigned)secs, panel_motor() ? "" : " once the motor starts");
    return true;
}

static bool cmd_receive(port_t *p, const char *name)
{
    if (!check_idle(p))
        return false;
    if (!vkc_name_valid(name)) {
        reply(p, "ERR bad name");
        return false;
    }
    if (!net_up()) {
        reply(p, "ERR network %s", net_status());
        return false;
    }
    job_params = cfg_params();
    tape_rec_start(&job_params, tape_buf, sizeof tape_buf, true);
    job = JOB_RECEIVE;
    job_port = p;
    snprintf(job_name, sizeof job_name, "%s", name);
    led(true);
    leds_update();
    reply(p, "ready to receive %s at %u baud; start SAVE on the computer", name,
          job_params.baud);
    return true;
}

static void cmd_stop(port_t *p)
{
    switch (job) {
    case JOB_PLAY:
        play_finish(true);
        break;
    case JOB_RECEIVE:
        receive_finish();
        break;
    default:
        break;
    }
    reply(p, "OK");
}

static void cmd_status(port_t *p)
{
    reply(p, "vkc %s, wifi %s, ip %s, server %s:%u, %u baud %u stop", VKC_VERSION,
          net_status(), net_ip(), g_cfg.server[0] ? g_cfg.server : "(unset)", g_cfg.port,
          g_cfg.baud, g_cfg.stop_bits);
    if (job != JOB_IDLE)
        reply(p, "%s", motor_desc());
    switch (job) {
    case JOB_PLAY:
        reply(p, "OK playing %s, %u of %u bytes", job_name, (unsigned)tape_play_pos(),
              (unsigned)job_len);
        break;
    case JOB_RECEIVE: {
        const kcs_dec_t *st = tape_rec_stats();
        reply(p, "OK receiving %s, %u bytes, carrier %s, %u framing errors", job_name,
              (unsigned)tape_rec_len(), tape_rec_carrier() ? "yes" : "no",
              (unsigned)st->framing_errors);
        break;
    }
    default:
        reply(p, "%s, current tape %s", motor_desc(), cur_tape[0] ? cur_tape : "(none)");
        reply(p, "OK idle");
        return;
    }
}

/*
 * Loopback self test: with a jumper from the tape output pin to the tape
 * input pin, play a pattern and decode it at the same time.  Exercises the
 * output timer, the edge interrupt and the decoder without any analog parts.
 */
static void cmd_selftest(port_t *p)
{
    if (!check_idle(p))
        return;
    kcs_params_t kp = cfg_params();
    const size_t n = 64;
    uint8_t *tx = tape_buf, *rx = tape_buf + 1024;
    for (size_t i = 0; i < n; i++)
        tx[i] = (uint8_t)(i * 37u + 11u);
    tx[0] = 0x00;
    tx[1] = 0xFF;

    reply(p, "self test: jumper GP%u to GP%u, %u bytes at %u baud", VKC_PIN_TAPE_OUT,
          VKC_PIN_TAPE_IN, (unsigned)n, kp.baud);
    tape_rec_start(&kp, rx, 1024, false);
    if (!tape_play_start(tx, n, &kp, 500, 200, false)) {
        tape_rec_stop();
        reply(p, "ERR cannot start playback");
        return;
    }
    led(true);
    while (tape_play_busy()) {
        tape_rec_poll();
        cyw43_arch_poll();
    }
    absolute_time_t until = make_timeout_time_ms(100);
    while (!time_reached(until))
        tape_rec_poll();
    tape_rec_stop();
    led(false);

    size_t got = tape_rec_len(), bad = 0;
    for (size_t i = 0; i < n; i++)
        bad += i >= got || rx[i] != tx[i];
    const kcs_dec_t *st = tape_rec_stats();
    if (got == n && !bad)
        reply(p, "OK self test passed");
    else if (got == 0)
        reply(p, "ERR nothing decoded - is the jumper fitted?");
    else
        reply(p, "ERR self test: %u bytes back, %u wrong, %u framing errors", (unsigned)got,
              (unsigned)bad, (unsigned)st->framing_errors);
}

static void cmd_help(port_t *p)
{
    reply(p, "dir                 list tapes on the server");
    reply(p, "play <name>         fetch a tape and play it to the computer");
    reply(p, "receive <name>      record from the computer and store as <name>");
    reply(p, "stop                stop playing / finish receiving now");
    reply(p, "motor [on|off|auto] force the cassette motor on/off, or follow REMOTE");
    reply(p, "status              show state");
    if (p->admin) {
        reply(p, "show                show settings");
        reply(p, "set <key> <value>   ssid pass server port baud stop leader trailer");
        reply(p, "                    idle uartbaud echo remote tape");
        reply(p, "save                write settings to flash");
        reply(p, "wifi                reconnect Wi-Fi with current settings");
        reply(p, "defaults            reset settings (not saved until 'save')");
        reply(p, "selftest            loopback test (jumper tape out pin to tape in pin)");
        reply(p, "reboot              restart");
    }
    reply(p, "OK");
}

static void cmd_show(port_t *p)
{
    reply(p, "ssid     %s", g_cfg.ssid);
    reply(p, "pass     %s", g_cfg.pass[0] ? "********" : "");
    reply(p, "server   %s", g_cfg.server);
    reply(p, "port     %u", g_cfg.port);
    reply(p, "baud     %u", g_cfg.baud);
    reply(p, "stop     %u", g_cfg.stop_bits);
    reply(p, "leader   %u ms", g_cfg.leader_ms);
    reply(p, "trailer  %u ms", g_cfg.trailer_ms);
    reply(p, "idle     %u ms", g_cfg.idle_ms);
    reply(p, "uartbaud %u", (unsigned)g_cfg.uart_baud);
    reply(p, "echo     %s", g_cfg.uart_echo ? "on" : "off");
    reply(p, "remote   %s", g_cfg.remote == REMOTE_ON ? "on" : g_cfg.remote == REMOTE_INV ? "inv" : "off");
    reply(p, "tape     %s", g_cfg.tape);
    reply(p, "OK");
}

static bool parse_ul(const char *s, unsigned long lo, unsigned long hi, unsigned long *out)
{
    char *end;
    if (!s || !*s)
        return false;
    unsigned long v = strtoul(s, &end, 10);
    if (*end || v < lo || v > hi)
        return false;
    *out = v;
    return true;
}

static void cmd_set(port_t *p, const char *key, const char *val)
{
    unsigned long v;
    if (!key) {
        reply(p, "ERR usage: set <key> <value>");
        return;
    }
    if (!val)
        val = "";

    if (!strcmp(key, "ssid")) {
        if (strlen(val) >= sizeof g_cfg.ssid)
            goto toolong;
        strcpy(g_cfg.ssid, val);
    } else if (!strcmp(key, "pass")) {
        if (strlen(val) >= sizeof g_cfg.pass)
            goto toolong;
        strcpy(g_cfg.pass, val);
    } else if (!strcmp(key, "server")) {
        if (strlen(val) >= sizeof g_cfg.server || strchr(val, ' '))
            goto toolong;
        strcpy(g_cfg.server, val);
    } else if (!strcmp(key, "port") && parse_ul(val, 1, 65535, &v)) {
        g_cfg.port = (uint16_t)v;
    } else if (!strcmp(key, "baud") && parse_ul(val, 300, 1200, &v) &&
               (v == 300 || v == 600 || v == 1200)) {
        g_cfg.baud = (uint16_t)v;
    } else if (!strcmp(key, "stop") && parse_ul(val, 1, 2, &v)) {
        g_cfg.stop_bits = (uint8_t)v;
    } else if (!strcmp(key, "leader") && parse_ul(val, 0, 60000, &v)) {
        g_cfg.leader_ms = (uint16_t)v;
    } else if (!strcmp(key, "trailer") && parse_ul(val, 0, 60000, &v)) {
        g_cfg.trailer_ms = (uint16_t)v;
    } else if (!strcmp(key, "idle") && parse_ul(val, 500, 60000, &v)) {
        g_cfg.idle_ms = (uint16_t)v;
    } else if (!strcmp(key, "uartbaud") && parse_ul(val, 110, 921600, &v)) {
        g_cfg.uart_baud = (uint32_t)v;
        uart_set_baudrate(VKC_CMD_UART, g_cfg.uart_baud);
    } else if (!strcmp(key, "echo") && (!strcmp(val, "on") || !strcmp(val, "off"))) {
        g_cfg.uart_echo = !strcmp(val, "on");
    } else if (!strcmp(key, "remote") &&
               (!strcmp(val, "off") || !strcmp(val, "on") || !strcmp(val, "inv"))) {
        g_cfg.remote = !strcmp(val, "on") ? REMOTE_ON : !strcmp(val, "inv") ? REMOTE_INV : REMOTE_OFF;
        panel_set_remote((remote_mode_t)g_cfg.remote);
    } else if (!strcmp(key, "tape") && (!*val || vkc_name_valid(val))) {
        snprintf(g_cfg.tape, sizeof g_cfg.tape, "%s", val);
        snprintf(cur_tape, sizeof cur_tape, "%s", val);
    } else {
        reply(p, "ERR bad key or value (see help)");
        return;
    }
    reply(p, "OK (use 'save' to keep%s)",
          (!strcmp(key, "ssid") || !strcmp(key, "pass")) ? ", 'wifi' to reconnect" : "");
    return;
toolong:
    reply(p, "ERR value too long");
}

static void cmd_motor(port_t *p, const char *arg)
{
    if (arg) {
        if (!strcmp(arg, "on"))
            panel_set_override(MOTOR_FORCE_ON);
        else if (!strcmp(arg, "off"))
            panel_set_override(MOTOR_FORCE_OFF);
        else if (!strcmp(arg, "auto"))
            panel_set_override(MOTOR_AUTO);
        else {
            reply(p, "ERR usage: motor [on|off|auto]");
            return;
        }
        sleep_ms(10); /* let the panel timer apply it */
    }
    reply(p, "OK %s", motor_desc());
}

/* Pick the next free recNNN name on the server for the RECORD button. */
static void rec_scan(void *ctx, const char *line)
{
    unsigned *max = ctx;
    if (strncmp(line, "rec", 3) || !isdigit((unsigned char)line[3]))
        return;
    char *end;
    unsigned long v = strtoul(line + 3, &end, 10);
    if ((*end == ' ' || !*end) && v >= *max && v < 999999)
        *max = (unsigned)v + 1;
}

static bool next_rec_name(char *name, size_t len)
{
    char err[96];
    unsigned next = 1, count;
    if (!vkc_dir(rec_scan, &next, &count, err, sizeof err)) {
        reply(&port_usb, "ERR %s", err);
        return false;
    }
    snprintf(name, len, "rec%03u", next);
    return true;
}

static void buttons_service(void)
{
    uint32_t b = panel_buttons();
    if (!b)
        return;
    port_t *p = &port_usb;

    if (b & PANEL_BTN_MOTOR) {
        motor_override_t o = panel_override();
        panel_set_override(o != MOTOR_AUTO ? MOTOR_AUTO
                           : panel_motor() ? MOTOR_FORCE_OFF : MOTOR_FORCE_ON);
        sleep_ms(10);
        reply(p, "# [MOTOR] %s", motor_desc());
    }

    if (b & PANEL_BTN_PLAY) {
        reply(p, "# [PLAY]");
        if (job == JOB_PLAY) {
            play_finish(true);
        } else if (job != JOB_IDLE) {
            panel_led_error(PANEL_LED_PLAY);
        } else if (!cur_tape[0]) {
            reply(p, "ERR no current tape; 'set tape <name>' or play one by name");
            panel_led_error(PANEL_LED_PLAY);
        } else if (!cmd_play(p, cur_tape)) {
            panel_led_error(PANEL_LED_PLAY);
        }
    }

    if (b & PANEL_BTN_REC) {
        char name[VKC_NAME_MAX + 1];
        reply(p, "# [RECORD]");
        if (job == JOB_RECEIVE) {
            receive_finish();
        } else if (job != JOB_IDLE) {
            panel_led_error(PANEL_LED_REC);
        } else if (!net_up()) {
            reply(p, "ERR network %s", net_status());
            panel_led_error(PANEL_LED_REC);
        } else if (!next_rec_name(name, sizeof name) || !cmd_receive(p, name)) {
            panel_led_error(PANEL_LED_REC);
        }
    }
}

static void dispatch(port_t *p, char *line)
{
    /* split off command word; the rest is kept intact for values with spaces */
    while (isspace((unsigned char)*line))
        line++;
    char *cmd = line;
    while (*line && !isspace((unsigned char)*line))
        line++;
    if (*line)
        *line++ = 0;
    while (isspace((unsigned char)*line))
        line++;
    char *rest = *line ? line : NULL;
    for (char *c = cmd; *c; c++)
        *c = (char)tolower((unsigned char)*c);
    if (!*cmd)
        return;

    /* first argument (for commands taking a single word) */
    char *arg = rest;
    if (arg) {
        char *e = arg + strlen(arg);
        while (e > arg && isspace((unsigned char)e[-1]))
            *--e = 0;
    }

    if (!strcmp(cmd, "dir") || !strcmp(cmd, "ls")) {
        cmd_dir(p);
    } else if (!strcmp(cmd, "play") || !strcmp(cmd, "load")) {
        cmd_play(p, arg ? arg : "");
    } else if (!strcmp(cmd, "receive") || !strcmp(cmd, "record")) {
        cmd_receive(p, arg ? arg : "");
    } else if (!strcmp(cmd, "stop")) {
        cmd_stop(p);
    } else if (!strcmp(cmd, "motor")) {
        cmd_motor(p, arg);
    } else if (!strcmp(cmd, "status")) {
        cmd_status(p);
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
        cmd_help(p);
    } else if (!p->admin) {
        reply(p, "ERR unknown command (help)");
    } else if (!strcmp(cmd, "show")) {
        cmd_show(p);
    } else if (!strcmp(cmd, "set")) {
        char *key = rest, *val = NULL;
        if (key) {
            val = key;
            while (*val && !isspace((unsigned char)*val))
                val++;
            if (*val)
                *val++ = 0;
            while (isspace((unsigned char)*val))
                val++;
            for (char *c = key; *c; c++)
                *c = (char)tolower((unsigned char)*c);
        }
        cmd_set(p, key, val);
    } else if (!strcmp(cmd, "save")) {
        if (job != JOB_IDLE)
            reply(p, "ERR busy");
        else
            reply(p, config_save() ? "OK saved" : "ERR flash write failed");
    } else if (!strcmp(cmd, "wifi")) {
        net_wifi_connect();
        reply(p, "OK");
    } else if (!strcmp(cmd, "selftest")) {
        cmd_selftest(p);
    } else if (!strcmp(cmd, "defaults")) {
        config_defaults(&g_cfg);
        panel_set_remote((remote_mode_t)g_cfg.remote);
        reply(p, "OK defaults loaded (not saved)");
    } else if (!strcmp(cmd, "reboot")) {
        reply(p, "OK rebooting");
        sleep_ms(100);
        watchdog_reboot(0, 0, 0);
    } else {
        reply(p, "ERR unknown command (help)");
    }
}

static void port_service(port_t *p, bool echo)
{
    int c;
    while ((c = p->get()) >= 0) {
        if (c == '\r' || c == '\n') {
            bool skip = c == '\n' && p->last_cr; /* CR LF counts once */
            p->last_cr = c == '\r';
            if (skip)
                continue;
            if (echo)
                p->put("\n");
            p->line[p->n] = 0;
            p->n = 0;
            dispatch(p, p->line);
            return; /* let the main loop run between commands */
        }
        p->last_cr = false;
        if (c == 0x08 || c == 0x7f) {
            if (p->n) {
                p->n--;
                if (echo)
                    p->put("\b \b");
            }
        } else if (c == 0x03 || c == 0x15) { /* ^C / ^U: discard line */
            p->n = 0;
            if (echo)
                p->put("^C\n");
        } else if (c >= 0x20 && c < 0x7f && p->n < sizeof p->line - 1) {
            p->line[p->n++] = (char)c;
            if (echo) {
                char s[2] = { (char)c, 0 };
                p->put(s);
            }
        }
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    stdio_init_all();

    bool cfg_ok = config_load();

    uart_init(VKC_CMD_UART, g_cfg.uart_baud);
    gpio_set_function(VKC_CMD_UART_TX, GPIO_FUNC_UART);
    gpio_set_function(VKC_CMD_UART_RX, GPIO_FUNC_UART);

    tape_init();
    panel_init((remote_mode_t)g_cfg.remote);
    snprintf(cur_tape, sizeof cur_tape, "%s", g_cfg.tape);

    /* give a USB terminal a moment to attach so the banner is seen */
    for (int i = 0; i < 20 && !stdio_usb_connected(); i++)
        sleep_ms(100);

    reply(&port_usb, "# VKC %s - Kansas City tape emulator (type 'help')", VKC_VERSION);
    if (!cfg_ok)
        reply(&port_usb, "# no saved settings, using defaults");

    if (!net_init(log_usb))
        reply(&port_usb, "# Wi-Fi unavailable; tape commands needing the server will fail");
    else
        net_wifi_connect();

    reply(&port_uart, "VKC %s ready", VKC_VERSION);

    for (;;) {
        net_service();
        jobs_service();
        buttons_service();
        leds_update();
        port_service(&port_usb, true);
        jobs_service();
        port_service(&port_uart, g_cfg.uart_echo);
        if (job == JOB_IDLE)
            cyw43_arch_wait_for_work_until(make_timeout_time_ms(1));
    }
}
