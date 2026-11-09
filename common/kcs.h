/*
 * kcs.h - Kansas City Standard (KCS / CUTS) encoder and decoder.
 *
 * Portable C99, no dependencies.  Shared by the Pico W firmware and the
 * Linux kctool so both sides use exactly the same modulation rules.
 *
 * Modulation: a '0' bit (space) is 1200 Hz, a '1' bit (mark) is 2400 Hz.
 *   300 baud  (classic KCS): 0 = 4 cycles of 1200 Hz, 1 = 8 cycles of 2400 Hz
 *   600 baud                : 0 = 2 cycles,           1 = 4 cycles
 *   1200 baud (CUTS)        : 0 = 1 cycle,            1 = 2 cycles
 * Framing: 1 start bit (0), 8 data bits LSB first, 1 or 2 stop bits (1).
 * The line idles at mark (2400 Hz); a leader of mark tone precedes data.
 *
 * Both encoder and decoder work in "half-cycles": the interval between two
 * consecutive zero crossings of the audio.  This maps directly onto GPIO
 * edge timing on the Pico and onto zero-crossing detection on a WAV file.
 */
#ifndef KCS_H
#define KCS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KCS_F0_HZ 1200u /* space, logic 0 */
#define KCS_F1_HZ 2400u /* mark,  logic 1 */

/* Nominal half-cycle lengths in nanoseconds (truncated). */
#define KCS_HALF0_NS (1000000000u / (2u * KCS_F0_HZ)) /* 416666 */
#define KCS_HALF1_NS (1000000000u / (2u * KCS_F1_HZ)) /* 208333 */

typedef struct {
    uint16_t baud;     /* 300, 600 or 1200 */
    uint8_t stop_bits; /* 1 or 2 */
} kcs_params_t;

#define KCS_PARAMS_DEFAULT ((kcs_params_t){ .baud = 300, .stop_bits = 2 })

bool kcs_params_valid(const kcs_params_t *p);

/* Number of half-cycles that make up one bit of the given value. */
unsigned kcs_halves_per_bit(const kcs_params_t *p, int bit);

/* ------------------------------------------------------------------ */
/* Encoder: turns a byte buffer into a sequence of half-cycle tones.   */
/* ------------------------------------------------------------------ */

typedef struct {
    kcs_params_t p;
    const uint8_t *data;
    size_t len;
    size_t pos;
    uint32_t bits_left;    /* leader / trailer bits still to send */
    uint32_t trailer_bits;
    uint16_t frame;        /* remaining bits of current frame, LSB first */
    uint8_t frame_bits;
    uint8_t stage;         /* internal */
    uint8_t tone;          /* tone of current bit */
    uint8_t halves_left;   /* half-cycles left in current bit */
} kcs_enc_t;

void kcs_enc_init(kcs_enc_t *e, const kcs_params_t *p, const uint8_t *data,
                  size_t len, uint32_t leader_ms, uint32_t trailer_ms);

/* Tone of the next half-cycle: 0 (1200 Hz) or 1 (2400 Hz); -1 when done. */
int kcs_enc_next(kcs_enc_t *e);

/* ------------------------------------------------------------------ */
/* Decoder: consumes half-cycle durations, emits bytes.                */
/* ------------------------------------------------------------------ */

typedef void (*kcs_byte_cb)(void *ctx, uint8_t byte);

typedef struct {
    kcs_params_t p;
    uint32_t bit_ns;      /* nominal bit time */
    uint32_t bit_cur;     /* bit time corrected for measured tape speed */
    uint32_t half1_est;   /* measured length of a mark half-cycle */
    uint64_t t;           /* time at end of last accepted half-cycle (ns) */
    uint64_t mark_start;  /* start of current run of mark, or UINT64_MAX */
    uint64_t space_start; /* start of current run of space, or UINT64_MAX */
    uint64_t mark_len;    /* length of the mark run before the current space */
    uint64_t last_edge;   /* leading edge of the last start bit used */
    uint64_t win_start;   /* voting window of the bit being received */
    uint64_t win_end;
    uint32_t acc_mark;    /* ns of mark / space seen inside the window */
    uint32_t acc_space;
    uint32_t pending_ns;  /* glitch time carried into the next half-cycle */
    uint64_t held_ns;     /* one half-cycle of look-ahead for the glitch filter */
    int8_t state;         /* -1 idle (hunting for start bit), 0..9 bit index */
    int8_t last_tone;
    uint16_t shreg;
    bool carrier;
    kcs_byte_cb cb;
    void *ctx;
    /* statistics */
    uint32_t bytes;
    uint32_t framing_errors;
    uint32_t glitches;
    uint32_t dropouts;
} kcs_dec_t;

void kcs_dec_init(kcs_dec_t *d, const kcs_params_t *p, kcs_byte_cb cb, void *ctx);

/* Feed the time between two consecutive zero crossings, in ns. */
void kcs_dec_half(kcs_dec_t *d, uint32_t half_ns);

/*
 * Signal lost (silence, buffer overrun, end of input): flushes the one
 * half-cycle of look-ahead and resynchronises.  Call it at end of input.
 */
void kcs_dec_gap(kcs_dec_t *d);

#ifdef __cplusplus
}
#endif

#endif /* KCS_H */
