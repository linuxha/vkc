/*
 * tape.h - cassette audio in/out on two GPIOs.
 *
 * Playback toggles VKC_PIN_TAPE_OUT once per half-cycle from a timer alarm,
 * producing a 1200/2400 Hz square wave that an RC network turns into
 * line-level audio for the computer's cassette input.
 *
 * Recording timestamps every edge on VKC_PIN_TAPE_IN (driven by a
 * comparator from the computer's cassette output) in an interrupt; the main
 * loop feeds the intervals to the shared KCS decoder.
 */
#ifndef VKC_TAPE_H
#define VKC_TAPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kcs.h"

void tape_init(void);

/*
 * Cassette motor.  When a job is "gated", playback pauses (output held
 * low) and recording ignores the input while the motor is stopped, just
 * as a real recorder's tape stops moving.  Safe to call from an IRQ.
 */
void tape_motor(bool run);

/* Playback.  data must stay valid until tape_play_busy() is false. */
bool tape_play_start(const uint8_t *data, size_t len, const kcs_params_t *p,
                     uint32_t leader_ms, uint32_t trailer_ms, bool gated);
bool tape_play_busy(void);
size_t tape_play_pos(void); /* bytes started so far */
void tape_play_stop(void);

/* Recording into buf (at most max bytes). */
void tape_rec_start(const kcs_params_t *p, uint8_t *buf, size_t max, bool gated);
void tape_rec_poll(void);           /* call often from the main loop */
size_t tape_rec_len(void);
bool tape_rec_overflow(void);       /* buffer full, bytes were dropped */
bool tape_rec_carrier(void);        /* tone currently present */
uint32_t tape_rec_ms_since_byte(void); /* UINT32_MAX if nothing yet */
const kcs_dec_t *tape_rec_stats(void);
void tape_rec_stop(void);

#endif
