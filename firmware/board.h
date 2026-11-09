/*
 * board.h - pin assignments and build-time limits for the VKC Pico W.
 * Override any of these with -D on the cmake command line if needed.
 */
#ifndef VKC_BOARD_H
#define VKC_BOARD_H

/* Command serial port (to the vintage computer's serial port / terminal). */
#ifndef VKC_CMD_UART
#define VKC_CMD_UART      uart0
#endif
#ifndef VKC_CMD_UART_TX
#define VKC_CMD_UART_TX   0   /* GP0, pin 1 */
#endif
#ifndef VKC_CMD_UART_RX
#define VKC_CMD_UART_RX   1   /* GP1, pin 2 */
#endif

/* Cassette interface.  See docs/hardware.md for the analog front ends. */
#ifndef VKC_PIN_TAPE_IN
#define VKC_PIN_TAPE_IN   2   /* GP2, pin 4: from comparator (computer's tape OUT) */
#endif
#ifndef VKC_PIN_TAPE_OUT
#define VKC_PIN_TAPE_OUT  3   /* GP3, pin 5: to RC filter (computer's tape IN)     */
#endif

/* Cassette motor control: the computer's REMOTE relay contacts (or an
 * opto-isolator) between this pin and GND.  Internal pull-up. */
#ifndef VKC_PIN_MOTOR_IN
#define VKC_PIN_MOTOR_IN  4   /* GP4, pin 6 */
#endif

/* Front-panel push buttons, each to GND.  Internal pull-ups. */
#ifndef VKC_PIN_BTN_PLAY
#define VKC_PIN_BTN_PLAY  5   /* GP5, pin 7 */
#endif
#ifndef VKC_PIN_BTN_REC
#define VKC_PIN_BTN_REC   6   /* GP6, pin 9 */
#endif
#ifndef VKC_PIN_BTN_MOTOR
#define VKC_PIN_BTN_MOTOR 7   /* GP7, pin 10 */
#endif

/* Front-panel LEDs, active high, each through a 330R to GND. */
#ifndef VKC_PIN_LED_PLAY
#define VKC_PIN_LED_PLAY  8   /* GP8, pin 11 */
#endif
#ifndef VKC_PIN_LED_REC
#define VKC_PIN_LED_REC   9   /* GP9, pin 12 */
#endif
#ifndef VKC_PIN_LED_MOTOR
#define VKC_PIN_LED_MOTOR 10  /* GP10, pin 14 */
#endif

/* Largest tape held in RAM (~70 minutes at 300 baud). */
#ifndef VKC_TAPE_MAX
#define VKC_TAPE_MAX      (128u * 1024u)
#endif

#endif
