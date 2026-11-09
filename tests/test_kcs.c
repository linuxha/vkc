/*
 * test_kcs.c - round-trip tests for the shared KCS codec.
 *
 * Encodes random data to half-cycle tones, converts them to durations with
 * the impairments a real cassette/comparator path adds (timing jitter,
 * duty-cycle skew, speed error, noise spikes), and checks the decoder
 * recovers the data exactly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kcs.h"

typedef struct {
    uint8_t buf[4096];
    size_t len;
} sink_t;

static void sink(void *ctx, uint8_t b)
{
    sink_t *s = ctx;
    if (s->len < sizeof s->buf)
        s->buf[s->len++] = b;
}

static uint32_t rng = 12345;
static int rnd(int n) /* 0..n-1 */
{
    rng = rng * 1103515245u + 12345u;
    return (int)((rng >> 8) % (uint32_t)n);
}

typedef struct {
    const char *name;
    int jitter_pct;  /* +/- uniform jitter, percent of the half-cycle */
    int skew_pct;    /* alternate halves lengthened/shortened (comparator offset) */
    int speed_pct;   /* tape speed error, percent (positive = slow tape) */
    int spike_every; /* insert a noise spike every N half-cycles (0 = none) */
    int tol[3];      /* byte errors allowed per 1000 at 300 / 600 / 1200 baud */
} impair_t;

/* Byte errors between expected and decoded data: Levenshtein edit distance. */
static size_t count_errors(const uint8_t *want, size_t n, const uint8_t *got, size_t m)
{
    size_t *prev = malloc((m + 1) * sizeof *prev), *cur = malloc((m + 1) * sizeof *cur);
    for (size_t j = 0; j <= m; j++)
        prev[j] = j;
    for (size_t i = 1; i <= n; i++) {
        cur[0] = i;
        for (size_t j = 1; j <= m; j++) {
            size_t sub = prev[j - 1] + (want[i - 1] != got[j - 1]);
            size_t del = prev[j] + 1, ins = cur[j - 1] + 1;
            cur[j] = sub < del ? (sub < ins ? sub : ins) : (del < ins ? del : ins);
        }
        size_t *t = prev;
        prev = cur;
        cur = t;
    }
    size_t d = prev[m];
    free(prev);
    free(cur);
    return d;
}

static int run(const kcs_params_t *p, const impair_t *im, size_t n)
{
    uint8_t data[2048];
    sink_t out = { .len = 0 };
    kcs_enc_t e;
    kcs_dec_t d;
    int tone, i = 0;

    for (size_t k = 0; k < n; k++)
        data[k] = (uint8_t)rnd(256);
    data[0] = 0x00; data[1] = 0xFF; data[2] = 0x55; data[3] = 0xAA; /* edge cases */

    kcs_enc_init(&e, p, data, n, 500, 200);
    kcs_dec_init(&d, p, sink, &out);

    while ((tone = kcs_enc_next(&e)) >= 0) {
        double ns = 1e9 / (2.0 * (tone ? KCS_F1_HZ : KCS_F0_HZ));
        ns *= 1.0 + im->speed_pct / 100.0;
        ns *= 1.0 + ((i & 1) ? im->skew_pct : -im->skew_pct) / 100.0;
        if (im->jitter_pct)
            ns *= 1.0 + (rnd(2 * im->jitter_pct * 10 + 1) - im->jitter_pct * 10) / 1000.0;
        if (im->spike_every && i % im->spike_every == 7) {
            /* impulse noise: the half-cycle is split as  a, tiny, b */
            uint32_t tiny = 5000 + (uint32_t)rnd(30000);
            uint32_t a = (uint32_t)(ns * (20 + rnd(61)) / 100) - tiny / 2;
            kcs_dec_half(&d, a);
            kcs_dec_half(&d, tiny);
            kcs_dec_half(&d, (uint32_t)ns - a - tiny);
        } else if (im->spike_every && i % im->spike_every == 50) {
            /* comparator chatter right at a zero crossing */
            uint32_t tiny = 2000 + (uint32_t)rnd(8000);
            kcs_dec_half(&d, (uint32_t)ns - tiny);
            kcs_dec_half(&d, tiny);
        } else {
            kcs_dec_half(&d, (uint32_t)ns);
        }
        i++;
    }
    kcs_dec_gap(&d);

    size_t err = count_errors(data, n, out.buf, out.len);
    int bi = p->baud == 300 ? 0 : p->baud == 600 ? 1 : 2;
    size_t tol = n * (size_t)im->tol[bi] / 1000u;
    int ok = err <= tol;
    printf("%-5s %4u baud %u stop  %-22s %4zu/%-4zu bytes  %3zu errors (allowed %zu)  fe=%u gl=%u\n",
           ok ? "ok" : "FAIL", p->baud, p->stop_bits, im->name, out.len, n, err, tol,
           d.framing_errors, d.glitches);
    return ok;
}

/* Data after a dropout must still decode once carrier returns. */
static int test_dropout(void)
{
    kcs_params_t p = KCS_PARAMS_DEFAULT;
    const uint8_t a[] = "HELLO", b[] = "WORLD";
    sink_t out = { .len = 0 };
    kcs_dec_t d;
    kcs_enc_t e;
    int tone;

    kcs_dec_init(&d, &p, sink, &out);
    kcs_enc_init(&e, &p, a, 5, 300, 100);
    while ((tone = kcs_enc_next(&e)) >= 0)
        kcs_dec_half(&d, tone ? KCS_HALF1_NS : KCS_HALF0_NS);
    kcs_dec_half(&d, 2000000000u); /* two seconds of silence */
    kcs_enc_init(&e, &p, b, 5, 300, 100);
    while ((tone = kcs_enc_next(&e)) >= 0)
        kcs_dec_half(&d, tone ? KCS_HALF1_NS : KCS_HALF0_NS);
    kcs_dec_gap(&d);

    int ok = out.len == 10 && !memcmp(out.buf, "HELLOWORLD", 10) && d.dropouts == 1;
    printf("%-5s dropout/resync (%zu bytes, %u dropouts)\n", ok ? "ok" : "FAIL", out.len,
           d.dropouts);
    return ok;
}

int main(void)
{
    static const impair_t imps[] = {
        { "clean", 0, 0, 0, 0, { 0, 0, 0 } },
        { "jitter 10%", 10, 0, 0, 0, { 0, 0, 0 } },
        { "skew 15%", 0, 15, 0, 0, { 0, 0, 0 } },
        { "speed +10%", 0, 0, 10, 0, { 0, 0, 0 } },
        { "speed -10%", 0, 0, -10, 0, { 0, 0, 0 } },
        { "jitter+skew+speed 6%", 8, 10, 6, 0, { 0, 0, 0 } },
        { "jitter+skew+speed -6%", 8, 10, -6, 0, { 0, 0, 0 } },
        /*
         * Impulse noise (spikes that cross zero mid half-cycle).  A spike in
         * the middle of a space half-cycle is indistinguishable from two mark
         * half-cycles, and at 1200 baud a 0 bit is a single cycle, so some
         * errors there are physics, not bugs.  The budgets sit just above the
         * observed worst case over many seeds, so regressions still show up.
         * "heavy" is ~25 spikes/second, far worse than a sane input stage.
         */
        { "spikes, heavy", 0, 0, 0, 97, { 0, 30, 80 } },
        { "spikes, light", 0, 0, 0, 797, { 0, 5, 10 } },
        { "everything", 6, 10, 4, 397, { 10, 15, 30 } },
    };
    static const kcs_params_t ps[] = { { 300, 2 }, { 300, 1 }, { 600, 2 }, { 1200, 2 }, { 1200, 1 } };
    int fails = 0;

    for (size_t i = 0; i < sizeof ps / sizeof ps[0]; i++)
        for (size_t j = 0; j < sizeof imps / sizeof imps[0]; j++)
            fails += !run(&ps[i], &imps[j], 1024);
    fails += !test_dropout();

    printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
    return fails != 0;
}
