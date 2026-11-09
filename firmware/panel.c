/*
 * panel.c - buttons, LEDs and the motor-control input.  See panel.h.
 */
#include "panel.h"

#include "board.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "tape.h"

#define TICK_MS      5
#define DEBOUNCE     4                  /* ticks a new level must hold: 20 ms */
#define BLINK_TICKS  (250 / TICK_MS)    /* 2 Hz blink */
#define FLASH_TICKS  (50 / TICK_MS)     /* 10 Hz error flash */
#define ERROR_TICKS  (1000 / TICK_MS)

typedef struct {
    uint pin;
    bool level;    /* debounced: true = active */
    uint8_t count; /* ticks the raw level has differed from 'level' */
} debounce_t;

static debounce_t btn[3] = {
    { .pin = VKC_PIN_BTN_PLAY },
    { .pin = VKC_PIN_BTN_REC },
    { .pin = VKC_PIN_BTN_MOTOR },
};
static debounce_t remote_in = { .pin = VKC_PIN_MOTOR_IN };

static const uint led_pin[PANEL_LED_COUNT] = { VKC_PIN_LED_PLAY, VKC_PIN_LED_REC };
static volatile led_mode_t led_mode[PANEL_LED_COUNT];
static volatile uint16_t led_err[PANEL_LED_COUNT];

static volatile uint32_t events;
static volatile remote_mode_t remote_mode;
static volatile motor_override_t override;
static volatile bool motor_on = true;
static uint32_t ticks;
static repeating_timer_t timer;

/* Returns true on a debounced transition to active. */
static bool debounce(debounce_t *d, bool raw)
{
    if (raw == d->level) {
        d->count = 0;
        return false;
    }
    if (++d->count < DEBOUNCE)
        return false;
    d->count = 0;
    d->level = raw;
    return raw;
}

static bool tick(repeating_timer_t *t)
{
    (void)t;
    ticks++;

    for (unsigned i = 0; i < 3; i++)
        if (debounce(&btn[i], !gpio_get(btn[i].pin))) /* buttons pull to GND */
            events |= 1u << i;

    bool pin = gpio_get(VKC_PIN_MOTOR_IN);
    debounce(&remote_in, remote_mode == REMOTE_INV ? pin : !pin);

    bool run;
    switch (override) {
    case MOTOR_FORCE_ON:  run = true; break;
    case MOTOR_FORCE_OFF: run = false; break;
    default:              run = remote_mode == REMOTE_OFF || remote_in.level; break;
    }
    motor_on = run;
    tape_motor(run);

    for (unsigned i = 0; i < PANEL_LED_COUNT; i++) {
        bool on;
        if (led_err[i]) {
            led_err[i]--;
            on = (ticks / FLASH_TICKS) & 1u;
        } else {
            led_mode_t m = led_mode[i];
            on = m == LED_ON || (m == LED_BLINK && ((ticks / BLINK_TICKS) & 1u));
        }
        gpio_put(led_pin[i], on);
    }
    /* MOTOR LED: lit while something is actually controlling the motor and
     * it is running.  With REMOTE off and no override it would always be
     * on, which says nothing, so it stays dark then. */
    gpio_put(VKC_PIN_LED_MOTOR,
             run && !(remote_mode == REMOTE_OFF && override == MOTOR_AUTO));
    return true;
}

void panel_init(remote_mode_t mode)
{
    remote_mode = mode;
    for (unsigned i = 0; i < 3; i++) {
        gpio_init(btn[i].pin);
        gpio_set_dir(btn[i].pin, GPIO_IN);
        gpio_pull_up(btn[i].pin);
    }
    gpio_init(VKC_PIN_MOTOR_IN);
    gpio_set_dir(VKC_PIN_MOTOR_IN, GPIO_IN);
    gpio_pull_up(VKC_PIN_MOTOR_IN); /* open contacts read high = motor off */
    gpio_set_input_hysteresis_enabled(VKC_PIN_MOTOR_IN, true);

    const uint leds[] = { VKC_PIN_LED_PLAY, VKC_PIN_LED_REC, VKC_PIN_LED_MOTOR };
    for (unsigned i = 0; i < 3; i++) {
        gpio_init(leds[i]);
        gpio_set_dir(leds[i], GPIO_OUT);
        gpio_put(leds[i], 0);
    }
    add_repeating_timer_ms(-TICK_MS, tick, NULL, &timer);
}

void panel_set_remote(remote_mode_t mode)
{
    remote_mode = mode;
}

uint32_t panel_buttons(void)
{
    uint32_t irq = save_and_disable_interrupts();
    uint32_t e = events;
    events = 0;
    restore_interrupts(irq);
    return e;
}

void panel_led(panel_led_t which, led_mode_t mode)
{
    led_mode[which] = mode;
}

void panel_led_error(panel_led_t which)
{
    led_err[which] = ERROR_TICKS;
}

void panel_set_override(motor_override_t o)
{
    override = o;
}

motor_override_t panel_override(void)
{
    return override;
}

bool panel_remote_asserted(void)
{
    return remote_in.level;
}

bool panel_motor(void)
{
    return motor_on;
}
