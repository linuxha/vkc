/*
 * kcnet.c - VKC/1 reference server and test client for kctool.
 * See docs/protocol.md.
 */
#include <dirent.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "kctool.h"
#include "vkc_proto.h"

#define SERVER_MAX_TAPE (16u * 1024u * 1024u)

typedef struct {
    int fd;
    char buf[4096];
    size_t pos, len;
} conn_t;

static int conn_fill(conn_t *c)
{
    if (c->pos < c->len)
        return 1;
    ssize_t r;
    do
        r = read(c->fd, c->buf, sizeof c->buf);
    while (r < 0 && errno == EINTR);
    if (r <= 0)
        return 0;
    c->pos = 0;
    c->len = (size_t)r;
    return 1;
}

/* Read a line without its terminator.  Returns length, or -1 on EOF/error/overlong. */
static int conn_line(conn_t *c, char *out, size_t max)
{
    size_t n = 0;
    for (;;) {
        if (!conn_fill(c))
            return -1;
        char ch = c->buf[c->pos++];
        if (ch == '\n')
            break;
        if (n + 1 >= max)
            return -1;
        out[n++] = ch;
    }
    if (n && out[n - 1] == '\r')
        n--;
    out[n] = 0;
    return (int)n;
}

static int conn_read(conn_t *c, uint8_t *out, size_t len)
{
    while (len) {
        if (!conn_fill(c))
            return 0;
        size_t k = c->len - c->pos;
        if (k > len)
            k = len;
        if (out) {
            memcpy(out, c->buf + c->pos, k);
            out += k;
        }
        c->pos += k;
        len -= k;
    }
    return 1;
}

static int conn_write(conn_t *c, const void *p, size_t len)
{
    const uint8_t *b = p;
    while (len) {
        ssize_t w = write(c->fd, b, len);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            return 0;
        b += w;
        len -= (size_t)w;
    }
    return 1;
}

static int conn_printf(conn_t *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int conn_printf(conn_t *c, const char *fmt, ...)
{
    char line[VKC_LINE_MAX + 64];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line)
        return 0;
    if (verbose > 1)
        fprintf(stderr, "> %s", line);
    return conn_write(c, line, (size_t)n);
}

/* Split a line into whitespace-separated words, in place. */
static int split(char *s, char **w, int max)
{
    int n = 0;
    char *save;
    for (char *t = strtok_r(s, " \t", &save); t && n < max; t = strtok_r(NULL, " \t", &save))
        w[n++] = t;
    return n;
}

static int parse_size(const char *s, size_t *out)
{
    char *end;
    errno = 0;
    unsigned long long v = strtoull(s, &end, 10);
    if (errno || *end || !*s || *s == '-')
        return 0;
    *out = (size_t)v;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Server                                                              */
/* ------------------------------------------------------------------ */

static const char *tape_dir = ".";

static void path_for(char *out, size_t max, const char *name, int meta)
{
    snprintf(out, max, "%s/%s%s%s", tape_dir, meta ? "." : "", name, meta ? ".attr" : "");
}

static void read_attrs(const char *name, char *out, size_t max)
{
    char p[4096];
    out[0] = 0;
    path_for(p, sizeof p, name, 1);
    FILE *f = fopen(p, "r");
    if (!f)
        return;
    if (fgets(out, (int)max, f))
        out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

static void srv_dir(conn_t *c)
{
    DIR *d = opendir(tape_dir);
    if (!d) {
        conn_printf(c, "ERR cannot read directory\n");
        return;
    }
    char **names = NULL;
    size_t n = 0;
    struct dirent *de;
    while ((de = readdir(d))) {
        char p[4096];
        struct stat st;
        if (!vkc_name_valid(de->d_name))
            continue;
        path_for(p, sizeof p, de->d_name, 0);
        if (stat(p, &st) || !S_ISREG(st.st_mode))
            continue;
        names = realloc(names, (n + 1) * sizeof *names);
        names[n++] = strdup(de->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, (int (*)(const void *, const void *))strcmp);

    conn_printf(c, "OK %zu\n", n);
    for (size_t i = 0; i < n; i++) {
        char p[4096], attrs[VKC_LINE_MAX];
        struct stat st;
        path_for(p, sizeof p, names[i], 0);
        if (stat(p, &st))
            st.st_size = 0;
        read_attrs(names[i], attrs, sizeof attrs);
        conn_printf(c, "%s %lld%s%s\n", names[i], (long long)st.st_size,
                    attrs[0] ? " " : "", attrs);
        free(names[i]);
    }
    free(names);
}

static void srv_get(conn_t *c, const char *name)
{
    char p[4096], attrs[VKC_LINE_MAX];
    if (!vkc_name_valid(name)) {
        conn_printf(c, "ERR bad name\n");
        return;
    }
    path_for(p, sizeof p, name, 0);
    FILE *f = fopen(p, "rb");
    if (!f) {
        conn_printf(c, "ERR not found\n");
        return;
    }
    fclose(f);
    size_t len;
    uint8_t *data = read_file(p, &len);
    read_attrs(name, attrs, sizeof attrs);
    conn_printf(c, "OK %zu%s%s\n", len, attrs[0] ? " " : "", attrs);
    conn_write(c, data, len);
    free(data);
    fprintf(stderr, "GET %s (%zu bytes)\n", name, len);
}

static void srv_put(conn_t *c, char **w, int nw)
{
    size_t len;
    if (nw < 3 || !parse_size(w[2], &len)) {
        conn_printf(c, "ERR usage: PUT <name> <size> [key=value ...]\n");
        return;
    }
    /* Always drain the payload so the connection stays in sync. */
    if (len > SERVER_MAX_TAPE) {
        conn_read(c, NULL, len);
        conn_printf(c, "ERR too large\n");
        return;
    }
    uint8_t *data = malloc(len ? len : 1);
    if (!data || !conn_read(c, data, len)) {
        free(data);
        return;
    }
    if (!vkc_name_valid(w[1])) {
        conn_printf(c, "ERR bad name\n");
        free(data);
        return;
    }

    char p[4096], tmp[4200];
    path_for(p, sizeof p, w[1], 0);
    snprintf(tmp, sizeof tmp, "%s/.%s.tmp", tape_dir, w[1]);
    FILE *f = fopen(tmp, "wb");
    if (!f || (len && fwrite(data, 1, len, f) != len) || fclose(f) || rename(tmp, p)) {
        unlink(tmp);
        conn_printf(c, "ERR write failed: %s\n", strerror(errno));
        free(data);
        return;
    }
    free(data);

    /* keep key=value attributes in a dot file next to the tape */
    path_for(p, sizeof p, w[1], 1);
    if (nw > 3 && (f = fopen(p, "w"))) {
        for (int i = 3; i < nw; i++)
            if (strchr(w[i], '='))
                fprintf(f, "%s%s", i > 3 ? " " : "", w[i]);
        fputc('\n', f);
        fclose(f);
    } else if (nw <= 3) {
        unlink(p);
    }
    conn_printf(c, "OK %zu\n", len);
    fprintf(stderr, "PUT %s (%zu bytes)\n", w[1], len);
}

static void serve_conn(int fd)
{
    conn_t c = { .fd = fd };
    char line[VKC_LINE_MAX];
    char *w[16];
    int hello = 0;

    struct timeval tv = { .tv_sec = 60 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    while (conn_line(&c, line, sizeof line) >= 0) {
        if (verbose > 1)
            fprintf(stderr, "< %s\n", line);
        int nw = split(line, w, 16);
        if (!nw)
            continue;
        if (!hello) {
            if (strcmp(w[0], "HELLO") || nw < 2 || strcmp(w[1], VKC_PROTO_VERSION)) {
                conn_printf(&c, "ERR unsupported version\n");
                return;
            }
            fprintf(stderr, "client %s\n", nw > 2 ? w[2] : "(anonymous)");
            conn_printf(&c, "OK %s kctool-serve\n", VKC_PROTO_VERSION);
            hello = 1;
        } else if (!strcmp(w[0], "DIR")) {
            srv_dir(&c);
        } else if (!strcmp(w[0], "GET") && nw == 2) {
            srv_get(&c, w[1]);
        } else if (!strcmp(w[0], "PUT")) {
            srv_put(&c, w, nw);
        } else if (!strcmp(w[0], "QUIT")) {
            conn_printf(&c, "OK bye\n");
            return;
        } else {
            conn_printf(&c, "ERR unknown command\n");
        }
    }
}

int cmd_serve(int argc, char **argv)
{
    int port = VKC_DEFAULT_PORT, c;
    while ((c = getopt(argc, argv, "p:d:v")) != -1) {
        switch (c) {
        case 'p': port = atoi(optarg); break;
        case 'd': tape_dir = optarg; break;
        case 'v': verbose++; break;
        default:
            fputs("usage: kctool serve [-p port] [-d dir] [-v]\n", stderr);
            return 2;
        }
    }
    signal(SIGPIPE, SIG_IGN);

    int s = socket(AF_INET6, SOCK_STREAM, 0);
    int fam6 = s >= 0;
    if (!fam6)
        s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0)
        die("socket: %s", strerror(errno));
    int one = 1, zero = 0;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    int rc;
    if (fam6) {
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof zero);
        struct sockaddr_in6 a = { .sin6_family = AF_INET6, .sin6_port = htons((uint16_t)port),
                                  .sin6_addr = IN6ADDR_ANY_INIT };
        rc = bind(s, (struct sockaddr *)&a, sizeof a);
    } else {
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
        rc = bind(s, (struct sockaddr *)&a, sizeof a);
    }
    if (rc || listen(s, 4))
        die("port %d: %s", port, strerror(errno));
    fprintf(stderr, "kctool: serving tapes from '%s' on port %d\n", tape_dir, port);

    for (;;) {
        int fd = accept(s, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR)
                continue;
            die("accept: %s", strerror(errno));
        }
        serve_conn(fd);
        close(fd);
    }
}

/* ------------------------------------------------------------------ */
/* Client                                                              */
/* ------------------------------------------------------------------ */

static void client_connect(conn_t *c, const char *target)
{
    char host[256], port[16];
    snprintf(port, sizeof port, "%d", VKC_DEFAULT_PORT);

    /* HOST, HOST:PORT, [v6]:PORT */
    const char *colon = strrchr(target, ':');
    if (target[0] == '[') {
        const char *rb = strchr(target, ']');
        if (!rb)
            die("bad address '%s'", target);
        snprintf(host, sizeof host, "%.*s", (int)(rb - target - 1), target + 1);
        if (rb[1] == ':')
            snprintf(port, sizeof port, "%s", rb + 2);
    } else if (colon && strchr(target, ':') == colon) {
        snprintf(host, sizeof host, "%.*s", (int)(colon - target), target);
        snprintf(port, sizeof port, "%s", colon + 1);
    } else {
        snprintf(host, sizeof host, "%s", target);
    }

    struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res, *ai;
    int e = getaddrinfo(host, port, &hints, &res);
    if (e)
        die("%s: %s", host, gai_strerror(e));
    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (!connect(fd, ai->ai_addr, ai->ai_addrlen))
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        die("cannot connect to %s port %s", host, port);

    struct timeval tv = { .tv_sec = 10 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    signal(SIGPIPE, SIG_IGN);
    memset(c, 0, sizeof *c);
    c->fd = fd;

    char line[VKC_LINE_MAX], hn[64] = "host";
    gethostname(hn, sizeof hn - 1);
    conn_printf(c, "HELLO %s kctool-%s\n", VKC_PROTO_VERSION, hn);
    if (conn_line(c, line, sizeof line) < 0)
        die("connection closed during handshake");
    if (strncmp(line, "OK", 2))
        die("server: %s", line);
    info("server: %s", line);
}

/* Read a status line; die on ERR.  Returns words after "OK". */
static int expect_ok(conn_t *c, char *line, size_t max, char **w, int maxw)
{
    if (conn_line(c, line, max) < 0)
        die("connection closed");
    if (strncmp(line, "OK", 2) || (line[2] && line[2] != ' '))
        die("server: %s", line);
    return split(line + 2, w, maxw);
}

int cmd_client(const char *verb, int argc, char **argv)
{
    char line[VKC_LINE_MAX];
    char *w[16];
    conn_t c;
    int opt;

    while ((opt = getopt(argc, argv, "v")) != -1) {
        if (opt == 'v')
            verbose++;
        else
            return 2;
    }
    argc -= optind;
    argv += optind;

    if (!strcmp(verb, "dir")) {
        if (argc != 1)
            die("usage: kctool dir HOST[:PORT]");
        client_connect(&c, argv[0]);
        conn_printf(&c, "DIR\n");
        int nw = expect_ok(&c, line, sizeof line, w, 16);
        size_t n;
        if (nw < 1 || !parse_size(w[0], &n))
            die("bad DIR reply");
        for (size_t i = 0; i < n; i++) {
            if (conn_line(&c, line, sizeof line) < 0)
                die("connection closed");
            puts(line);
        }
    } else if (!strcmp(verb, "get")) {
        if (argc < 2 || argc > 3)
            die("usage: kctool get HOST[:PORT] NAME [OUT]");
        client_connect(&c, argv[0]);
        conn_printf(&c, "GET %s\n", argv[1]);
        int nw = expect_ok(&c, line, sizeof line, w, 16);
        size_t n;
        if (nw < 1 || !parse_size(w[0], &n))
            die("bad GET reply");
        for (int i = 1; i < nw; i++)
            info("attribute %s", w[i]);
        uint8_t *data = malloc(n ? n : 1);
        if (!data || !conn_read(&c, data, n))
            die("short read");
        write_file(argc == 3 ? argv[2] : argv[1], data, n);
        info("received %zu bytes", n);
        free(data);
    } else { /* put */
        if (argc < 3)
            die("usage: kctool put HOST[:PORT] NAME IN [key=value ...]");
        if (!vkc_name_valid(argv[1]))
            die("bad tape name '%s'", argv[1]);
        size_t n;
        uint8_t *data = read_file(argv[2], &n);
        client_connect(&c, argv[0]);
        char attrs[VKC_LINE_MAX] = "";
        for (int i = 3; i < argc; i++) {
            strncat(attrs, " ", sizeof attrs - strlen(attrs) - 1);
            strncat(attrs, argv[i], sizeof attrs - strlen(attrs) - 1);
        }
        conn_printf(&c, "PUT %s %zu%s\n", argv[1], n, attrs);
        if (!conn_write(&c, data, n))
            die("write failed");
        expect_ok(&c, line, sizeof line, w, 16);
        info("stored %zu bytes", n);
        free(data);
    }
    conn_printf(&c, "QUIT\n");
    close(c.fd);
    return 0;
}
