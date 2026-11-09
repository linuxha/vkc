/*
 * kcs.c - Kansas City Standard (KCS / CUTS) encoder and decoder.
 * See kcs.h for a description of the format.
 */
#include "kcs.h"

#define KCS_NONE UINT64_MAX

/* Half-cycle classification limits (ns). */
#define HALF_MIN_NS   (KCS_HALF1_NS * 2u / 5u)            /* shorter: glitch     */
/* tape speed tracking range: +/-12% */
#define HALF1_MIN_EST (KCS_HALF1_NS * 88u / 100u)
#define HALF1_MAX_EST (KCS_HALF1_NS * 112u / 100u)
#define HALF_MAX_NS   (KCS_HALF0_NS * 7u / 4u)             /* longer: no carrier  */

enum { ST_LEADER, ST_DATA, ST_TRAILER, ST_DONE };

bool kcs_params_valid(const kcs_params_t *p)
{
    return p && (p->baud == 300 || p->baud == 600 || p->baud == 1200) &&
           (p->stop_bits == 1 || p->stop_bits == 2);
}

unsigned kcs_halves_per_bit(const kcs_params_t *p, int bit)
{
    return 2u * (bit ? KCS_F1_HZ : KCS_F0_HZ) / p->baud;
}

/* ------------------------------------------------------------------ */
/* Encoder                                                             */
/* ------------------------------------------------------------------ */

void kcs_enc_init(kcs_enc_t *e, const kcs_params_t *p, const uint8_t *data,
                  size_t len, uint32_t leader_ms, uint32_t trailer_ms)
{
    e->p = *p;
    e->data = data;
    e->len = len;
    e->pos = 0;
    e->bits_left = (uint32_t)((uint64_t)leader_ms * p->baud / 1000u);
    e->trailer_bits = (uint32_t)((uint64_t)trailer_ms * p->baud / 1000u);
    e->frame = 0;
    e->frame_bits = 0;
    e->stage = ST_LEADER;
    e->tone = 1;
    e->halves_left = 0;
}

static int enc_next_bit(kcs_enc_t *e)
{
    for (;;) {
        switch (e->stage) {
        case ST_LEADER:
            if (e->bits_left) {
                e->bits_left--;
                return 1;
            }
            e->stage = ST_DATA;
            break;
        case ST_DATA:
            if (e->frame_bits) {
                int b = e->frame & 1u;
                e->frame >>= 1;
                e->frame_bits--;
                return b;
            }
            if (e->pos < e->len) {
                uint16_t stop = (uint16_t)((1u << e->p.stop_bits) - 1u);
                /* bit 0: start (0), bits 1-8: data LSB first, then stop bits */
                e->frame = (uint16_t)(((uint16_t)e->data[e->pos++] << 1) | (stop << 9));
                e->frame_bits = (uint8_t)(9u + e->p.stop_bits);
                break;
            }
            e->stage = ST_TRAILER;
            e->bits_left = e->trailer_bits;
            break;
        case ST_TRAILER:
            if (e->bits_left) {
                e->bits_left--;
                return 1;
            }
            e->stage = ST_DONE;
            break;
        default:
            return -1;
        }
    }
}

int kcs_enc_next(kcs_enc_t *e)
{
    if (e->halves_left == 0) {
        int bit = enc_next_bit(e);
        if (bit < 0)
            return -1;
        e->tone = (uint8_t)bit;
        e->halves_left = (uint8_t)kcs_halves_per_bit(&e->p, bit);
    }
    e->halves_left--;
    return e->tone;
}

/* ------------------------------------------------------------------ */
/* Decoder                                                             */
/*                                                                     */
/* Each half-cycle is classified as mark or space by its length, which  */
/* gives a tone-vs-time signal.  That signal is then treated like a     */
/* UART line: after at least half a bit of mark, the first space        */
/* half-cycle is the leading edge of a start bit.  Rather than sampling */
/* one instant, each bit is decided by majority over its middle half,   */
/* so a single misread half-cycle (noise) cannot flip a bit.  Resync    */
/* happens on every start bit; speed error only matters within one     */
/* byte, and mark/space transitions inside a byte also nudge the       */
/* windows, which tracks tape speed error and wow/flutter.              */
/* ------------------------------------------------------------------ */

static void dec_half(kcs_dec_t *d, uint64_t ns);

void kcs_dec_init(kcs_dec_t *d, const kcs_params_t *p, kcs_byte_cb cb, void *ctx)
{
    d->p = *p;
    d->bit_ns = 1000000000u / p->baud;
    d->bit_cur = d->bit_ns;
    d->half1_est = KCS_HALF1_NS;
    d->t = 0;
    d->mark_start = d->space_start = d->last_edge = KCS_NONE;
    d->mark_len = 0;
    d->last_tone = 1;
    d->win_start = d->win_end = 0;
    d->acc_mark = d->acc_space = 0;
    d->pending_ns = 0;
    d->held_ns = 0;
    d->state = -1;
    d->shreg = 0;
    d->carrier = false;
    d->cb = cb;
    d->ctx = ctx;
    d->bytes = d->framing_errors = d->glitches = d->dropouts = 0;
}

void kcs_dec_gap(kcs_dec_t *d)
{
    if (d->held_ns) {
        uint64_t h = d->held_ns;
        d->held_ns = 0;
        dec_half(d, h);
    }
    d->state = -1;
    d->mark_start = d->space_start = KCS_NONE;
    d->pending_ns = 0;
    d->carrier = false;
}

static void dec_bit(kcs_dec_t *d, int v)
{
    int idx = d->state;

    if (idx == 0) { /* start bit */
        d->state = v == 0 ? 1 : -1;
    } else if (idx <= 8) {
        d->shreg |= (uint16_t)(v << (idx - 1));
        d->state++;
    } else { /* first stop bit */
        if (v) {
            d->bytes++;
            if (d->cb)
                d->cb(d->ctx, (uint8_t)d->shreg);
        } else {
            d->framing_errors++;
        }
        d->state = -1;
    }
}

/* Accumulate [ta, tb) of the given tone into the bit voting windows. */
static void dec_vote(kcs_dec_t *d, uint64_t ta, uint64_t tb, int tone)
{
    while (d->state >= 0 && tb > d->win_start) {
        uint64_t lo = ta > d->win_start ? ta : d->win_start;
        uint64_t hi = tb < d->win_end ? tb : d->win_end;
        if (hi > lo) {
            if (tone)
                d->acc_mark += (uint32_t)(hi - lo);
            else
                d->acc_space += (uint32_t)(hi - lo);
        }
        if (tb < d->win_end)
            break;
        int bit = d->acc_mark > d->acc_space;
        d->acc_mark = d->acc_space = 0;
        d->win_start += d->bit_cur;
        d->win_end += d->bit_cur;
        dec_bit(d, bit);
    }
}

static void dec_half(kcs_dec_t *d, uint64_t ns)
{
    uint64_t ta = d->t, tb = d->t + ns;
    d->t = tb;

    if (ns > HALF_MAX_NS) {
        if (d->carrier)
            d->dropouts++;
        d->state = -1;
        d->mark_start = d->space_start = KCS_NONE;
        d->carrier = false;
        return;
    }
    d->carrier = true;

    /* classify against the measured tape speed, then refine the estimate */
    int tone = ns * 2u < (uint64_t)d->half1_est * 3u ? 1 : 0;
    {
        int64_t m = tone ? (int64_t)ns : (int64_t)ns / 2;
        int64_t est = d->half1_est + (m - (int64_t)d->half1_est) / 32;
        if (est < HALF1_MIN_EST)
            est = HALF1_MIN_EST;
        if (est > HALF1_MAX_EST)
            est = HALF1_MAX_EST;
        d->half1_est = (uint32_t)est;
    }

    /* track runs of mark and space */
    if (tone) {
        if (d->mark_start == KCS_NONE)
            d->mark_start = ta;
        d->space_start = KCS_NONE;
    } else if (d->space_start == KCS_NONE) {
        d->space_start = ta;
        d->mark_len = d->mark_start == KCS_NONE ? 0 : ta - d->mark_start;
        d->mark_start = KCS_NONE;
    }

    /*
     * Timing recovery inside a byte: a mark/space transition is a bit
     * boundary, so pull the voting windows half way towards it.  This
     * follows tape speed error and wow/flutter between start bits.
     */
    if (d->state > 0 && tone != d->last_tone) {
        int64_t bstart = (int64_t)(d->win_start - d->bit_cur / 4u);
        int64_t e = (int64_t)ta - bstart;
        if (e > (int64_t)d->bit_cur / 2)
            e -= d->bit_cur;
        if (e > -(int64_t)d->bit_cur / 4 && e < (int64_t)d->bit_cur / 4) {
            d->win_start += e / 2;
            d->win_end += e / 2;
        }
    }
    d->last_tone = (int8_t)tone;

    dec_vote(d, ta, tb, tone);

    /*
     * Hunting for a start bit: the leading edge is where the current space
     * run began, provided at least half a bit of mark preceded it.  The edge
     * may lie in the past (inside the previous byte's stop-bit window, when
     * the tape runs fast), so the run so far is replayed into the window.
     */
    if (d->state < 0 && tone == 0 && d->mark_len >= d->bit_cur / 2u &&
        (d->last_edge == KCS_NONE || d->space_start > d->last_edge)) {
        uint64_t edge = d->space_start;
        d->last_edge = edge;
        d->bit_cur = (uint32_t)((uint64_t)d->bit_ns * d->half1_est / KCS_HALF1_NS);
        d->state = 0;
        d->shreg = 0;
        d->win_start = edge + d->bit_cur / 4u;
        d->win_end = edge + d->bit_cur * 3u / 4u;
        d->acc_mark = d->acc_space = 0;
        dec_vote(d, edge, tb, 0);
    }
}

/* How far a half-cycle is from the nearest valid length, in per-mille. */
static uint32_t half_dev(uint64_t x)
{
    uint64_t d1 = (x > KCS_HALF1_NS ? x - KCS_HALF1_NS : KCS_HALF1_NS - x) * 1000u / KCS_HALF1_NS;
    uint64_t d0 = (x > KCS_HALF0_NS ? x - KCS_HALF0_NS : KCS_HALF0_NS - x) * 1000u / KCS_HALF0_NS;
    uint64_t d = d1 < d0 ? d1 : d0;
    return d > 100000u ? 100000u : (uint32_t)d;
}

static uint32_t max_u32(uint32_t a, uint32_t b) { return a > b ? a : b; }

/*
 * Glitch filter.  A noise spike adds two extra zero crossings, so instead
 * of one half-cycle the decoder sees  [A] [tiny] [B]  (the spike may be
 * several tiny pieces, which are summed).  Depending on where the spike
 * landed, the right regrouping is
 *     A | tiny+B      chatter just after a real crossing
 *     A+tiny | B      chatter just before a real crossing
 *     A+tiny+B        an impulse inside one half-cycle
 * The grouping whose pieces best match valid half-cycle lengths wins; the
 * merge-all option is slightly penalised, because two whole half-cycles of
 * mark joined together would look like one of space.  This needs one
 * half-cycle (A) of look-ahead.  A spike exactly in the middle of a space
 * half-cycle is indistinguishable from two mark half-cycles; the bit
 * voting window absorbs that.
 */
void kcs_dec_half(kcs_dec_t *d, uint32_t half_ns)
{
    if (half_ns < HALF_MIN_NS) {
        d->pending_ns += half_ns;
        d->glitches++;
        return;
    }
    uint64_t b = half_ns;
    if (d->pending_ns) {
        uint64_t a = d->held_ns, t = d->pending_ns;
        d->pending_ns = 0;
        if (a) {
            uint32_t s1 = max_u32(half_dev(a), half_dev(t + b));
            uint32_t s2 = max_u32(half_dev(a + t), half_dev(b));
            uint32_t s3 = half_dev(a + t + b) + 150u;
            if (s3 < s1 && s3 < s2) {
                d->held_ns = a + t + b;
                return;
            }
            if (s2 < s1) {
                d->held_ns = a + t;
            } else {
                b += t;
            }
        } else {
            b += t;
        }
    }
    if (d->held_ns)
        dec_half(d, d->held_ns);
    d->held_ns = b;
}
