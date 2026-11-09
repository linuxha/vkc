/*
 * net.c - Wi-Fi and a minimal blocking TCP client on lwIP's raw API.
 */
#include "net.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/netif.h"
#include "lwip/tcp.h"
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "vkc_proto.h"

#define CONNECT_TIMEOUT_MS 8000
#define IO_TIMEOUT_MS      10000  /* inactivity timeout */
#define WIFI_RETRY_MS      15000

static net_log_fn log_fn;
static char ip_str[20] = "-";

static void logf_(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (log_fn)
        log_fn(buf);
}

static void poll_wait(void)
{
    cyw43_arch_poll();
    cyw43_arch_wait_for_work_until(make_timeout_time_ms(1));
}

/* ------------------------------------------------------------------ */
/* Wi-Fi                                                               */
/* ------------------------------------------------------------------ */

static int last_link = 0x7fff;
static absolute_time_t wifi_started;
static bool wifi_requested;

bool net_init(net_log_fn log)
{
    log_fn = log;
    if (cyw43_arch_init()) {
        logf_("wifi: cyw43 init failed");
        return false;
    }
    cyw43_arch_enable_sta_mode();
    netif_set_hostname(&cyw43_state.netif[CYW43_ITF_STA], "vkc");
    return true;
}

void net_wifi_connect(void)
{
    wifi_requested = false;
    if (!g_cfg.ssid[0]) {
        logf_("wifi: no SSID set (use: set ssid <name>, set pass <password>, save)");
        return;
    }
    cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
    const char *pw = g_cfg.pass[0] ? g_cfg.pass : NULL;
    uint32_t auth = pw ? CYW43_AUTH_WPA2_MIXED_PSK : CYW43_AUTH_OPEN;
    if (cyw43_arch_wifi_connect_async(g_cfg.ssid, pw, auth)) {
        logf_("wifi: connect request failed");
        return;
    }
    wifi_requested = true;
    wifi_started = get_absolute_time();
    last_link = 0x7fff;
    logf_("wifi: joining '%s'", g_cfg.ssid);
}

static int link_status(void)
{
    return cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
}

bool net_up(void)
{
    return link_status() == CYW43_LINK_UP;
}

const char *net_status(void)
{
    switch (link_status()) {
    case CYW43_LINK_DOWN:    return wifi_requested ? "down" : "not configured";
    case CYW43_LINK_JOIN:    return "joining";
    case CYW43_LINK_NOIP:    return "waiting for DHCP";
    case CYW43_LINK_UP:      return "up";
    case CYW43_LINK_FAIL:    return "failed";
    case CYW43_LINK_NONET:   return "network not found";
    case CYW43_LINK_BADAUTH: return "wrong password";
    default:                 return "?";
    }
}

const char *net_ip(void)
{
    return ip_str;
}

void net_service(void)
{
    cyw43_arch_poll();

    int st = link_status();
    if (st != last_link) {
        last_link = st;
        if (st == CYW43_LINK_UP) {
            snprintf(ip_str, sizeof ip_str, "%s",
                     ip4addr_ntoa(netif_ip4_addr(&cyw43_state.netif[CYW43_ITF_STA])));
            logf_("wifi: up, address %s", ip_str);
        } else {
            snprintf(ip_str, sizeof ip_str, "-");
            if (wifi_requested)
                logf_("wifi: %s", net_status());
        }
    }
    /* retry after failures, or if a join attempt hangs */
    if (wifi_requested && st != CYW43_LINK_UP &&
        absolute_time_diff_us(wifi_started, get_absolute_time()) > WIFI_RETRY_MS * 1000ll) {
        if (st < 0 || st == CYW43_LINK_DOWN || st == CYW43_LINK_JOIN || st == CYW43_LINK_NOIP)
            net_wifi_connect();
    }
}

/* ------------------------------------------------------------------ */
/* Blocking TCP client (one connection at a time)                      */
/* ------------------------------------------------------------------ */

#define RX_SIZE 16384u /* must be >= TCP_WND so received data always fits */
_Static_assert(RX_SIZE >= TCP_WND, "receive ring smaller than TCP window");

static struct {
    struct tcp_pcb *pcb;
    bool connected;
    bool closed;   /* peer closed */
    bool failed;   /* connection error; pcb already freed by lwIP */
    uint8_t rx[RX_SIZE];
    size_t rx_head, rx_len;
} C;

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    (void)arg;
    (void)pcb;
    (void)err;
    if (!p) {
        C.closed = true;
        return ERR_OK;
    }
    if (p->tot_len > RX_SIZE - C.rx_len)
        return ERR_MEM; /* lwIP keeps it and retries */
    size_t tail = (C.rx_head + C.rx_len) % RX_SIZE;
    size_t first = RX_SIZE - tail;
    if (first > p->tot_len)
        first = p->tot_len;
    pbuf_copy_partial(p, C.rx + tail, (u16_t)first, 0);
    if (first < p->tot_len)
        pbuf_copy_partial(p, C.rx, (u16_t)(p->tot_len - first), (u16_t)first);
    C.rx_len += p->tot_len;
    pbuf_free(p);
    return ERR_OK;
}

static void on_err(void *arg, err_t err)
{
    (void)arg;
    (void)err;
    C.pcb = NULL;
    C.failed = true;
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err)
{
    (void)arg;
    (void)pcb;
    C.connected = err == ERR_OK;
    return ERR_OK;
}

static void conn_close(void)
{
    if (C.pcb) {
        tcp_arg(C.pcb, NULL);
        tcp_recv(C.pcb, NULL);
        tcp_err(C.pcb, NULL);
        if (tcp_close(C.pcb) != ERR_OK)
            tcp_abort(C.pcb);
        C.pcb = NULL;
    }
    C.connected = false;
}

static volatile int dns_state; /* 0 pending, 1 ok, -1 failed */
static ip_addr_t dns_result;

static void on_dns(const char *name, const ip_addr_t *ip, void *arg)
{
    (void)name;
    (void)arg;
    if (ip) {
        dns_result = *ip;
        dns_state = 1;
    } else {
        dns_state = -1;
    }
}

static bool resolve(const char *host, ip_addr_t *out, char *err, size_t errlen)
{
    if (ipaddr_aton(host, out))
        return true;
    dns_state = 0;
    err_t e = dns_gethostbyname(host, out, on_dns, NULL);
    if (e == ERR_OK)
        return true;
    if (e != ERR_INPROGRESS) {
        snprintf(err, errlen, "cannot resolve %s", host);
        return false;
    }
    absolute_time_t deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    while (dns_state == 0 && !time_reached(deadline))
        poll_wait();
    if (dns_state != 1) {
        snprintf(err, errlen, "cannot resolve %s", host);
        return false;
    }
    *out = dns_result;
    return true;
}

static bool conn_open(char *err, size_t errlen)
{
    ip_addr_t addr;

    if (!net_up()) {
        snprintf(err, errlen, "network %s", net_status());
        return false;
    }
    if (!g_cfg.server[0]) {
        snprintf(err, errlen, "no server set (set server <host>)");
        return false;
    }
    if (!resolve(g_cfg.server, &addr, err, errlen))
        return false;

    C.pcb = NULL;
    C.connected = C.closed = C.failed = false;
    C.rx_head = C.rx_len = 0;
    C.pcb = tcp_new_ip_type(IP_GET_TYPE(&addr));
    if (!C.pcb) {
        snprintf(err, errlen, "out of memory");
        return false;
    }
    tcp_arg(C.pcb, NULL);
    tcp_recv(C.pcb, on_recv);
    tcp_err(C.pcb, on_err);
    if (tcp_connect(C.pcb, &addr, g_cfg.port, on_connected) != ERR_OK) {
        conn_close();
        snprintf(err, errlen, "connect failed");
        return false;
    }
    absolute_time_t deadline = make_timeout_time_ms(CONNECT_TIMEOUT_MS);
    while (!C.connected && !C.failed && !time_reached(deadline))
        poll_wait();
    if (!C.connected) {
        conn_close();
        snprintf(err, errlen, "cannot connect to %s:%u", g_cfg.server, g_cfg.port);
        return false;
    }
    return true;
}

static bool conn_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    absolute_time_t deadline = make_timeout_time_ms(IO_TIMEOUT_MS);
    while (len) {
        if (!C.pcb || C.failed || time_reached(deadline))
            return false;
        size_t n = tcp_sndbuf(C.pcb);
        if (n > len)
            n = len;
        if (n > 0xFFFF)
            n = 0xFFFF;
        if (n && tcp_write(C.pcb, p, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK) {
            p += n;
            len -= n;
            deadline = make_timeout_time_ms(IO_TIMEOUT_MS);
        } else {
            tcp_output(C.pcb);
            poll_wait();
        }
    }
    tcp_output(C.pcb);
    return true;
}

static bool conn_printf(const char *fmt, ...)
{
    char line[VKC_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    return n > 0 && (size_t)n < sizeof line && conn_write(line, (size_t)n);
}

/* Read exactly len bytes (buf may be NULL to discard). */
static bool conn_read(uint8_t *buf, size_t len)
{
    absolute_time_t deadline = make_timeout_time_ms(IO_TIMEOUT_MS);
    while (len) {
        if (C.rx_len) {
            size_t n = RX_SIZE - C.rx_head;
            if (n > C.rx_len)
                n = C.rx_len;
            if (n > len)
                n = len;
            if (buf) {
                memcpy(buf, C.rx + C.rx_head, n);
                buf += n;
            }
            C.rx_head = (C.rx_head + n) % RX_SIZE;
            C.rx_len -= n;
            len -= n;
            if (C.pcb)
                tcp_recved(C.pcb, (u16_t)n);
            deadline = make_timeout_time_ms(IO_TIMEOUT_MS);
            continue;
        }
        if (C.closed || C.failed || time_reached(deadline))
            return false;
        poll_wait();
    }
    return true;
}

static bool conn_line(char *out, size_t max)
{
    size_t n = 0;
    for (;;) {
        uint8_t c;
        if (!conn_read(&c, 1))
            return false;
        if (c == '\n')
            break;
        if (n + 1 >= max)
            return false;
        out[n++] = (char)c;
    }
    if (n && out[n - 1] == '\r')
        n--;
    out[n] = 0;
    return true;
}

/* ------------------------------------------------------------------ */
/* VKC/1 protocol                                                      */
/* ------------------------------------------------------------------ */

/* Expect "OK ..."; on success returns pointer to the text after "OK". */
static const char *expect_ok(char *line, size_t max, char *err, size_t errlen)
{
    if (!conn_line(line, max)) {
        snprintf(err, errlen, "no reply from server");
        return NULL;
    }
    if (!strncmp(line, "OK", 2) && (line[2] == 0 || line[2] == ' '))
        return line[2] ? line + 3 : line + 2;
    snprintf(err, errlen, "server: %s", line);
    return NULL;
}

static bool session_open(char *err, size_t errlen)
{
    char id[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    char line[VKC_LINE_MAX];

    if (!conn_open(err, errlen))
        return false;
    pico_get_unique_board_id_string(id, sizeof id);
    if (!conn_printf("HELLO %s vkc-pico-%s\n", VKC_PROTO_VERSION, id) ||
        !expect_ok(line, sizeof line, err, errlen)) {
        if (!err[0])
            snprintf(err, errlen, "handshake failed");
        conn_close();
        return false;
    }
    return true;
}

static void session_close(void)
{
    conn_printf("QUIT\n");
    conn_close();
}

/* Find key=value in a reply's attribute list. */
static bool attr_num(const char *attrs, const char *key, unsigned long *out)
{
    size_t kl = strlen(key);
    for (const char *p = attrs; p && *p;) {
        while (*p == ' ')
            p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            *out = strtoul(p + kl + 1, NULL, 10);
            return true;
        }
        p = strchr(p, ' ');
    }
    return false;
}

bool vkc_dir(vkc_dir_cb cb, void *ctx, unsigned *count, char *err, size_t errlen)
{
    char line[VKC_LINE_MAX];
    const char *r;
    err[0] = 0;
    if (!session_open(err, errlen))
        return false;
    bool ok = conn_printf("DIR\n") && (r = expect_ok(line, sizeof line, err, errlen));
    if (ok) {
        unsigned n = (unsigned)strtoul(r, NULL, 10);
        for (unsigned i = 0; i < n && ok; i++) {
            ok = conn_line(line, sizeof line);
            if (ok)
                cb(ctx, line);
        }
        if (!ok)
            snprintf(err, errlen, "connection lost");
        *count = n;
    }
    session_close();
    return ok;
}

bool vkc_get(const char *name, uint8_t *buf, size_t max, size_t *len,
             uint16_t *baud, uint8_t *stop_bits, char *err, size_t errlen)
{
    char line[VKC_LINE_MAX];
    const char *r;
    err[0] = 0;
    if (!session_open(err, errlen))
        return false;
    bool ok = conn_printf("GET %s\n", name) && (r = expect_ok(line, sizeof line, err, errlen));
    if (ok) {
        char *end;
        unsigned long n = strtoul(r, &end, 10), v;
        if (end == r) {
            snprintf(err, errlen, "bad reply from server");
            ok = false;
        } else if (n > max) {
            snprintf(err, errlen, "tape too large (%lu bytes, max %u)", n, (unsigned)max);
            ok = false;
        } else {
            if (attr_num(end, "baud", &v))
                *baud = (uint16_t)v;
            if (attr_num(end, "stop", &v))
                *stop_bits = (uint8_t)v;
            ok = conn_read(buf, n);
            if (!ok)
                snprintf(err, errlen, "connection lost");
            *len = n;
        }
    }
    if (ok)
        session_close();
    else
        conn_close();
    return ok;
}

bool vkc_put(const char *name, const uint8_t *buf, size_t len, const kcs_params_t *p,
             char *err, size_t errlen)
{
    char line[VKC_LINE_MAX];
    err[0] = 0;
    if (!session_open(err, errlen))
        return false;
    bool ok = conn_printf("PUT %s %u baud=%u stop=%u\n", name, (unsigned)len, p->baud,
                          p->stop_bits) &&
              conn_write(buf, len);
    if (!ok)
        snprintf(err, errlen, "connection lost");
    else
        ok = expect_ok(line, sizeof line, err, errlen) != NULL;
    session_close();
    return ok;
}
