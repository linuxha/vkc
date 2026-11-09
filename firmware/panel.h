/*
 * panel.h - front panel: PLAY / RECORD / MOTOR buttons, their LEDs, and
 * the computer's cassette motor-control (REMOTE) input.
 *
 * A 5 ms timer interrupt debounces the inputs, works out whether the
 * cassette motor is running (and tells tape.c), and drives the LEDs, so
 * the LEDs keep blinking while the main loop is blocked on the network.
 */
#ifndef VKC_PANEL_H
#define VKC_PANEL_H

#include <stdbool.h>
#include <stdint.h>

/* Button press events, returned by panel_buttons(). */
#define PANEL_BTN_PLAY   (1u << 0)
#define PANEL_BTN_REC    (1u << 1)
#define PANEL_BTN_MOTOR  (1u << 2)

typedef enum { PANEL_LED_PLAY, PANEL_LED_REC, PANEL_LED_COUNT } panel_led_t;
typedef enum { LED_OFF, LED_ON, LED_BLINK } led_mode_t;

/* How the REMOTE input is read (g_cfg.remote). */
typedef enum {
    REMOTE_OFF = 0, /* ignored: the motor always runs */
    REMOTE_ON = 1,  /* contacts closed (pin pulled low) = motor on */
    REMOTE_INV = 2, /* pin high = motor on */
} remote_mode_t;

/* Manual override, set by the MOTOR button or the 'motor' command. */
typedef enum { MOTOR_AUTO, MOTOR_FORCE_ON, MOTOR_FORCE_OFF } motor_override_t;

void panel_init(remote_mode_t mode);
void panel_set_remote(remote_mode_t mode);

uint32_t panel_buttons(void); /* PANEL_BTN_* pressed since the last call */

void panel_led(panel_led_t which, led_mode_t mode);
void panel_led_error(panel_led_t which); /* fast flash for 1 s, then back to its mode */

void panel_set_override(motor_override_t o);
motor_override_t panel_override(void);
bool panel_remote_asserted(void); /* REMOTE input says "motor on" (debounced) */
bool panel_motor(void);           /* motor running after remote mode and override */

#endif
