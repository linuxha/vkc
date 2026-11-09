/*
 * tape.c - cassette audio in/out.  See tape.h.
 */
#include "tape.h"

#include "board.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "pico/stdlib.h"

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

/*
 * Half-cycle lengths are 416.67 us (1200 Hz) and 208.33 us (2400 Hz).  Keep
 * the remainder in units of 1/(2*F1) us so the output frequency is exact
 * on average and does not drift over a long tape.
 */
#define TX_DEN (2u * KCS_F1_HZ)

static kcs_enc_t tx_enc;
static volatile bool tx_busy;
static volatile bool tx_abort;
static alarm_id_t tx_alarm_id;
static uint32_t tx_acc;
static bool tx_level;
static bool tx_gated;

/* Cassette motor: set from the panel timer, read by playback and recording. */
static volatile bool motor_run = true;

void tape_motor(bool run)
{
    motor_run = run;
}

static int64_t tx_alarm(alarm_id_t id, void *ud)
{
    (void)id;
    (void)ud;
    if (!tx_abort && tx_gated && !motor_run) {
        /* motor stopped: the tape pauses where it is and the output goes quiet */
        if (tx_level) {
            tx_level = false;
            gpio_put(VKC_PIN_TAPE_OUT, 0);
        }
        return -1000;
    }
    int tone = tx_abort ? -1 : kcs_enc_next(&tx_enc);
    if (tone < 0) {
        gpio_put(VKC_PIN_TAPE_OUT, 0);
        tx_level = false;
        tx_busy = false;
        return 0;
    }
    tx_level = !tx_level;
    gpio_put(VKC_PIN_TAPE_OUT, tx_level);

    tx_acc += tone ? 1000000u : 2000000u; /* half-cycle in us * TX_DEN */
    uint32_t us = tx_acc / TX_DEN;
    tx_acc %= TX_DEN;
    /* negative: relative to when this alarm was due, so latency doesn't accumulate */
    return -(int64_t)us;
}

bool tape_play_start(const uint8_t *data, size_t len, const kcs_params_t *p,
                     uint32_t leader_ms, uint32_t trailer_ms, bool gated)
{
    if (tx_busy || !kcs_params_valid(p))
        return false;
    kcs_enc_init(&tx_enc, p, data, len, leader_ms, trailer_ms);
    tx_acc = 0;
    tx_level = false;
    tx_abort = false;
    tx_gated = gated;
    tx_busy = true;
    tx_alarm_id = add_alarm_in_us(1000, tx_alarm, NULL, true);
    if (tx_alarm_id <= 0) {
        tx_busy = false;
        return false;
    }
    return true;
}

bool tape_play_busy(void)
{
    return tx_busy;
}

size_t tape_play_pos(void)
{
    return tx_enc.pos;
}

void tape_play_stop(void)
{
    if (!tx_busy)
        return;
    tx_abort = true;
    absolute_time_t deadline = make_timeout_time_ms(10);
    while (tx_busy && !time_reached(deadline))
        tight_loop_contents();
    if (tx_busy) { /* should not happen; force it */
        cancel_alarm(tx_alarm_id);
        gpio_put(VKC_PIN_TAPE_OUT, 0);
        tx_busy = false;
    }
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

#define EDGE_RING 2048u /* power of two; ~0.4 s of 2400 Hz edges */

static volatile uint32_t edge_ring[EDGE_RING];
static volatile uint32_t edge_head;
static uint32_t edge_tail;
static uint32_t edge_prev;
static bool edge_have_prev;
static volatile uint32_t edge_last_us;

static kcs_dec_t rx_dec;
static uint8_t *rx_buf;
static size_t rx_max, rx_len;
static bool rx_overflow;
static bool rx_active;
static bool rx_gated;
static bool rx_have_byte;
static uint32_t rx_last_byte_us;

static void __not_in_flash_func(edge_isr)(void)
{
    uint32_t ev = gpio_get_irq_event_mask(VKC_PIN_TAPE_IN);
    if (ev & (GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL)) {
        gpio_acknowledge_irq(VKC_PIN_TAPE_IN, ev);
        uint32_t now = time_us_32();
        uint32_t h = edge_head;
        edge_ring[h & (EDGE_RING - 1u)] = now;
        edge_head = h + 1u;
        edge_last_us = now;
    }
}

static void rx_byte(void *ctx, uint8_t b)
{
    (void)ctx;
    if (rx_len < rx_max)
        rx_buf[rx_len++] = b;
    else
        rx_overflow = true;
    rx_have_byte = true;
    rx_last_byte_us = time_us_32();
}

void tape_rec_start(const kcs_params_t *p, uint8_t *buf, size_t max, bool gated)
{
    tape_rec_stop();
    kcs_dec_init(&rx_dec, p, rx_byte, NULL);
    rx_buf = buf;
    rx_max = max;
    rx_len = 0;
    rx_overflow = false;
    rx_have_byte = false;
    edge_tail = edge_head;
    edge_have_prev = false;
    rx_gated = gated;
    rx_active = true;
    gpio_acknowledge_irq(VKC_PIN_TAPE_IN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL);
    gpio_set_irq_enabled(VKC_PIN_TAPE_IN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true);
}

void tape_rec_poll(void)
{
    if (!rx_active)
        return;
    uint32_t head = edge_head;
    if (rx_gated && !motor_run) {
        /* motor stopped: nothing reaches the tape */
        edge_tail = head;
        edge_have_prev = false;
        if (rx_dec.carrier)
            kcs_dec_gap(&rx_dec);
        return;
    }
    if (head - edge_tail > EDGE_RING) {
        /* main loop stalled too long; lose sync rather than decode garbage */
        edge_tail = head;
        edge_have_prev = false;
        kcs_dec_gap(&rx_dec);
        return;
    }
    while (edge_tail != head) {
        uint32_t ts = edge_ring[edge_tail & (EDGE_RING - 1u)];
        edge_tail++;
        if (edge_have_prev) {
            uint64_t ns = (uint64_t)(ts - edge_prev) * 1000u;
            kcs_dec_half(&rx_dec, ns > UINT32_MAX ? UINT32_MAX : (uint32_t)ns);
        }
        edge_prev = ts;
        edge_have_prev = true;
    }
    /* silence produces no edges at all, so time it out here */
    if (edge_have_prev && time_us_32() - edge_last_us > 50000u && rx_dec.carrier) {
        kcs_dec_gap(&rx_dec);
        edge_have_prev = false;
    }
}

size_t tape_rec_len(void)
{
    return rx_len;
}

bool tape_rec_overflow(void)
{
    return rx_overflow;
}

bool tape_rec_carrier(void)
{
    return rx_active && rx_dec.carrier;
}

uint32_t tape_rec_ms_since_byte(void)
{
    if (!rx_have_byte)
        return UINT32_MAX;
    return (time_us_32() - rx_last_byte_us) / 1000u;
}

const kcs_dec_t *tape_rec_stats(void)
{
    return &rx_dec;
}

void tape_rec_stop(void)
{
    if (!rx_active)
        return;
    gpio_set_irq_enabled(VKC_PIN_TAPE_IN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    tape_rec_poll();
    kcs_dec_gap(&rx_dec);
    rx_active = false;
}

/* ------------------------------------------------------------------ */

void tape_init(void)
{
    gpio_init(VKC_PIN_TAPE_OUT);
    gpio_set_dir(VKC_PIN_TAPE_OUT, GPIO_OUT);
    gpio_put(VKC_PIN_TAPE_OUT, 0);

    gpio_init(VKC_PIN_TAPE_IN);
    gpio_set_dir(VKC_PIN_TAPE_IN, GPIO_IN);
    gpio_disable_pulls(VKC_PIN_TAPE_IN);
    gpio_set_input_hysteresis_enabled(VKC_PIN_TAPE_IN, true);

    gpio_add_raw_irq_handler(VKC_PIN_TAPE_IN, edge_isr);
    irq_set_priority(IO_IRQ_BANK0, PICO_HIGHEST_IRQ_PRIORITY);
    irq_set_enabled(IO_IRQ_BANK0, true);
}
