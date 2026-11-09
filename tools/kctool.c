/*
 * kctool - create, decode and test Kansas City Standard tapes.
 *
 *   kctool encode [opts] IN.bin OUT.wav    bytes -> KCS audio
 *   kctool decode [opts] IN.wav OUT.bin    KCS audio -> bytes
 *   kctool serve  [-p port] [-d dir]       reference VKC/1 tape server
 *   kctool dir    HOST[:PORT]              list tapes on a server
 *   kctool get    HOST[:PORT] NAME [OUT]   download a tape
 *   kctool put    HOST[:PORT] NAME IN      upload a tape
 *
 * "-" may be used for stdin/stdout, so it works with aplay/arecord:
 *   kctool encode prog.bin - | aplay
 *   arecord -f S16_LE -r 44100 -c 1 | kctool decode - prog.bin
 */
#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kcs.h"
#include "kctool.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int verbose;

void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("kctool: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

void info(const char *fmt, ...)
{
    va_list ap;
    if (!verbose)
        return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static FILE *open_in(const char *path)
{
    if (!strcmp(path, "-"))
        return stdin;
    FILE *f = fopen(path, "rb");
    if (!f)
        die("%s: %s", path, strerror(errno));
    return f;
}

static FILE *open_out(const char *path)
{
    if (!strcmp(path, "-"))
        return stdout;
    FILE *f = fopen(path, "wb");
    if (!f)
        die("%s: %s", path, strerror(errno));
    return f;
}

static void close_out(FILE *f, const char *path)
{
    if (fflush(f) || ferror(f) || (f != stdout && fclose(f)))
        die("%s: write error", path);
}

uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = open_in(path);
    size_t cap = 65536, n = 0;
    uint8_t *buf = malloc(cap);
    size_t r;
    while (buf && (r = fread(buf + n, 1, cap - n, f)) > 0) {
        n += r;
        if (n == cap)
            buf = realloc(buf, cap *= 2);
    }
    if (!buf)
        die("out of memory");
    if (ferror(f))
        die("%s: read error", path);
    if (f != stdin)
        fclose(f);
    *len = n;
    return buf;
}

void write_file(const char *path, const uint8_t *buf, size_t len)
{
    FILE *f = open_out(path);
    if (len && fwrite(buf, 1, len, f) != len)
        die("%s: write error", path);
    close_out(f, path);
}

static long parse_num(const char *s, const char *what, long lo, long hi)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || *end || v < lo || v > hi)
        die("bad %s '%s' (expected %ld..%ld)", what, s, lo, hi);
    return v;
}

/* ------------------------------------------------------------------ */
/* WAV output                                                          */
/* ------------------------------------------------------------------ */

static void put_le(uint8_t *p, uint32_t v, int n)
{
    for (int i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void wav_header(FILE *f, uint32_t rate, uint32_t nsamples)
{
    uint8_t h[44];
    uint32_t data = nsamples * 2u;
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36u + data, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);       /* fmt chunk size */
    put_le(h + 20, 1, 2);        /* PCM */
    put_le(h + 22, 1, 2);        /* mono */
    put_le(h + 24, rate, 4);
    put_le(h + 28, rate * 2u, 4);
    put_le(h + 32, 2, 2);        /* block align */
    put_le(h + 34, 16, 2);       /* bits */
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data, 4);
    fwrite(h, 1, sizeof h, f);
}

typedef struct {
    uint32_t rate;
    double amp;      /* 0..1 of full scale */
    int square;
    uint32_t silence_samples;
} render_opts_t;

/*
 * Render the encoder output.  With f == NULL only counts samples, so the
 * WAV header can be written before the data even when streaming to stdout.
 */
static uint32_t render(FILE *f, const kcs_params_t *p, const uint8_t *data, size_t len,
                       uint32_t leader_ms, uint32_t trailer_ms, const render_opts_t *o)
{
    kcs_enc_t e;
    int16_t buf[4096];
    size_t nb = 0;
    uint64_t n = 0;     /* next output sample index */
    double t = 0;       /* start of current half-cycle, in samples */
    int pol = 1, tone;

#define EMIT(v)                                                   \
    do {                                                          \
        if (f) {                                                  \
            buf[nb++] = (int16_t)(v);                             \
            if (nb == sizeof buf / sizeof buf[0]) {               \
                fwrite(buf, 2, nb, f);                            \
                nb = 0;                                           \
            }                                                     \
        }                                                         \
        n++;                                                      \
    } while (0)

    for (uint32_t i = 0; i < o->silence_samples; i++)
        EMIT(0);
    t = (double)n;

    kcs_enc_init(&e, p, data, len, leader_ms, trailer_ms);
    while ((tone = kcs_enc_next(&e)) >= 0) {
        double dur = (double)o->rate / (2.0 * (tone ? KCS_F1_HZ : KCS_F0_HZ));
        double end = t + dur;
        while ((double)n < end) {
            double v = o->square ? 1.0 : sin(M_PI * ((double)n - t) / dur);
            EMIT(lrint(pol * v * o->amp * 32767.0));
        }
        t = end;
        pol = -pol;
    }
    for (uint32_t i = 0; i < o->silence_samples; i++)
        EMIT(0);
#undef EMIT
    if (f && nb)
        fwrite(buf, 2, nb, f);
    return (uint32_t)n;
}

static void usage_encode(void)
{
    fputs("usage: kctool encode [-b baud] [-s stopbits] [-r rate] [-l leader_ms]\n"
          "                     [-t trailer_ms] [-a amplitude%] [-q] IN OUT.wav\n"
          "  -b  300 (default), 600 or 1200\n"
          "  -s  stop bits, 1 or 2 (default 2)\n"
          "  -r  sample rate (default 44100)\n"
          "  -l  leader tone in ms (default 5000)\n"
          "  -t  trailer tone in ms (default 1000)\n"
          "  -a  amplitude, percent of full scale (default 70)\n"
          "  -q  square wave instead of sine\n",
          stderr);
    exit(2);
}

static int cmd_encode(int argc, char **argv)
{
    kcs_params_t p = KCS_PARAMS_DEFAULT;
    render_opts_t o = { .rate = 44100, .amp = 0.7, .square = 0 };
    uint32_t leader = 5000, trailer = 1000;
    int c;

    while ((c = getopt(argc, argv, "b:s:r:l:t:a:qv")) != -1) {
        switch (c) {
        case 'b': p.baud = (uint16_t)parse_num(optarg, "baud", 1, 65535); break;
        case 's': p.stop_bits = (uint8_t)parse_num(optarg, "stop bits", 1, 2); break;
        case 'r': o.rate = (uint32_t)parse_num(optarg, "rate", 8000, 192000); break;
        case 'l': leader = (uint32_t)parse_num(optarg, "leader", 0, 600000); break;
        case 't': trailer = (uint32_t)parse_num(optarg, "trailer", 0, 600000); break;
        case 'a': o.amp = parse_num(optarg, "amplitude", 1, 100) / 100.0; break;
        case 'q': o.square = 1; break;
        case 'v': verbose++; break;
        default: usage_encode();
        }
    }
    if (argc - optind != 2)
        usage_encode();
    if (!kcs_params_valid(&p))
        die("baud must be 300, 600 or 1200");

    size_t len;
    uint8_t *data = read_file(argv[optind], &len);
    o.silence_samples = o.rate / 2;

    uint32_t ns = render(NULL, &p, data, len, leader, trailer, &o);
    if ((uint64_t)ns * 2u + 36u > UINT32_MAX)
        die("output too long for a WAV file");

    FILE *f = open_out(argv[optind + 1]);
    wav_header(f, o.rate, ns);
    render(f, &p, data, len, leader, trailer, &o);
    close_out(f, argv[optind + 1]);
    info("encoded %zu bytes at %u baud: %u samples, %.1f s",
         len, p.baud, ns, (double)ns / o.rate);
    free(data);
    return 0;
}

/* ------------------------------------------------------------------ */
/* WAV input                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    FILE *f;
    uint32_t rate;
    uint16_t channels, bits;
    uint64_t remaining; /* bytes left in data chunk; UINT64_MAX = until EOF */
} wav_in_t;

static uint32_t get_le(const uint8_t *p, int n)
{
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void skip_bytes(FILE *f, uint32_t n)
{
    while (n--)
        if (fgetc(f) == EOF)
            die("truncated WAV file");
}

static void wav_open(wav_in_t *w, const char *path)
{
    uint8_t h[12], ch[8];
    int have_fmt = 0;

    w->f = open_in(path);
    if (fread(h, 1, 12, w->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
        die("%s: not a WAV file", path);
    for (;;) {
        if (fread(ch, 1, 8, w->f) != 8)
            die("%s: no data chunk", path);
        uint32_t sz = get_le(ch + 4, 4);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            if (sz < 16 || fread(fmt, 1, 16, w->f) != 16)
                die("%s: bad fmt chunk", path);
            uint16_t tag = (uint16_t)get_le(fmt, 2);
            w->channels = (uint16_t)get_le(fmt + 2, 2);
            w->rate = get_le(fmt + 4, 4);
            w->bits = (uint16_t)get_le(fmt + 14, 2);
            if ((tag != 1 && tag != 0xFFFE) || (w->bits != 8 && w->bits != 16) ||
                !w->channels || w->rate < 4000)
                die("%s: only 8/16-bit PCM WAV is supported", path);
            skip_bytes(w->f, sz - 16 + (sz & 1));
            have_fmt = 1;
        } else if (!memcmp(ch, "data", 4)) {
            if (!have_fmt)
                die("%s: data before fmt chunk", path);
            /* arecord writes 0 or 0xFFFFFFFF when streaming */
            w->remaining = (sz == 0 || sz == 0xFFFFFFFFu) ? UINT64_MAX : sz;
            return;
        } else {
            skip_bytes(w->f, sz + (sz & 1));
        }
    }
}

/* Next sample of channel 0, scaled to -1..1.  Returns 0 at end. */
static int wav_sample(wav_in_t *w, double *out)
{
    uint8_t fr[64];
    size_t fsz = (size_t)w->channels * (w->bits / 8u);
    if (fsz > sizeof fr)
        die("too many channels");
    if (w->remaining != UINT64_MAX) {
        if (w->remaining < fsz)
            return 0;
        w->remaining -= fsz;
    }
    if (fread(fr, 1, fsz, w->f) != fsz)
        return 0;
    if (w->bits == 8)
        *out = ((int)fr[0] - 128) / 128.0;
    else
        *out = (int16_t)get_le(fr, 2) / 32768.0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Decode                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
} outbuf_t;

static void collect(void *ctx, uint8_t b)
{
    outbuf_t *o = ctx;
    if (o->len == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 4096;
        o->buf = realloc(o->buf, o->cap);
        if (!o->buf)
            die("out of memory");
    }
    o->buf[o->len++] = b;
}

static void usage_decode(void)
{
    fputs("usage: kctool decode [-b baud|auto] [-s stopbits] [-v] IN.wav OUT\n"
          "  -b  300 (default), 600, 1200 or auto\n"
          "  -s  stop bits, 1 or 2 (default 2; only the first is checked)\n"
          "  -v  print statistics\n",
          stderr);
    exit(2);
}

static int cmd_decode(int argc, char **argv)
{
    static const uint16_t bauds[] = { 300, 600, 1200 };
    enum { NB = sizeof bauds / sizeof bauds[0] };
    kcs_params_t p = KCS_PARAMS_DEFAULT;
    int autobaud = 0, c;

    while ((c = getopt(argc, argv, "b:s:v")) != -1) {
        switch (c) {
        case 'b':
            if (!strcmp(optarg, "auto"))
                autobaud = 1;
            else
                p.baud = (uint16_t)parse_num(optarg, "baud", 1, 65535);
            break;
        case 's': p.stop_bits = (uint8_t)parse_num(optarg, "stop bits", 1, 2); break;
        case 'v': verbose++; break;
        default: usage_decode();
        }
    }
    if (argc - optind != 2)
        usage_decode();
    if (!kcs_params_valid(&p))
        die("baud must be 300, 600, 1200 or auto");

    kcs_dec_t dec[NB];
    outbuf_t out[NB] = { 0 };
    int ndec = autobaud ? NB : 1;
    for (int i = 0; i < ndec; i++) {
        kcs_params_t q = p;
        if (autobaud)
            q.baud = bauds[i];
        kcs_dec_init(&dec[i], &q, collect, &out[i]);
    }

    wav_in_t w;
    wav_open(&w, argv[optind]);
    info("input: %u Hz, %u-bit, %u channel(s)", w.rate, w.bits, w.channels);

    /*
     * Zero-crossing detector: DC-blocking high-pass, peak envelope follower,
     * and hysteresis at 20% of the envelope, so noise near zero and
     * low-level hiss during silence produce no crossings.
     */
    const double hp_a = 0.995;
    const double env_decay = exp(-1.0 / (w.rate * 0.05));
    const double floor_th = 0.02;
    const double ns_per_sample = 1e9 / w.rate;
    double x, xprev = 0, y = 0, yprev = 0, env = 0, last_cross = -1;
    int state = 0; /* +1 high, -1 low, 0 unknown */
    uint64_t n = 0;

    while (wav_sample(&w, &x)) {
        y = x - xprev + hp_a * y;
        xprev = x;
        double a = fabs(y);
        env = a > env ? a : env * env_decay;
        double th = env * 0.2;
        if (th < floor_th)
            th = floor_th;

        int flip = 0;
        double level = 0;
        if (state >= 0 && y < -th) {
            flip = state > 0;
            state = -1;
            level = -th;
        } else if (state <= 0 && y > th) {
            flip = state < 0;
            state = 1;
            level = th;
        }
        if (flip) {
            /* interpolate the threshold crossing between samples */
            double frac = (yprev - level) / (yprev - y);
            double tc = (double)n - 1.0 + frac;
            if (last_cross >= 0) {
                double half = (tc - last_cross) * ns_per_sample;
                if (half > 4e9)
                    half = 4e9;
                for (int i = 0; i < ndec; i++)
                    kcs_dec_half(&dec[i], (uint32_t)half);
            }
            last_cross = tc;
        }
        yprev = y;
        n++;
    }
    if (w.f != stdin)
        fclose(w.f);
    for (int i = 0; i < ndec; i++)
        kcs_dec_gap(&dec[i]);

    int best = 0;
    if (autobaud) {
        long best_score = 0;
        for (int i = 0; i < ndec; i++) {
            long score = (long)dec[i].bytes - 4L * (long)dec[i].framing_errors;
            info("  %4u baud: %u bytes, %u framing errors", bauds[i],
                 dec[i].bytes, dec[i].framing_errors);
            if (i == 0 || score > best_score) {
                best = i;
                best_score = score;
            }
        }
        info("auto: selected %u baud", bauds[best]);
    }
    kcs_dec_t *d = &dec[best];
    info("decoded %u bytes from %.1f s of audio at %u baud", d->bytes,
         (double)n / w.rate, d->p.baud);
    info("framing errors %u, glitches %u, dropouts %u", d->framing_errors,
         d->glitches, d->dropouts);
    if (d->framing_errors && !verbose)
        fprintf(stderr, "kctool: warning: %u framing errors\n", d->framing_errors);

    write_file(argv[optind + 1], out[best].buf, out[best].len);
    for (int i = 0; i < ndec; i++)
        free(out[i].buf);
    return d->bytes ? 0 : 1;
}

/* ------------------------------------------------------------------ */

static void usage(void)
{
    fputs("usage: kctool <command> [args]\n"
          "  encode [opts] IN OUT.wav        bytes -> Kansas City audio\n"
          "  decode [opts] IN.wav OUT        Kansas City audio -> bytes\n"
          "  serve  [-p port] [-d dir]       run a reference VKC/1 tape server\n"
          "  dir    HOST[:PORT]              list tapes on a server\n"
          "  get    HOST[:PORT] NAME [OUT]   download a tape (OUT defaults to NAME)\n"
          "  put    HOST[:PORT] NAME IN      upload a tape\n"
          "Use '-' for stdin/stdout.  Run 'kctool <command> -h' for options.\n",
          stderr);
    exit(2);
}

int main(int argc, char **argv)
{
    if (argc < 2)
        usage();
    const char *cmd = argv[1];
    argc--;
    argv++;
    if (!strcmp(cmd, "encode"))
        return cmd_encode(argc, argv);
    if (!strcmp(cmd, "decode"))
        return cmd_decode(argc, argv);
    if (!strcmp(cmd, "serve"))
        return cmd_serve(argc, argv);
    if (!strcmp(cmd, "dir") || !strcmp(cmd, "get") || !strcmp(cmd, "put"))
        return cmd_client(cmd, argc, argv);
    usage();
}
