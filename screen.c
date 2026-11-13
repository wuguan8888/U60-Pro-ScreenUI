#define _GNU_SOURCE
#include "abi.h"
#include "abi_bg.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <strings.h>
#include <unistd.h>

/* All launcher calls go through LV(): the b31 address is mapped to the running ABI.
 * U60_FAKE_LVGL (host tests only) routes them to a recording fake instead (tests/fake_lvgl.h). */
#ifdef U60_FAKE_LVGL
static void *fake_fn(uintptr_t b31);
static unsigned char fake_modal, fake_palette[256];
#define LV(a) fake_fn(a)
#define MODAL_BYTE (fake_modal)
#define PALETTE (fake_palette)
#else
#define LV(a) ((void *)(uintptr_t)launcher_abi_address(abi, (a)))
#define MODAL_BYTE (*(unsigned char *)abi->modal)
#define PALETTE ((unsigned char *)(uintptr_t)abi->palette)
#endif
#define F(a, r, ...) ((r (*)(__VA_ARGS__))LV(a))
#define BGF(a, r, ...) ((r (*)(__VA_ARGS__))LVBG(a))
#define P(o, n) (*(void **)((char *)(o) + (n)))
#define HIDDEN 1
#define CLICKABLE 2
#define SCROLLABLE 16
#define CHAIN_HOR 256
#define BUBBLE 16384
#define MAX_NODES 96
#define NAME_MAX 160

static const struct launcher_abi *abi;
static void *mainform, *page, *title, *node, *tun_label, *state_label;
static void *tun_caption;
#define LIST_ROWS 80
#define ROW_H 36
/* Page numbers (1-based, as stored at form+840). 1-2 are the stock pages. */
enum { PG_OVERVIEW = 3, PG_PROXY = 4, PG_BAND = 5, PG_CELL = 6, PAGE_COUNT = 6 };
#define PAGE_X(pg) (((pg) - 1) * 320)
/* Object budget (each label/panel is one pool entry; button() = 2). Checked by tests/test_ui.c
 * against what create() really allocates. The pool is static: create() can never hit _exit(70). */
#define BUDGET_PROXY (LIST_ROWS * 2 + 15)   /* 175 */
#define BUDGET_OVERVIEW 48
#define BAND_CHIPS 28
#define BUDGET_BAND (BAND_CHIPS * 4 + 20)   /* 132 */
#define CELL_ROWS 8
#define BUDGET_CELL (CELL_ROWS * 4 + 16)    /* 48 */
#define BUDGET_OVERLAY 24
#define BUDGET_DOTS 4
#define POOL_MAX (BUDGET_PROXY + BUDGET_OVERVIEW + BUDGET_BAND + BUDGET_CELL + BUDGET_OVERLAY + BUDGET_DOTS)
static void *allocs[POOL_MAX];
static int nalloc, current = 1, page_count = PAGE_COUNT;
static void *dots[PAGE_COUNT], *timer, *mode_buttons[3], *mode_captions[3];
static void *list_scroll, *row_buttons[LIST_ROWS], *row_captions[LIST_ROWS];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char nodes[MAX_NODES][NAME_MAX];
static char mode_name[8] = "rule";
static int node_count, node_index = -1, tun_on, generation, have_state;
static char status[96] = "正在读取";
static int modal_on(void) {
#ifdef U60_FAKE_LVGL
    return fake_modal;
#else
    return abi && MODAL_BYTE;
#endif
}

/* ---- background image + translucent cards (LVGL v8.3, RGB565) ---- */
#define BG_W 320
#define BG_H 432                      /* our page is 320x432; dots strip below is stock */
#define BG_BYTES ((size_t)BG_W * BG_H * 2)
#define BG_FILE "/data/u60-screen/bg.bin"
#define BG_DISABLE "/data/u60-screen/disable-bg"
#ifdef U60_FAKE_LVGL
#define LVBG(a) fake_fn(a)
#else
#define LVBG(a) ((void *)(uintptr_t)(a))
#endif
static int bg_abi_ok;                 /* set in init() after word-by-word verification */
static const char *bg_path = BG_FILE;  /* host previews point this at ./bg.bin */
static int bg_on;                     /* decided per create() */
static uint8_t *bg_pixels;            /* loaded once, never freed (LVGL keeps the pointer) */
static bg_img_dsc_t bg_dsc;
static void *tun_button, *test_button, *test_caption;
static int bg_load(void) {
    if (bg_pixels) return 0;
    int fd = open(bg_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || (size_t)st.st_size != BG_BYTES) { close(fd); return -1; }
    uint8_t *buf = malloc(BG_BYTES);
    if (!buf) { close(fd); return -1; }
    size_t got = 0;
    while (got < BG_BYTES) {
        ssize_t n = read(fd, buf + got, BG_BYTES - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != BG_BYTES) { free(buf); return -1; }
    bg_dsc.header = BG_IMG_HEADER(BG_W, BG_H);
    bg_dsc.data_size = (uint32_t)BG_BYTES;
    bg_dsc.data = buf;
    bg_pixels = buf;
    return 0;
}
static void bg_decide(void) {
    bg_on = bg_abi_ok && access(BG_DISABLE, F_OK) != 0 && bg_load() == 0;
}

/* Every pool object is OBJ_BYTES: the stock C++ wrapper uses the first 80 (label) / 48 (panel)
 * bytes; we keep a per-object cache of what we last set at OBJ_CACHE so render() can skip
 * identical LVGL calls (each one would invalidate + relayout the object). */
#define OBJ_BYTES 128
#define OBJ_CACHE 96
struct objcache { uint32_t text_h, tcol, bg; int16_t px, py; uint8_t has_text, has_tcol, has_bg, fknown, fval, has_pos; };
/* The only stock objects we ever pass to setters are the two stock page dots (0x50-byte wrappers,
 * no room for a cache at +96): give them a throw-away cache so a cached setter can never write past them. */
static void *stock_dot[2];
static struct objcache *oc(void *o) {
    static struct objcache scratch;
    if (o && (o == stock_dot[0] || o == stock_dot[1])) { memset(&scratch, 0, sizeof scratch); return &scratch; }
#ifdef U60_FAKE_LVGL
    {   /* tests: every other cached setter target must be one of our own pool objects */
        int mine = 0;
        for (int i = 0; i < nalloc && !mine; i++) mine = allocs[i] == o;
        if (!mine) { fprintf(stderr, "oc(): cached setter on a non-pool object %p\n", o); abort(); }
    }
#endif
    return (struct objcache *)((char *)o + OBJ_CACHE);
}
static void pos(void *o, int x, int y) { F(0x537450, void, void *, int, int)(o, x, y); }
static void size(void *o, int w, int h) { F(0x5382b4, void, void *, int, int)(o, w, h); }
static void flag_raw(void *o, unsigned f, int on) { F(on ? 0x42e3c0 : 0x42e4f4, void, void *, unsigned)(P(o, 8), f); }
/* HIDDEN / CLICKABLE toggles are cached (render() sets them every pass); other flags go straight through. */
static void flag(void *o, unsigned f, int on) {
    struct objcache *c = oc(o);
    if (f == HIDDEN || f == CLICKABLE) {
        unsigned bit = f == HIDDEN ? 1u : 2u;
        if ((c->fknown & bit) && !!(c->fval & bit) == !!on) return;
        c->fknown |= (uint8_t)bit;
        if (on) c->fval |= (uint8_t)bit; else c->fval &= (uint8_t)~bit;
    } else {
        if (f & HIDDEN) c->fknown &= (uint8_t)~1u;
        if (f & CLICKABLE) c->fknown &= (uint8_t)~2u;
    }
    flag_raw(o, f, on);
}
/* Uncached palette colour. Also used on the stock page dots, which are 0x50-byte stock wrappers
 * (operator new(0x50) at 0x4bd62c/0x4bd66c) with no cache area: never touch oc(o) here. The pool
 * objects it is used on are fresh from calloc (cache already empty) when it is called. */
static void color(void *o, int offset, int text) {
    unsigned char *p = PALETTE + offset;
    F(text ? 0x52f904 : 0x53793c, void, void *, int, int, int, int)(o, p[0], p[1], p[2], 255);
}
static void set_rgba(void *o, int r, int g, int b, int a, int text_color) {
    struct objcache *c = oc(o);
    uint32_t key = (uint32_t)(r & 255) | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255) << 16 | (uint32_t)(a & 255) << 24;
    if (text_color) { if (c->has_tcol && c->tcol == key) return; c->has_tcol = 1; c->tcol = key; }
    else { if (c->has_bg && c->bg == key) return; c->has_bg = 1; c->bg = key; }
    F(text_color ? 0x52f904 : 0x53793c, void, void *, int, int, int, int)(o, r, g, b, a);
}
static void paint(void *o, int r, int g, int b, int text_color) { set_rgba(o, r, g, b, 255, text_color); }
static void curve(void *o, int radius) { F(0x537868, void, void *, int)(o, radius); }
/* background fill with opacity; opacity is only used when the background image is on */
static void fill(void *o, int r, int g, int b, int alpha) { set_rgba(o, r, g, b, bg_on ? alpha : 255, 0); }
static void border(void *o, int width, int r, int g, int b, int opa) {
    if (!bg_abi_ok) return;               /* border setters are verified with the bg ABI */
    BGF(BG_SET_BORDER_WIDTH, void, void *, int)(o, width);
    BGF(BG_SET_BORDER_RGB, void, void *, int, int, int)(o, r, g, b);
    BGF(BG_LV_BORDER_OPA, void, void *, int, unsigned)(P(o, 8), opa, 0);
}
static void outline(void *o) { if (bg_on) border(o, 1, 200, 225, 255, 56); }
static void *objmem(void) {
    void *p = calloc(1, OBJ_BYTES);
    if (!p || nalloc >= POOL_MAX) _exit(70);       /* unreachable: budget checked by tests/test_ui.c */
    allocs[nalloc++] = p;
    return p;
}
static uint32_t fnv(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (unsigned char)*s) * 16777619u;
    return h | 1u;
}
/* Length of the longest prefix of s that is complete UTF-8 (snprintf truncation can cut a character). */
static size_t utf8_valid_len(const char *s) {
    size_t n = strlen(s), i = 0, ok = 0;
    while (i < n) {
        unsigned char ch = (unsigned char)s[i];
        size_t k = ch < 0x80 ? 1 : (ch & 0xE0) == 0xC0 ? 2 : (ch & 0xF0) == 0xE0 ? 3 : (ch & 0xF8) == 0xF0 ? 4 : 0;
        if (!k || i + k > n) break;
        size_t j = 1;
        while (j < k && ((unsigned char)s[i + j] & 0xC0) == 0x80) j++;
        if (j < k) break;
        i += k;
        ok = i;
    }
    return ok;
}
static void text(void *o, const char *s) {
    static char clean[1024];                  /* LVGL thread only */
    size_t vl = utf8_valid_len(s);
    if (s[vl]) {                              /* cut at the last complete character */
        if (vl >= sizeof clean) vl = sizeof clean - 1;
        memcpy(clean, s, vl);
        clean[vl] = 0;
        clean[utf8_valid_len(clean)] = 0;
        s = clean;
    }
    struct objcache *c = oc(o);
    uint32_t h = fnv(s);
    if (c->has_text && c->text_h == h) return;
    c->has_text = 1; c->text_h = h;
    struct { const char *p; size_t n, cap, pad; } str = {s, strlen(s), strlen(s), 0};
    F(0x52fa80, void, void *, void *, int)(o, &str, 0);
}
static void *label(void *p, int x, int y, int w, int h, int font, const char *s) {
    void *o = objmem();
    struct { const char *p; size_t n, cap, pad; } str = {"", 0, 0, 0};
    F(0x52fcc4, void, void *, void *, void *, int)(o, p, &str, 0);
    pos(o, x, y); size(o, w, h);
    F(0x52fa04, void, void *, int)(o, font);
    F(0x52f73c, void, void *, int)(o, 1);
    color(o, 24, 1); text(o, s); flag(o, CLICKABLE | SCROLLABLE, 0);
    return o;
}
static void *panel(void *p, int x, int y, int w, int h, int card) {
    void *o = objmem();
    F(0x533dcc, void, void *, void *, int)(o, p, -1);
    pos(o, x, y); size(o, w, h); flag(o, SCROLLABLE, 0); flag(o, CHAIN_HOR | BUBBLE, 1);
    if (card) { color(o, 12, 0); F(0x537868, void, void *, int)(o, 12); }
    return o;
}

/* ---- HTTP + proxy logic (worker thread only; no LVGL calls in this section) ----
 * Everything here is plain C so it can be compiled natively and tested against a mock
 * (tests/test_logic.c includes this file with U60_HOST_TEST). */
#ifndef U60_API_HOST
#define U60_API_HOST "192.168.0.1"
#endif
static const char *api_host = U60_API_HOST;
static int mihomo_port = 9097, agent_port = 9090;
#define T_MIHOMO_READ 2000           /* ms; the 2s budget the old code used for every call */
#define T_MIHOMO_WRITE 3000
#define T_AGENT 8000                 /* agent hot-reloads mihomo before it answers */
#define T_AGENT_TUN 12000            /* a TUN change restarts mihomo */
#define RULE_GROUP_PREFERRED "\xF0\x9F\x9A\x80\xE8\x8A\x82\xE7\x82\xB9\xE9\x80\x89\xE6\x8B\xA9" /* 🚀节点选择 */
#define RULE_GROUP_TTL 30            /* s; re-resolve the rule-mode selector this often */

static char agent_token[80];
static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Response body sink. tail=1 keeps the LAST cap-1 bytes (for /rules, where MATCH is last). */
struct sink { char *buf; size_t cap, len; int tail, truncated; };
static void sink_put(struct sink *s, const char *d, size_t n) {
    if (!s->buf || s->cap == 0) return;
    size_t room = s->cap - 1 - s->len;
    if (n <= room) { memcpy(s->buf + s->len, d, n); s->len += n; return; }
    s->truncated = 1;
    if (!s->tail) { memcpy(s->buf + s->len, d, room); s->len += room; return; }
    size_t keep = s->cap - 1;
    if (n >= keep) { memcpy(s->buf, d + (n - keep), keep); s->len = keep; return; }
    size_t drop = s->len + n - keep;
    memmove(s->buf, s->buf + drop, s->len - drop);
    s->len -= drop;
    memcpy(s->buf + s->len, d, n);
    s->len += n;
}

/* Incremental HTTP/1.1 response parser: status line, headers, identity or chunked body. */
struct resp { int status, chunked, state; long chunk_left; char hdr[2048]; size_t hlen; char line[24]; size_t llen; };
enum { R_HEAD, R_SIZE, R_DATA, R_DATA_END, R_IDENT, R_DONE };
static void resp_headers_done(struct resp *r) {
    r->hdr[r->hlen] = 0;
    if (sscanf(r->hdr, "HTTP/%*d.%*d %d", &r->status) != 1) r->status = -1;
    for (char *l = strstr(r->hdr, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        if (!strncasecmp(l + 2, "transfer-encoding:", 18)) {
            char *v = l + 20;
            char *e = strstr(v, "\r\n");
            size_t n = e ? (size_t)(e - v) : strlen(v);
            for (size_t i = 0; i + 7 <= n; i++) if (!strncasecmp(v + i, "chunked", 7)) r->chunked = 1;
        }
    }
    r->state = r->chunked ? R_SIZE : R_IDENT;
}
static void resp_feed(struct resp *r, const char *d, size_t n, struct sink *s) {
    size_t i = 0;
    while (i < n && r->state != R_DONE) {
        switch (r->state) {
        case R_HEAD:
            if (r->hlen + 1 >= sizeof r->hdr) { r->status = -1; r->state = R_DONE; return; }
            r->hdr[r->hlen++] = d[i++];
            if (r->hlen >= 4 && !memcmp(r->hdr + r->hlen - 4, "\r\n\r\n", 4)) resp_headers_done(r);
            break;
        case R_IDENT: sink_put(s, d + i, n - i); i = n; break;
        case R_SIZE: {
            char c = d[i++];
            if (c != '\n') { if (r->llen + 1 < sizeof r->line) r->line[r->llen++] = c; break; }
            r->line[r->llen] = 0;
            r->llen = 0;
            r->chunk_left = strtol(r->line, NULL, 16);
            r->state = r->chunk_left > 0 ? R_DATA : R_DONE;
            break;
        }
        case R_DATA: {
            size_t take = n - i < (size_t)r->chunk_left ? n - i : (size_t)r->chunk_left;
            sink_put(s, d + i, take);
            i += take;
            r->chunk_left -= (long)take;
            if (!r->chunk_left) { r->state = R_DATA_END; r->llen = 0; }
            break;
        }
        case R_DATA_END: /* the CRLF after chunk data */
            if (d[i++] == '\n') r->state = R_SIZE;
            break;
        }
    }
}

/* Returns the HTTP status (e.g. 200/204/401), or -1 on connect/send/recv failure or timeout.
 * out (if non-NULL) receives the decoded body, NUL-terminated. */
static int http_last_sent;            /* 1 once the whole request left this host (worker thread) */
static int http_req(int port, const char *method, const char *path, const char *body, const char *bearer, int timeout_ms,
                    char *out, size_t out_len, int tail) {
    http_last_sent = 0;
    if (out && out_len) out[0] = 0;
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);   /* also bounds connect() on Linux */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, api_host, &addr.sin_addr);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr)) { close(fd); return -1; }
    char head[1280];
    size_t blen = body ? strlen(body) : 0;
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n", method, path, api_host, port);
    if (bearer && bearer[0]) n += snprintf(head + n, sizeof head - (size_t)n, "Authorization: Bearer %s\r\n", bearer);
    if (body) n += snprintf(head + n, sizeof head - (size_t)n, "Content-Type: application/json\r\nContent-Length: %zu\r\n", blen);
    n += snprintf(head + n, sizeof head - (size_t)n, "\r\n");
    if (n >= (int)sizeof head) { close(fd); return -1; }
    if (send(fd, head, (size_t)n, MSG_NOSIGNAL) != n || (body && blen && send(fd, body, blen, MSG_NOSIGNAL) != (ssize_t)blen)) { close(fd); return -1; }
    http_last_sent = 1;
    static struct resp r;            /* worker thread only; 2.1 KB kept off the (128 KB musl) thread stack */
    memset(&r, 0, sizeof r);
    struct sink s = {out, out_len, 0, tail, 0};
    char chunk[4096];
    int64_t deadline = now_ms() + timeout_ms;
    int err = 0;
    while (r.state != R_DONE) {
        ssize_t got = recv(fd, chunk, sizeof chunk, 0);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { err = 1; break; }          /* timeout (EAGAIN) or reset */
        if (got == 0) break;
        resp_feed(&r, chunk, (size_t)got, &s);
        if (now_ms() > deadline) { err = 1; break; }
    }
    close(fd);
    if (out && out_len) out[s.len] = 0;
    if (r.state == R_HEAD || r.status <= 0) return -1;      /* never got a full header */
    if (err && r.state != R_IDENT) return -1;               /* cut off mid-body */
    return r.status;
}
static int ok2xx(int status) { return status >= 200 && status < 300; }
static int http(const char *method, const char *path, const char *body, char *out, size_t out_len) {
    return ok2xx(http_req(mihomo_port, method, path, body, NULL, T_MIHOMO_READ, out, out_len, 0)) ? 0 : -1;
}

/* RFC 3986 path-segment encoding: everything except unreserved is %XX (UTF-8 bytes, emoji included). */
static int url_encode(const char *in, char *out, size_t out_len) {
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        int plain = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '~';
        if (n + (plain ? 1 : 3) >= out_len) return -1;
        if (plain) out[n++] = (char)*p;
        else { out[n++] = '%'; out[n++] = hex[*p >> 4]; out[n++] = hex[*p & 15]; }
    }
    out[n] = 0;
    return 0;
}
static int json_escape(const char *in, char *out, size_t out_len) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        if (n + 7 >= out_len) return -1;
        if (*p == '"' || *p == '\\') { out[n++] = '\\'; out[n++] = (char)*p; }
        else if (*p < 0x20) n += (size_t)snprintf(out + n, out_len - n, "\\u%04x", *p);
        else out[n++] = (char)*p;
    }
    out[n] = 0;
    return 0;
}
static size_t utf8_put(char *d, unsigned c) {
    if (c < 0x80) { d[0] = (char)c; return 1; }
    if (c < 0x800) { d[0] = (char)(0xC0 | c >> 6); d[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c < 0x10000) { d[0] = (char)(0xE0 | c >> 12); d[1] = (char)(0x80 | (c >> 6 & 63)); d[2] = (char)(0x80 | (c & 63)); return 3; }
    d[0] = (char)(0xF0 | c >> 18); d[1] = (char)(0x80 | (c >> 12 & 63)); d[2] = (char)(0x80 | (c >> 6 & 63)); d[3] = (char)(0x80 | (c & 63));
    return 4;
}
static int hex4(const char *p, unsigned *v) {
    *v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : c >= 'A' && c <= 'F' ? (unsigned)(c - 'A' + 10) : 99;
        if (d > 15) return -1;
        *v = *v << 4 | d;
    }
    return 0;
}
/* Parses a JSON string at p (must start with '"'), handling escapes (Go emits \u0026 for &, etc). */
static const char *json_string(const char *p, char *dest, size_t dest_len) {
    if (*p != '"') return NULL;
    size_t n = 0;
    for (p++; *p && *p != '"'; p++) {
        if (n + 5 >= dest_len) return NULL;
        if (*p != '\\') { dest[n++] = *p; continue; }
        p++;
        switch (*p) {
        case 'n': dest[n++] = '\n'; break;
        case 't': dest[n++] = '\t'; break;
        case 'r': dest[n++] = '\r'; break;
        case 'b': dest[n++] = '\b'; break;
        case 'f': dest[n++] = '\f'; break;
        case 'u': {
            unsigned c;
            if (hex4(p + 1, &c)) return NULL;
            p += 4;
            if (c >= 0xD800 && c < 0xDC00 && p[1] == '\\' && p[2] == 'u') {
                unsigned lo;
                if (!hex4(p + 3, &lo) && lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); p += 6; }
            }
            n += utf8_put(dest + n, c);
            break;
        }
        case 0: return NULL;
        default: dest[n++] = *p; break;   /* \" \\ \/ */
        }
    }
    if (*p != '"') return NULL;
    dest[n] = 0;
    return p + 1;
}
static int skip_name(const char *name) {
    return !strcmp(name, "DIRECT") || !strcmp(name, "REJECT") || strstr(name, "剩余") || strstr(name, "到期") || strstr(name, "重置");
}

/* GET /proxies/<name>. Returns 0 and fills type/now (and the "all" list if list!=NULL). */
static int get_group(const char *name, char *buf, size_t buf_len, char *type, size_t type_len, char *now, size_t now_len,
                     char (*list)[NAME_MAX], int max, int *count) {
    char enc[NAME_MAX * 3 + 1], path[NAME_MAX * 3 + 16];
    if (url_encode(name, enc, sizeof enc)) return -1;
    snprintf(path, sizeof path, "/proxies/%s", enc);
    if (http("GET", path, NULL, buf, buf_len)) return -1;
    if (type) { type[0] = 0; const char *t = strstr(buf, "\"type\":"); if (t) json_string(t + 7, type, type_len); }
    if (now) { now[0] = 0; const char *t = strstr(buf, "\"now\":"); if (t) json_string(t + 6, now, now_len); }
    if (count) *count = 0;
    if (list && count) {
        const char *p = strstr(buf, "\"all\":[");
        if (p) {
            p += 7;
            while (*p && *p != ']' && *count < max) {
                while (*p == ' ' || *p == ',') p++;
                if (*p != '"') break;
                char item[NAME_MAX];
                p = json_string(p, item, sizeof item);
                if (!p) break;
                if (!skip_name(item)) snprintf(list[(*count)++], NAME_MAX, "%s", item);
            }
        }
    }
    return 0;
}
static int is_selector(const char *name, char *buf, size_t buf_len, char *now, size_t now_len) {
    char type[32];
    return !get_group(name, buf, buf_len, type, sizeof type, now, now_len, NULL, 0, NULL) && !strcmp(type, "Selector");
}

/* Which selector actually picks the node in rule mode.
 *  1. 🚀节点选择 (the group the user's subscription uses; verified on the device)
 *  2. the MATCH rule's target, following `now` through nested Selectors (e.g. 🐟漏网之鱼 -> 🚀节点选择)
 *  3. the first Selector in GLOBAL.all (config order; same as the agent's main_route())
 *  4. GLOBAL */
static char rule_group[NAME_MAX];
static int64_t rule_group_at = -1;
static char gbuf[65536];                 /* worker-only scratch, too big for the thread stack */
static void resolve_rule_group(char *dest, size_t dest_len) {
    char now[NAME_MAX];
    if (is_selector(RULE_GROUP_PREFERRED, gbuf, sizeof gbuf, now, sizeof now)) { snprintf(dest, dest_len, "%s", RULE_GROUP_PREFERRED); return; }
    if (ok2xx(http_req(mihomo_port, "GET", "/rules", NULL, NULL, T_MIHOMO_WRITE, gbuf, 16384, 1))) {
        char target[NAME_MAX] = "";
        const char *m = NULL;
        for (const char *p = gbuf; (p = strstr(p, "\"type\":\"Match\"")); p++) m = p;
        if (m) {
            const char *px = strstr(m, "\"proxy\":");
            const char *next = strstr(m + 1, "\"type\":");
            if (px && (!next || px < next)) json_string(px + 8, target, sizeof target);
        }
        if (target[0] && is_selector(target, gbuf, sizeof gbuf, now, sizeof now)) {
            for (int depth = 0; depth < 4 && now[0]; depth++) {
                char next_now[NAME_MAX];
                if (!is_selector(now, gbuf, sizeof gbuf, next_now, sizeof next_now)) break;
                snprintf(target, sizeof target, "%s", now);
                snprintf(now, sizeof now, "%s", next_now);
            }
            snprintf(dest, dest_len, "%s", target);
            return;
        }
    }
    static char all[MAX_NODES][NAME_MAX];
    int count = 0;
    if (!get_group("GLOBAL", gbuf, sizeof gbuf, NULL, 0, NULL, 0, all, MAX_NODES, &count))
        for (int i = 0; i < count; i++)
            if (is_selector(all[i], gbuf, sizeof gbuf, now, sizeof now)) { snprintf(dest, dest_len, "%s", all[i]); return; }
    snprintf(dest, dest_len, "GLOBAL");
}
static const char *group_for_mode(const char *mode) {
    if (!strcmp(mode, "global")) return "GLOBAL";
    int64_t t = now_ms();
    if (rule_group_at < 0 || t - rule_group_at > RULE_GROUP_TTL * 1000 || !rule_group[0]) {
        resolve_rule_group(rule_group, sizeof rule_group);
        rule_group_at = t;
    }
    return rule_group;
}

static int load_agent_password(char *dest, size_t dest_len) {
    FILE *file = fopen("/data/local/tmp/start_zte_agent.sh", "r");
    if (!file) return -1;
    char line[512];
    int ok = -1;
    while (fgets(line, sizeof line, file)) {
        char *mark = strstr(line, "ZTE_AGENT_PASSWORD=");
        if (!mark) continue;
        mark += strlen("ZTE_AGENT_PASSWORD=");
        if (*mark == '\'' || *mark == '"') mark++;
        size_t n = 0;
        while (*mark && *mark != '\'' && *mark != '"' && *mark != ' ' && *mark != '\n' && n + 1 < dest_len) dest[n++] = *mark++;
        dest[n] = 0;
        ok = n ? 0 : -1;
        break;
    }
    fclose(file);
    return ok;
}
static int ensure_agent(void) {
    if (agent_token[0]) return 0;
    char password[160];
    if (load_agent_password(password, sizeof password)) return -1;
    char encoded[480], body[512], response[2048];
    if (json_escape(password, encoded, sizeof encoded)) { memset(password, 0, sizeof password); return -1; }
    memset(password, 0, sizeof password);
    snprintf(body, sizeof body, "{\"password\":\"%s\"}", encoded);
    memset(encoded, 0, sizeof encoded);
    int st = http_req(agent_port, "POST", "/api/auth/login", body, NULL, T_MIHOMO_WRITE, response, sizeof response, 0);
    memset(body, 0, sizeof body);
    if (!ok2xx(st)) return -1;
    const char *token = strstr(response, "\"token\":\"");
    if (!token) return -1;
    token += 9;
    size_t n = 0;
    while (*token && *token != '"' && n + 1 < sizeof agent_token) agent_token[n++] = *token++;
    agent_token[n] = 0;
    return agent_token[0] ? 0 : -1;
}
/* PUT /api/proxy/settings. Re-logs in once on 401/403 (expired token). A timeout is NOT retried:
 * the agent may still be applying the first request. */
static int agent_settings(const char *body, int timeout_ms) {
    char response[2048];
    if (ensure_agent()) return -1;
    int st = http_req(agent_port, "PUT", "/api/proxy/settings", body, agent_token, timeout_ms, response, sizeof response, 0);
    if (ok2xx(st)) return 0;
    agent_token[0] = 0;
    if (st != 401 && st != 403) return -1;
    if (ensure_agent()) return -1;
    st = http_req(agent_port, "PUT", "/api/proxy/settings", body, agent_token, timeout_ms, response, sizeof response, 0);
    if (!ok2xx(st)) { agent_token[0] = 0; return -1; }
    return 0;
}
/* DELETE /connections: drop the old chain's connections (QUIC/H2 keep reusing them otherwise). */
static int close_connections(void) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (ok2xx(http_req(mihomo_port, "DELETE", "/connections", NULL, NULL, T_MIHOMO_WRITE, NULL, 0, 0))) return 0;
        usleep(300000);
    }
    return -1;
}
static int select_node(const char *group, const char *name) {
    char enc[NAME_MAX * 3 + 1], path[NAME_MAX * 3 + 16], esc[NAME_MAX * 6 + 1], body[NAME_MAX * 6 + 16], response[1024];
    if (url_encode(group, enc, sizeof enc) || json_escape(name, esc, sizeof esc)) return -1;
    snprintf(path, sizeof path, "/proxies/%s", enc);
    snprintf(body, sizeof body, "{\"name\":\"%s\"}", esc);
    return ok2xx(http_req(mihomo_port, "PUT", path, body, NULL, T_MIHOMO_WRITE, response, sizeof response, 0)) ? 0 : -1;
}

/* 1 if `group` is a Selector whose `all` lists `name` (raw list, nothing skipped), 0 if not, -1 on HTTP error. */
static int group_has_member(const char *group, const char *name) {
    char type[32];
    char enc[NAME_MAX * 3 + 1], path[NAME_MAX * 3 + 16];
    if (url_encode(group, enc, sizeof enc)) return -1;
    snprintf(path, sizeof path, "/proxies/%s", enc);
    if (http("GET", path, NULL, gbuf, sizeof gbuf)) return -1;
    type[0] = 0;
    const char *t = strstr(gbuf, "\"type\":");
    if (t) json_string(t + 7, type, sizeof type);
    if (strcmp(type, "Selector")) return 0;
    const char *p = strstr(gbuf, "\"all\":[");
    if (!p) return 0;
    for (p += 7; *p && *p != ']';) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') break;
        char item[NAME_MAX];
        p = json_string(p, item, sizeof item);
        if (!p) break;
        if (!strcmp(item, name)) return 1;
    }
    return 0;
}
/* Node tap: switch the group that carries traffic in the current mode (`active`, the list on screen)
 * and then mirror the pick into the other mode's group (GLOBAL <-> rule selector) when it lists the
 * node, so the choice holds in either mode and survives mode switches.
 * Returns 0 ok, -1 active PUT failed (nothing else touched); *soft set if the mirror failed. */
static int select_node_synced(const char *active, const char *name, int *soft) {
    *soft = 0;
    if (select_node(active, name)) return -1;
    char other[NAME_MAX];
    snprintf(other, sizeof other, "%s", strcmp(active, "GLOBAL") ? "GLOBAL" : group_for_mode("rule"));
    if (!strcmp(other, active)) return 0;            /* no separate rule selector (fallback was GLOBAL) */
    int member = group_has_member(other, name);
    if (member < 0 || (member == 1 && select_node(other, name))) *soft = 1;
    return 0;
}

/* ---- shared state between the LVGL thread (clicked/render/tick) and the worker ---- */
enum { OP_NONE, OP_MODE, OP_NODE, OP_TUN, OP_TEST };
static int busy_op;                       /* op in flight (tap accepted, not yet refreshed) */
static int busy_mode = -1, busy_tun = -1;
static char busy_name[NAME_MAX], busy_group[NAME_MAX];
static char group_name[NAME_MAX] = "GLOBAL"; /* the group whose list is on screen */
static char note[64];                     /* failure / warning text */
static int64_t note_until;
static pthread_cond_t wake_cond;
static int wake_flag;
static pthread_once_t wake_once = PTHREAD_ONCE_INIT;
static void wake_init(void) {
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);   /* the clock jumps at NTP sync after boot */
    pthread_cond_init(&wake_cond, &a);
    pthread_condattr_destroy(&a);
}
static int note_soft;                     /* 1 = notice (amber), 0 = failure (red) */
static void set_note_locked(const char *s, int ms, int soft) {
    snprintf(note, sizeof note, "%s", s);
    note_until = now_ms() + ms;
    note_soft = soft;
}

static void refresh_state(void) {
    static char buf[65536], list[MAX_NODES][NAME_MAX];
    char now[NAME_MAX] = "", type[32];
    int count = 0, tun = 0, ok = 0;
    char cfg[8192] = "";
    char parsed_mode[8] = "rule";
    int cfg_ok = !http("GET", "/configs", NULL, cfg, sizeof cfg);
    if (cfg_ok) {
        const char *t = strstr(cfg, "\"tun\":");
        if (t && strstr(t, "\"enable\":true") && strstr(t, "\"enable\":true") < t + 80) tun = 1;
        const char *mode_at = strstr(cfg, "\"mode\":\"");
        if (mode_at) {
            mode_at += 8;
            if (!strncmp(mode_at, "global", 6)) snprintf(parsed_mode, sizeof parsed_mode, "global");
            else if (!strncmp(mode_at, "direct", 6)) snprintf(parsed_mode, sizeof parsed_mode, "direct");
        }
    } else {
        /* /configs did not answer: keep the last known mode/TUN instead of claiming "rule" + GLOBAL list */
        pthread_mutex_lock(&lock);
        int known = have_state;
        if (known) { snprintf(parsed_mode, sizeof parsed_mode, "%s", mode_name); tun = tun_on; }
        pthread_mutex_unlock(&lock);
        cfg_ok = known;
    }
    char group[NAME_MAX];
    snprintf(group, sizeof group, "%s", cfg_ok ? group_for_mode(parsed_mode) : "GLOBAL");
    if (!get_group(group, buf, sizeof buf, type, sizeof type, now, sizeof now, list, MAX_NODES, &count)) ok = 1;
    else if (strcmp(group, "GLOBAL")) {
        rule_group_at = -1;                 /* group vanished (new subscription?): re-resolve next time */
        snprintf(group, sizeof group, "GLOBAL");
        if (!get_group(group, buf, sizeof buf, type, sizeof type, now, sizeof now, list, MAX_NODES, &count)) ok = 1;
    }
    pthread_mutex_lock(&lock);
    snprintf(mode_name, sizeof mode_name, "%s", parsed_mode);
    snprintf(group_name, sizeof group_name, "%s", group);
    node_count = count;
    for (int i = 0; i < count; i++) memcpy(nodes[i], list[i], NAME_MAX);
    node_index = -1;
    for (int i = 0; i < count; i++) if (!strcmp(nodes[i], now)) node_index = i;
    tun_on = tun;
    have_state = ok;
    snprintf(status, sizeof status, ok ? (count ? "" : "没有可选节点") : "代理接口无响应");
    generation++;
    pthread_mutex_unlock(&lock);
}


/* ---- 全部测速 (latency test of the group on screen) ----
 * Same URL/timeout as the agent's POST /api/proxy/delay (config::HEALTH_URL, timeout=5000). */
#define DELAY_QUERY "url=https%3A%2F%2Fwww.gstatic.com%2Fgenerate_204&timeout=5000"
#define T_GROUP_DELAY 8000           /* ms socket budget for the whole group call */
#define T_NODE_DELAY 6000            /* ms per node in the per-node fallback (mihomo gives up at 5000) */
#define FALLBACK_BUDGET 30000        /* ms overall cap for the per-node fallback */
#define DELAY_KEEP_MS (5 * 60 * 1000)
struct delay_entry { char name[NAME_MAX]; int ms; };   /* ms: >0 measured, 0 = timeout/failed */
static struct delay_entry delays[MAX_NODES];           /* guarded by lock */
static int delay_count;
static int64_t delay_at = -1;
static char test_names[MAX_NODES][NAME_MAX];           /* snapshot taken at tap time (UI thread, under lock) */
static int test_count;
/* -1 = no (fresh) result for this name. Caller holds lock. */
static int delay_lookup_locked(const char *name) {
    if (delay_at < 0 || now_ms() - delay_at > DELAY_KEEP_MS) return -1;
    for (int i = 0; i < delay_count; i++) if (!strcmp(delays[i].name, name)) return delays[i].ms;
    return -1;
}
static void delay_set_locked(const char *name, int ms) {
    for (int i = 0; i < delay_count; i++) if (!strcmp(delays[i].name, name)) { delays[i].ms = ms; return; }
    if (delay_count < MAX_NODES) { snprintf(delays[delay_count].name, NAME_MAX, "%s", name); delays[delay_count++].ms = ms; }
}
/* 0 none, 1 good (<200ms), 2 slow (<500ms), 3 bad (>=500ms or timeout) — same cut-offs as the web UI */
static int delay_tone(int ms) { return ms < 0 ? 0 : ms == 0 ? 3 : ms < 200 ? 1 : ms < 500 ? 2 : 3; }
#ifdef U60_ASCII_LABELS
#define TXT_TEST_ALL "TEST"
#define TXT_TESTING "TESTING..."
#define TXT_TIMEOUT "timeout"
#define TXT_TEST_FAIL "test failed"
#else
#define TXT_TEST_ALL "全部测速"
#define TXT_TESTING "测速中..."
#define TXT_TIMEOUT "超时"
#define TXT_TEST_FAIL "测速失败"
#endif
static void delay_text(int ms, char *out, size_t out_len) {
    if (ms < 0) out[0] = 0;
    else if (ms == 0) snprintf(out, out_len, "%s", TXT_TIMEOUT);
    else snprintf(out, out_len, "%dms", ms);
}
/* Row caption: the delay goes FIRST so it stays visible even if a long name wraps/clips. */
static void row_text(const char *name, int ms, char *out, size_t out_len) {
    char d[24];
    delay_text(ms, d, sizeof d);
    if (d[0]) snprintf(out, out_len, "%s  %s", d, name);
    else snprintf(out, out_len, "%s", name);
}
/* Parses mihomo's {"name":ms,...} map into the shared table. Returns entries parsed, -1 if not an object. */
static int parse_delay_map(const char *p, int (*cb)(const char *name, int ms)) {
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
    if (*p != '{') return -1;
    p++;
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t') p++;
        if (*p == '}' || !*p) break;
        char name[NAME_MAX];
        const char *q = json_string(p, name, sizeof name);
        if (!q) return n ? n : -1;
        p = q;
        while (*p == ' ' || *p == ':') p++;
        char *end;
        long ms = strtol(p, &end, 10);
        if (end == p) return n ? n : -1;
        p = end;
        while (*p && *p != ',' && *p != '}') p++;    /* skip fractions/garbage */
        cb(name, ms > 0 ? (int)(ms > 99999 ? 99999 : ms) : 0);
        n++;
    }
    return n;
}
static int store_delay_cb(const char *name, int ms) { delay_set_locked(name, ms); return 0; }
static char dbuf[16384];             /* worker-only */
/* Returns 0 on success, -1 on failure (shows TXT_TEST_FAIL). */
static int run_delay_test(const char *group, char (*names)[NAME_MAX], int count) {
    char enc[NAME_MAX * 3 + 1], path[NAME_MAX * 3 + 128];
    if (url_encode(group, enc, sizeof enc)) return -1;
    snprintf(path, sizeof path, "/group/%s/delay?%s", enc, DELAY_QUERY);
    int st = http_req(mihomo_port, "GET", path, NULL, NULL, T_GROUP_DELAY, dbuf, sizeof dbuf, 0);
    if (ok2xx(st) || st == 504) {
        pthread_mutex_lock(&lock);
        delay_count = 0;
        delay_at = now_ms();
        for (int i = 0; i < count; i++) delay_set_locked(names[i], 0);   /* tested but absent => timeout */
        int parsed = st == 504 ? 0 : parse_delay_map(dbuf, store_delay_cb);   /* 504: mihomo's "all timed out" */
        if (parsed < 0) delay_count = 0;
        generation++;
        pthread_mutex_unlock(&lock);
        return parsed < 0 ? -1 : 0;
    }
    if (st != 404) return -1;                    /* old mihomo without /group: test node by node */
    pthread_mutex_lock(&lock);
    delay_count = 0;
    delay_at = now_ms();
    generation++;
    pthread_mutex_unlock(&lock);
    int64_t deadline = now_ms() + FALLBACK_BUDGET;
    int answered = 0;                             /* mihomo replied at all (any HTTP status) */
    for (int i = 0; i < count && now_ms() < deadline; i++) {
        if (url_encode(names[i], enc, sizeof enc)) continue;
        snprintf(path, sizeof path, "/proxies/%s/delay?%s", enc, DELAY_QUERY);
        int s2 = http_req(mihomo_port, "GET", path, NULL, NULL, T_NODE_DELAY, dbuf, sizeof dbuf, 0);
        if (s2 == 404) { answered = 1; continue; }  /* node vanished since the tap */
        int ms = 0;                                  /* 408 Timeout / 503 / socket timeout => 超时 */
        if (ok2xx(s2)) { const char *d = strstr(dbuf, "\"delay\":"); ms = d ? atoi(d + 8) : 0; if (ms < 0) ms = 0; }
        if (s2 > 0) answered = 1;
        pthread_mutex_lock(&lock);
        delay_set_locked(names[i], ms);
        generation++;                                /* results appear row by row */
        pthread_mutex_unlock(&lock);
    }
    if (answered) return 0;
    pthread_mutex_lock(&lock);
    delay_count = 0;                                 /* mihomo never answered: don't claim every node timed out */
    generation++;
    pthread_mutex_unlock(&lock);
    return -1;
}

/* Runs the op the UI thread queued (at most one, see clicked()). Returns 1 if an op ran. */
static int apply_pending(void) {
    int op, mode, tun;
    char name[NAME_MAX], group[NAME_MAX];
    pthread_mutex_lock(&lock);
    op = busy_op; mode = busy_mode; tun = busy_tun;
    snprintf(name, sizeof name, "%s", busy_name);
    snprintf(group, sizeof group, "%s", busy_group);
    pthread_mutex_unlock(&lock);
    if (op == OP_NONE) return 0;
    const char *fail = NULL, *warn = NULL;
    if (op == OP_NODE) {
        int unsynced = 0;
        if (select_node_synced(group, name, &unsynced)) fail = "节点切换失败";
        else if (close_connections()) warn = "已切换，旧连接未断开";
        else if (unsynced) warn = "已切换（另一模式未同步）";
    } else if (op == OP_MODE) {
        const char *value = mode == 1 ? "global" : mode == 2 ? "direct" : "rule";
        char body[32];
        snprintf(body, sizeof body, "{\"mode\":\"%s\"}", value);
        if (agent_settings(body, T_AGENT)) fail = "模式切换失败";
        else if (close_connections()) warn = "已切换，旧连接未断开";
        rule_group_at = -1;
    } else if (op == OP_TEST) {
        static char names[MAX_NODES][NAME_MAX];
        pthread_mutex_lock(&lock);
        int count = test_count;
        memcpy(names, test_names, sizeof names);
        pthread_mutex_unlock(&lock);
        if (run_delay_test(group, names, count)) fail = TXT_TEST_FAIL;
    } else if (op == OP_TUN) {
        char body[32];
        snprintf(body, sizeof body, "{\"tun\":%s}", tun ? "true" : "false");
        if (agent_settings(body, T_AGENT_TUN)) fail = "透明代理切换失败";
    }
    pthread_mutex_lock(&lock);
    if (fail) set_note_locked(fail, 4000, 0);
    else if (warn) set_note_locked(warn, 3000, 1);
    pthread_mutex_unlock(&lock);
    return 1;
}

/* ==== Agent data for the overview / band-lock / cell-lock pages (worker thread only) ====
 * Shapes come from the agent code, not the design spec: GET /api/dashboard returns the RAW
 * zte_nwinfo_api netinfo object under data.signal (agent/src/handlers.rs dashboard(), read_radio()),
 * and the web app maps it client-side (web-app/src/data/api.ts mapSignal, data/bands.ts).
 * The C below mirrors that mapping field for field. */

/* -- minimal JSON walker: no allocation, values are pointers into the response buffer -- */
static const char *jws(const char *p) { while (p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; return p; }
static const char *jskip(const char *p) {
    p = jws(p);
    if (!p || !*p) return NULL;
    if (*p == '"') {
        for (p++; *p && *p != '"'; p++) if (*p == '\\' && p[1]) p++;
        return *p == '"' ? p + 1 : NULL;
    }
    if (*p == '{' || *p == '[') {
        int depth = 0;
        for (; *p; p++) {
            if (*p == '"') { const char *q = jskip(p); if (!q) return NULL; p = q - 1; continue; }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && --depth == 0) return p + 1;
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
    return p == s ? NULL : p;
}
/* Value of `key` in the object at obj (top level of that object only), or NULL. */
static const char *jget(const char *obj, const char *key) {
    obj = jws(obj);
    if (!obj || *obj != '{') return NULL;
    const char *p = jws(obj + 1);
    size_t klen = strlen(key);
    while (p && *p == '"') {
        const char *k = p + 1, *after = jskip(p);
        if (!after) return NULL;
        int match = (size_t)(after - 1 - k) == klen && !memcmp(k, key, klen);   /* looked-up keys have no escapes */
        p = jws(after);
        if (*p != ':') return NULL;
        const char *v = jws(p + 1);
        if (match) return v;
        p = jskip(v);
        if (!p) return NULL;
        p = jws(p);
        if (*p != ',') break;
        p = jws(p + 1);
    }
    return NULL;
}
static const char *jpath(const char *json, const char *path) {
    char key[48];
    const char *v = json;
    while (*path && v) {
        size_t n = strcspn(path, ".");
        if (n >= sizeof key) return NULL;
        memcpy(key, path, n); key[n] = 0;
        v = jget(v, key);
        path += n;
        if (*path == '.') path++;
    }
    return v;
}
static int jobj(const char *v) { return v && *v == '{'; }
static int jstr(const char *v, char *out, size_t n) {
    if (n) out[0] = 0;
    if (!v || *v != '"') return -1;
    return json_string(v, out, n) ? 0 : -1;
}
static int jbool(const char *v) { return !v ? -1 : !strncmp(v, "true", 4) ? 1 : !strncmp(v, "false", 5) ? 0 : -1; }
/* web-app validate.ts finiteNumber(): /^[+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?$/ after trim */
static int numeric_text(const char *s, size_t n, double *out) {
    while (n && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')) { s++; n--; }
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    if (!n || n > 40) return -1;
    char buf[48];
    memcpy(buf, s, n);
    buf[n] = 0;
    const char *q = buf;
    if (*q == '+' || *q == '-') q++;
    int digits = 0;
    while (*q >= '0' && *q <= '9') { q++; digits++; }
    if (*q == '.') { q++; while (*q >= '0' && *q <= '9') { q++; digits++; } }
    if (!digits) return -1;
    if (*q == 'e' || *q == 'E') {
        q++;
        if (*q == '+' || *q == '-') q++;
        int ed = 0;
        while (*q >= '0' && *q <= '9') { q++; ed++; }
        if (!ed) return -1;
    }
    if (*q) return -1;
    double d = strtod(buf, NULL);
    if (!(d > -1e300 && d < 1e300)) return -1;
    *out = d;
    return 0;
}
static int jnum(const char *v, double *out) {
    if (!v) return -1;
    if (*v == '"') {
        char s[48];
        if (jstr(v, s, sizeof s)) return -1;
        return numeric_text(s, strlen(s), out);
    }
    if (*v != '-' && *v != '+' && *v != '.' && !(*v >= '0' && *v <= '9')) return -1;
    const char *e = jskip(v);
    return e ? numeric_text(v, (size_t)(e - v), out) : -1;
}
static int is_int(double d) { return d >= -9e15 && d <= 9e15 && d == (double)(long long)d; }
static int jint(const char *v, long long min, long long max, long long *out) {
    double d;
    if (jnum(v, &d) || !is_int(d) || d < (double)min || d > (double)max) return -1;
    *out = (long long)d;
    return 0;
}
/* Up to max numeric elements of an array; -1 if v is not an array. */
static int jarr_ints(const char *v, int *out, int max) {
    v = jws(v);
    if (!v || *v != '[') return -1;
    int n = 0;
    const char *p = jws(v + 1);
    while (p && *p && *p != ']') {
        double d;
        if (n < max && !jnum(p, &d) && is_int(d)) out[n++] = (int)d;
        p = jskip(p);
        p = jws(p);
        if (p && *p == ',') p = jws(p + 1);
    }
    return n;
}
static int jarr_len(const char *v) {
    v = jws(v);
    if (!v || *v != '[') return -1;
    int n = 0;
    const char *p = jws(v + 1);
    while (p && *p && *p != ']') {
        p = jskip(p);
        if (!p) return -1;
        n++;
        p = jws(p);
        if (*p == ',') p = jws(p + 1);
    }
    return n;
}

/* -- the mapped model (mirrors web-app types.ts SignalInfo / CarrierComponent / BandLockState) -- */
#define MAX_CARRIERS 8
#define MAX_LOCK_BANDS 64
struct carrier {
    char label[8], band[12], bw[16];   /* "PCC"/"SCC0"; "n78"/"B3"; "100 MHz" or "—" */
    int bandnum, pci;                  /* pci -1 = not reported (0 is valid) */
    long long arfcn;
    int has_rsrp, has_sinr;
    double rsrp, sinr;
};
enum { LK_UNKNOWN, LK_AUTO, LK_LOCKED };
struct bandlock { int kind, n; int bands[MAX_LOCK_BANDS]; };   /* bands sorted, unique */
enum { NM_UNKNOWN, NM_SA, NM_NSA, NM_LTE };
enum { RAT_NONE, RAT_LTE, RAT_NR };
struct dash {
    int ok, stale;                     /* ok = last GET parsed; stale = sources.signal.stale */
    char type[24], carrier_name[48];
    int mode, bars;                    /* bars -1 = unknown */
    int nlte, nnr, prim;               /* prim: RAT_* of the serving (primary) carrier */
    struct carrier lte[MAX_CARRIERS], nr[MAX_CARRIERS];
    struct bandlock lte_lock, sa_lock;
    int has_speed; double rx_Bps, tx_Bps;
    int has_batt, batt_pct, charging;
    int has_cpu_temp, has_batt_temp; double cpu_temp, batt_temp;
    int has_usage; double day_bytes;
};

static int str_is(const char *s, const char *a) { return !strcmp(s, a); }
static void trim_copy(char *out, size_t n, const char *s, size_t len) {
    while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
    while (len && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = 0;
}
/* Splits "a,b,c" into at most max trimmed fields (each < 24 bytes). Returns the field count. */
static int split_fields(const char *seg, size_t seglen, char (*f)[24], int max) {
    int n = 0;
    size_t i = 0;
    while (n < max) {
        size_t j = i;
        while (j < seglen && seg[j] != ',') j++;
        trim_copy(f[n++], 24, seg + i, j - i);
        if (j >= seglen) break;
        i = j + 1;
    }
    return n;
}
static int field_int(const char *s, long long min, long long max, long long *out) {
    double d;
    if (numeric_text(s, strlen(s), &d) || !is_int(d) || d < (double)min || d > (double)max) return -1;
    *out = (long long)d;
    return 0;
}
static int field_num(const char *s, double *out) { return numeric_text(s, strlen(s), out); }
static int digits_of(const char *s) { int v = 0, any = 0; for (; *s; s++) if (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); any = 1; if (v > 100000) break; } return any ? v : 0; }

/* api.ts classifyNetworkType(): ENDC/EN-DC/NSA -> nsa, \bSA\b -> sa, 4G/LTE -> lte. Bare 5G/NR = unknown. */
static int classify_network_type(const char *raw) {
    char t[32];
    size_t n = 0;
    for (const char *p = raw; *p && n + 1 < sizeof t; p++) t[n++] = (char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p);
    t[n] = 0;
    char *s = t;
    while (*s == ' ') s++;
    for (char *e = s + strlen(s); e > s && e[-1] == ' '; ) *--e = 0;
    if (!*s) return NM_UNKNOWN;
    if (strstr(s, "ENDC") || strstr(s, "EN-DC") || strstr(s, "NSA")) return NM_NSA;
    for (const char *p = s; (p = strstr(p, "SA")); p++) {
        int wb = p == s || !((p[-1] >= 'A' && p[-1] <= 'Z') || (p[-1] >= '0' && p[-1] <= '9') || p[-1] == '_');
        char c = p[2];
        int we = !((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
        if (wb && we) return NM_SA;
    }
    if (str_is(s, "4G") || strstr(s, "LTE")) return NM_LTE;
    return NM_UNKNOWN;
}
static int cmp_int(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void norm_bands(int *b, int *n) {
    qsort(b, (size_t)*n, sizeof *b, cmp_int);
    int k = 0;
    for (int i = 0; i < *n; i++) if (b[i] > 0 && (k == 0 || b[k - 1] != b[i])) b[k++] = b[i];
    *n = k;
}
/* bands.ts parseLteBandLock(): '' / '0' automatic; hex '0x..' or decimal mask, band N = bit N-1, max 128. */
static void parse_lte_lock(const char *v, struct bandlock *out) {
    char s[64];
    memset(out, 0, sizeof *out);
    out->kind = LK_UNKNOWN;
    if (!v || *v != '"' || jstr(v, s, sizeof s)) return;          /* missing / non-string: unknown */
    char t[64];
    trim_copy(t, sizeof t, s, strlen(s));
    if (!t[0] || str_is(t, "0")) { out->kind = LK_AUTO; return; }
    unsigned __int128 mask = 0;
    const char *p = t;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        if (!*p) return;
        int sig = 0;
        for (; *p; p++) {
            int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : *p >= 'A' && *p <= 'F' ? *p - 'A' + 10 : -1;
            if (d < 0) return;
            if (sig || d) sig++;
            if (sig > 32) return;                                      /* bits above 128: cannot represent */
            mask = mask << 4 | (unsigned)d;
        }
    } else {
        const unsigned __int128 max = ~(unsigned __int128)0;
        for (; *p; p++) {
            if (*p < '0' || *p > '9') return;
            unsigned d = (unsigned)(*p - '0');
            if (mask > (max - d) / 10) return;
            mask = mask * 10 + d;
        }
    }
    if (!mask) { out->kind = LK_AUTO; return; }
    for (int b = 1; b <= 128 && out->n < MAX_LOCK_BANDS; b++) if ((mask >> (b - 1)) & 1) out->bands[out->n++] = b;
    out->kind = out->n ? LK_LOCKED : LK_UNKNOWN;
}
/* bands.ts parseNrBandLock(): '' / '0' automatic; comma list of 1..1024. */
static void parse_nr_lock(const char *v, struct bandlock *out) {
    char s[512];
    memset(out, 0, sizeof *out);
    out->kind = LK_UNKNOWN;
    if (!v || *v != '"' || jstr(v, s, sizeof s)) return;
    char t[512];
    trim_copy(t, sizeof t, s, strlen(s));
    if (!t[0] || str_is(t, "0")) { out->kind = LK_AUTO; return; }
    int n = 0, bands[MAX_LOCK_BANDS];
    for (const char *p = t;;) {
        const char *e = strchr(p, ',');
        size_t len = e ? (size_t)(e - p) : strlen(p);
        char tok[16];
        if (len > 15) return;
        trim_copy(tok, sizeof tok, p, len);
        if (!tok[0] || strspn(tok, "0123456789") != strlen(tok) || strlen(tok) > 5) return;
        int b = atoi(tok);
        if (b < 1 || b > 1024 || n >= MAX_LOCK_BANDS) return;
        bands[n++] = b;
        if (!e) break;
        p = e + 1;
    }
    norm_bands(bands, &n);
    memcpy(out->bands, bands, sizeof(int) * (size_t)n);
    out->n = n;
    out->kind = LK_LOCKED;
}
/* api.ts mapSignal() */
static void map_signal(const char *sig, struct dash *d) {
    static char ca[1024], casig[1024];
    char tmp[64];
    long long ll;
    double x;
    d->nlte = d->nnr = 0;
    d->prim = RAT_NONE;
    /* LTE PCC; skip ghost carriers (SA mode leaves stale LTE fields with an NR active band) */
    int pcc_pci = jint(jget(sig, "lte_pci"), 0, 503, &ll) ? -1 : (int)ll;
    long long pcc_earfcn = -1;
    if (!jnum(jget(sig, "wan_active_channel"), &x) && x >= 0 && is_int(x)) pcc_earfcn = (long long)x;
    char pband[16];
    jstr(jget(sig, "wan_active_band"), pband, sizeof pband);
    int pband_num = digits_of(pband);
    jstr(jget(sig, "lteca"), ca, sizeof ca);
    char pcc_bw[24] = "";
    if (pcc_pci >= 0 && pcc_earfcn >= 0) {
        for (const char *seg = ca; *seg;) {
            const char *e = strchr(seg, ';');
            size_t len = e ? (size_t)(e - seg) : strlen(seg);
            char f[8][24];
            if (split_fields(seg, len, f, 8) >= 5) {
                long long a, b;
                if (!field_int(f[0], -9000000000000000LL, 9000000000000000LL, &a) && !field_int(f[3], -9000000000000000LL, 9000000000000000LL, &b) &&
                    a == pcc_pci && b == pcc_earfcn) { snprintf(pcc_bw, sizeof pcc_bw, "%s", f[4]); break; }
            }
            if (!e) break;
            seg = e + 1;
        }
    }
    int lte_valid = pband[0] && !str_is(pband, "0") && !str_is(pband, "B") && !str_is(pband, "B0") && pband[0] != 'n' && pband[0] != 'N' &&
                    pcc_earfcn > 0;
    if (lte_valid) {
        struct carrier *c = &d->lte[d->nlte++];
        memset(c, 0, sizeof *c);
        snprintf(c->label, sizeof c->label, "PCC");
        if (pband_num) snprintf(c->band, sizeof c->band, "B%d", pband_num); else snprintf(c->band, sizeof c->band, "%s", pband);
        c->bandnum = pband_num;
        c->pci = pcc_pci;
        c->arfcn = pcc_earfcn;
        if (pcc_bw[0]) snprintf(c->bw, sizeof c->bw, "%s MHz", pcc_bw); else snprintf(c->bw, sizeof c->bw, "—");
        if (!jnum(jget(sig, "lte_rsrp"), &x) && x < 0) { c->has_rsrp = 1; c->rsrp = x; }
        if (!jnum(jget(sig, "lte_snr"), &x)) { c->has_sinr = 1; c->sinr = x; }
        /* SCCs: lteca "pci,band,?,earfcn,bw;..." (PCC entry skipped once) + ltecasig "rsrp,rsrq,sinr,rssi,ul,active;..." by index */
        jstr(jget(sig, "ltecasig"), casig, sizeof casig);
        double sig_rsrp[MAX_CARRIERS], sig_sinr[MAX_CARRIERS];
        int sig_has_rsrp[MAX_CARRIERS], sig_has_sinr[MAX_CARRIERS], nsig = 0;
        for (const char *seg = casig; *seg && nsig < MAX_CARRIERS;) {
            const char *e = strchr(seg, ';');
            size_t len = e ? (size_t)(e - seg) : strlen(seg);
            char f[8][24];
            int nf = len ? split_fields(seg, len, f, 8) : 0;
            int blank = 1;
            for (size_t i = 0; i < len; i++) if (seg[i] != ' ' && seg[i] != '\t') blank = 0;
            if (!blank && nf >= 4) {
                sig_has_rsrp[nsig] = !field_num(f[0], &sig_rsrp[nsig]);
                sig_has_sinr[nsig] = !field_num(f[2], &sig_sinr[nsig]);
                nsig++;
            }
            if (!e) break;
            seg = e + 1;
        }
        int pcc_found = 0, idx = 0;
        for (const char *seg = ca; *seg && d->nlte < MAX_CARRIERS;) {
            const char *e = strchr(seg, ';');
            size_t len = e ? (size_t)(e - seg) : strlen(seg);
            char f[8][24];
            int blank = 1;
            for (size_t i = 0; i < len; i++) if (seg[i] != ' ' && seg[i] != '\t') blank = 0;
            if (!blank && split_fields(seg, len, f, 8) >= 5) {
                long long epci = -1, eear = 0;
                int has_pci = !field_int(f[0], -9000000000000000LL, 9000000000000000LL, &epci);
                int has_ear = !field_int(f[3], -9000000000000000LL, 9000000000000000LL, &eear);
                if (!pcc_found && pcc_pci >= 0 && has_pci && has_ear && epci == pcc_pci && eear == pcc_earfcn) pcc_found = 1;
                else {
                    struct carrier *s = &d->lte[d->nlte++];
                    memset(s, 0, sizeof *s);
                    snprintf(s->label, sizeof s->label, "SCC%d", idx);
                    snprintf(s->band, sizeof s->band, "B%s", f[1]);
                    s->bandnum = atoi(f[1]);
                    s->pci = has_pci && epci >= 0 && epci <= 503 ? (int)epci : -1;
                    s->arfcn = has_ear ? eear : 0;
                    snprintf(s->bw, sizeof s->bw, "%s MHz", f[4]);
                    if (idx < nsig && sig_has_rsrp[idx] && sig_rsrp[idx] != 0) { s->has_rsrp = 1; s->rsrp = sig_rsrp[idx]; }
                    if (idx < nsig && sig_has_sinr[idx]) { s->has_sinr = 1; s->sinr = sig_sinr[idx]; }
                    idx++;
                }
            }
            if (!e) break;
            seg = e + 1;
        }
    }
    /* NR PCC (+ nrca SCCs) */
    char nband[16];
    jstr(jget(sig, "nr5g_action_band"), nband, sizeof nband);
    long long narfcn = 0;
    if (!jnum(jget(sig, "nr5g_action_channel"), &x) && x >= 0 && is_int(x)) narfcn = (long long)x;
    if (nband[0] && !str_is(nband, "0") && !str_is(nband, "n") && !str_is(nband, "n0") && narfcn > 0) {
        struct carrier *c = &d->nr[d->nnr++];
        memset(c, 0, sizeof *c);
        snprintf(c->label, sizeof c->label, "PCC");
        if (nband[0] == 'n') snprintf(c->band, sizeof c->band, "%s", nband); else snprintf(c->band, sizeof c->band, "n%s", nband);
        c->bandnum = digits_of(nband);
        c->pci = jint(jget(sig, "nr5g_pci"), 0, 1007, &ll) ? -1 : (int)ll;
        c->arfcn = narfcn;
        if (!jnum(jget(sig, "nr5g_bandwidth"), &x)) snprintf(c->bw, sizeof c->bw, "%g MHz", x); else snprintf(c->bw, sizeof c->bw, "—");
        if (!jnum(jget(sig, "nr5g_rsrp"), &x) && x < 0) { c->has_rsrp = 1; c->rsrp = x; }
        if (!jnum(jget(sig, "nr5g_snr"), &x)) { c->has_sinr = 1; c->sinr = x; }
        int ppci = c->pci;
        jstr(jget(sig, "nrca"), ca, sizeof ca);
        for (const char *seg = ca; *seg && d->nnr < MAX_CARRIERS;) {
            const char *e = strchr(seg, ';');
            size_t len = e ? (size_t)(e - seg) : strlen(seg);
            char f[12][24];
            int blank = 1;
            for (size_t i = 0; i < len; i++) if (seg[i] != ' ' && seg[i] != '\t') blank = 0;
            int nf = blank ? 0 : split_fields(seg, len, f, 12);
            if (nf >= 6) {
                long long spci = -1, sarfcn = 0, sband = 0;
                int has_pci = !field_int(f[1], 0, 1007, &spci);
                if (field_int(f[4], -9000000000000000LL, 9000000000000000LL, &sarfcn)) sarfcn = 0;
                if (!(has_pci && ppci >= 0 && spci == ppci && sarfcn == narfcn)) {
                    if (field_int(f[3], -9000000000000000LL, 9000000000000000LL, &sband)) sband = 0;
                    struct carrier *s = &d->nr[d->nnr];
                    memset(s, 0, sizeof *s);
                    snprintf(s->label, sizeof s->label, "SCC%d", d->nnr - 1);
                    d->nnr++;
                    snprintf(s->band, sizeof s->band, "n%lld", sband);
                    s->bandnum = (int)sband;
                    s->pci = has_pci ? (int)spci : -1;
                    s->arfcn = sarfcn;
                    snprintf(s->bw, sizeof s->bw, "%s MHz", f[5]);
                    double r;
                    int measured = nf >= 8 && !field_num(f[7], &r) && r > -140;   /* -140 = 3GPP floor: unmeasured */
                    if (measured) { s->has_rsrp = 1; s->rsrp = r; }
                    if (measured && nf >= 10 && !field_num(f[9], &r)) { s->has_sinr = 1; s->sinr = r; }
                }
            }
            if (!e) break;
            seg = e + 1;
        }
    }
    jstr(jget(sig, "network_type"), d->type, sizeof d->type);
    d->mode = classify_network_type(d->type);
    /* api.ts pickPrimary(): SA -> NR PCC, LTE/NSA -> LTE PCC (anchor), unknown -> only if exactly one RAT has a PCC */
    int lp = d->nlte > 0, np = d->nnr > 0;
    if (d->mode == NM_SA) d->prim = np ? RAT_NR : RAT_NONE;
    else if (d->mode == NM_LTE || d->mode == NM_NSA) d->prim = lp ? RAT_LTE : RAT_NONE;
    else d->prim = lp && !np ? RAT_LTE : np && !lp ? RAT_NR : RAT_NONE;
    char name[48];
    d->carrier_name[0] = 0;
    if (!jstr(jget(sig, "network_provider_fullname"), name, sizeof name) && name[strspn(name, " \t")]) snprintf(d->carrier_name, sizeof d->carrier_name, "%s", name);
    else if (!jstr(jget(sig, "network_provider"), name, sizeof name) && name[strspn(name, " \t")]) snprintf(d->carrier_name, sizeof d->carrier_name, "%s", name);
    d->bars = jint(jget(sig, "signalbar"), 0, 5, &ll) ? -1 : (int)ll;
    parse_lte_lock(jget(sig, "lte_band_lock"), &d->lte_lock);
    parse_nr_lock(jget(sig, "nr5g_sa_band_lock"), &d->sa_lock);   /* SA only: the NSA lock is a separate observation */
    (void)tmp;
}
/* Parses a whole GET /api/dashboard response. Returns 0 if {"ok":true,"data":{...}}. */
static int parse_dashboard(const char *json, struct dash *d) {
    memset(d, 0, sizeof *d);
    d->bars = -1;
    d->lte_lock.kind = d->sa_lock.kind = LK_UNKNOWN;
    if (jbool(jget(json, "ok")) != 1) return -1;
    const char *data = jget(json, "data");
    if (!jobj(data)) return -1;
    const char *sig = jget(data, "signal");
    if (jobj(sig)) map_signal(sig, d);
    d->stale = jbool(jpath(data, "sources.signal.stale")) == 1 || !jobj(sig);
    double x;
    const char *sp = jget(data, "speed");
    if (jobj(sp) && !jnum(jget(sp, "rx_speed"), &d->rx_Bps) && !jnum(jget(sp, "tx_speed"), &d->tx_Bps)) d->has_speed = 1;
    const char *bt = jget(data, "battery");
    if (jobj(bt) && jget(bt, "status")) {            /* agent: battery is null without a battery */
        char st[24];
        long long pct;
        if (!jint(jget(bt, "capacity"), 0, 100, &pct)) { d->has_batt = 1; d->batt_pct = (int)pct; }
        jstr(jget(bt, "status"), st, sizeof st);
        d->charging = str_is(st, "Charging");
        if (!jnum(jget(bt, "temperature"), &x) && x != 0) { d->has_batt_temp = 1; d->batt_temp = x / 10; }
    }
    const char *th = jget(data, "thermal");
    if (jobj(th) && !jnum(jget(th, "cpuss_temp"), &x)) { d->has_cpu_temp = 1; d->cpu_temp = x; }
    const char *day = jpath(data, "data_usage.day");
    double rx, tx;
    if (jobj(day) && !jnum(jget(day, "rx_bytes"), &rx) && !jnum(jget(day, "tx_bytes"), &tx) && rx >= 0 && tx >= 0) {
        d->has_usage = 1;
        d->day_bytes = rx + tx;                       /* usage.ts usageTotal(): both directions or unknown */
    }
    d->ok = 1;
    return 0;
}

/* -- shared agent-side state (guarded by lock) -- */
static struct dash dash;
static int dash_err;                 /* 0 ok, 1 agent unreachable, 2 login failed, 3 bad response */
static int clients_n = -1;
static int caps_ok, caps_lte[64], caps_nr[64], ncaps_lte, ncaps_nr, caps_nsa_supported;
static int visible_page = 1;
#define T_AGENT_READ 4000
#define T_LOCK 15000                 /* web client default timeout (client.ts) */
static int lock_timeout_ms = T_LOCK;
static int64_t login_retry_at;       /* background reads back off after a failed login */
/* GET with the agent token; re-logs in once on 401/403 (a read is safe to repeat). */
static int agent_get(const char *path, char *out, size_t len, int *err) {
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!agent_token[0] && now_ms() < login_retry_at) { *err = 2; return -1; }
        if (ensure_agent()) { login_retry_at = now_ms() + 30000; *err = 2; return -1; }
        int st = http_req(agent_port, "GET", path, NULL, agent_token, T_AGENT_READ, out, len, 0);
        if (st == 401 || st == 403) { agent_token[0] = 0; continue; }
        if (st < 0) { *err = 1; return -1; }
        if (!ok2xx(st)) { *err = 3; return -1; }
        *err = 0;
        return st;
    }
    *err = 2;
    return -1;
}
static char dashbuf[98304];          /* worker-only */
/* bands this device has served on since the screen started (PCC or SCC); used to warn before
 * locking to bands it has never seen. Lock held. */
static unsigned char seen_band[2][1024];          /* [0] NR, [1] LTE */
static void note_seen_locked(void) {
    for (int i = 0; i < dash.nnr; i++) if (dash.nr[i].bandnum > 0 && dash.nr[i].bandnum < 1024) seen_band[0][dash.nr[i].bandnum] = 1;
    for (int i = 0; i < dash.nlte; i++) if (dash.lte[i].bandnum > 0 && dash.lte[i].bandnum < 1024) seen_band[1][dash.lte[i].bandnum] = 1;
}
static void refresh_dash(void) {
    static struct dash d;
    int err = 0;
    if (agent_get("/api/dashboard", dashbuf, sizeof dashbuf, &err) > 0 && parse_dashboard(dashbuf, &d)) err = 3;
    if (err) memset(&d, 0, sizeof d);
    pthread_mutex_lock(&lock);
    dash = d;                        /* a failed read shows "—", never the last numbers */
    dash_err = err;
    if (!err) note_seen_locked();
    generation++;
    pthread_mutex_unlock(&lock);
}
static void refresh_clients(void) {
    int err = 0, n = -1;
    if (agent_get("/api/network/clients", dashbuf, sizeof dashbuf, &err) > 0 && jbool(jget(dashbuf, "ok")) == 1)
        n = jarr_len(jpath(dashbuf, "data.clients"));
    pthread_mutex_lock(&lock);
    if (n != clients_n) { clients_n = n; generation++; }
    pthread_mutex_unlock(&lock);
}
static void refresh_caps(void) {
    int err = 0, l[64], r[64], nl = -1, nr = -1;
    if (agent_get("/api/modem/capabilities", dashbuf, sizeof dashbuf, &err) > 0 && jbool(jget(dashbuf, "ok")) == 1) {
        nl = jarr_ints(jpath(dashbuf, "data.lte_bands"), l, 64);
        nr = jarr_ints(jpath(dashbuf, "data.nr_sa_bands"), r, 64);
    }
    if (nl < 0 || nr < 0) return;
    norm_bands(l, &nl);
    norm_bands(r, &nr);
    pthread_mutex_lock(&lock);
    memcpy(caps_lte, l, sizeof l); ncaps_lte = nl;
    memcpy(caps_nr, r, sizeof r); ncaps_nr = nr;
    caps_nsa_supported = jbool(jpath(dashbuf, "data.nr_nsa_band_lock_supported")) == 1;
    caps_ok = 1;
    generation++;
    pthread_mutex_unlock(&lock);
}

/* ==== Band / cell lock operations ====
 * Endpoints (agent/src/server.rs routes, agent/src/cell.rs handlers):
 *   POST /api/cell/band/nr    {"nr5g_type":"SA","nr5g_band":"41,78"}            cell_band_nr (SA only, NR_BANDS)
 *   POST /api/cell/band/lte   {"is_lte_band":"1","lte_band_mask":"<decimal u128>","is_gw_band":"0","gw_band_mask":"0"}
 *   POST /api/cell/band/reset (no body)                                           nwinfo_rest_band_rat
 *   POST /api/cell/lock/nr    {"lock_nr_pci","lock_nr_earfcn","lock_nr_cell_band"} (strings, as the web app sends)
 *   POST /api/cell/lock/lte   {"lock_lte_pci","lock_lte_earfcn"}
 *   POST /api/cell/lock/reset (no body)                                           nwinfo_reset_band_cell_setting
 * Replies: 200 {"ok":true,"data":..}; 400/503 {"ok":false,"error":".."}; 401 unauthorized.
 * Safety: the only caller of lock_post() is lock_step(), which runs a snapshot only after the
 * confirm button consumed a matching token; a lock POST is never retried automatically. */
enum { LK_NONE, LK_BAND_NR, LK_BAND_LTE, LK_BAND_RESET, LK_CELL_NR, LK_CELL_LTE, LK_CELL_RESET };
enum { LS_IDLE, LS_CONFIRM, LS_SENDING, LS_VERIFY, LS_OK, LS_FAIL, LS_SENT };
struct lockop { int kind, page, nb; int bands[MAX_LOCK_BANDS]; int pci, bandnum; long long arfcn; };
static struct lockop lk_op;                /* frozen snapshot: what the dialog shows and the POST sends */
static int lk_state, lk_polls, lk_preset;  /* preset: the target cell already served before the lock */
static unsigned lk_token, lk_confirmed;
static int64_t lk_state_at, lk_next_poll;
static char lk_reason[256], lk_readback[160];
static char dlg_title[64], dlg_lead[128], dlg_details[320], dlg_cons[400], dlg_recovery[160], dlg_warn[96];
static int dlg_warn_bad;                   /* 1: red "never seen" warning instead of the amber default */
static char cell_last_req[64];             /* last cell lock this screen sent (RAM only; the agent has no readback) */
static int verify_every_ms = 2000;         /* SPEC 5.6: every 2 s ... */
#define VERIFY_POLLS 15                     /* ... at most 15 times (30 s) */
#define RESULT_OK_MS 5000
static int lock_is_band(int k) { return k == LK_BAND_NR || k == LK_BAND_LTE || k == LK_BAND_RESET; }
enum { SP_IDLE, SP_RUN, SP_DONE, SP_FAIL, SP_STOPPED };   /* 测速 state (speed test, overview) */
static int st_state;
static int lock_busy_locked(void) { return lk_state == LS_CONFIRM || lk_state == LS_SENDING || lk_state == LS_VERIFY; }

/* -- drafts (band page): kept apart from the observation, like web draft.ts -- */
struct draft { int edited, n; int bands[MAX_LOCK_BANDS]; };
static struct draft drafts[2];             /* 0 = NR (SA), 1 = LTE */
static int band_tab;                       /* which chip set is shown; UI only */
static const struct bandlock *observed_lock_locked(int rat) { return rat ? &dash.lte_lock : &dash.sa_lock; }
static int lock_is_all_locked(int rat);
/* The draft starts from the device's lock only when that lock is a real subset. A lock covering every
 * supported band is how this firmware reports "auto" (nwinfo_rest_band_rat leaves all bands listed), so
 * the draft then starts EMPTY and taps ADD bands. (Starting from all bands made one tap on n78 mean
 * "everything except n78" -- the 2026-11-13 device incident.) */
static int observed_draft_locked(int rat, int *out) {
    const struct bandlock *l = observed_lock_locked(rat);
    if (!dash.ok || l->kind != LK_LOCKED || lock_is_all_locked(rat)) return 0;
    memcpy(out, l->bands, sizeof(int) * (size_t)l->n);
    return l->n;
}
static int draft_locked(int rat, int *out) {
    if (drafts[rat].edited) { memcpy(out, drafts[rat].bands, sizeof(int) * (size_t)drafts[rat].n); return drafts[rat].n; }
    return observed_draft_locked(rat, out);
}
static int has_band(const int *b, int n, int x) { for (int i = 0; i < n; i++) if (b[i] == x) return 1; return 0; }
static int same_bands(const int *a, int na, const int *b, int nb) { return na == nb && !memcmp(a, b, sizeof(int) * (size_t)na); }
/* number of bands that differ between the draft and the device (the "(N)" on the apply button) */
static int draft_changes_locked(int rat) {
    int d[MAX_LOCK_BANDS], o[MAX_LOCK_BANDS];
    int nd = draft_locked(rat, d), no = observed_draft_locked(rat, o), c = 0;
    for (int i = 0; i < nd; i++) c += !has_band(o, no, d[i]);
    for (int i = 0; i < no; i++) c += !has_band(d, nd, o[i]);
    return c;
}
static void draft_toggle_locked(int rat, int band) {
    struct draft *dr = &drafts[rat];
    if (!dr->edited) { dr->n = observed_draft_locked(rat, dr->bands); dr->edited = 1; }
    int k = -1;
    for (int i = 0; i < dr->n; i++) if (dr->bands[i] == band) k = i;
    if (k >= 0) { memmove(dr->bands + k, dr->bands + k + 1, sizeof(int) * (size_t)(dr->n - k - 1)); dr->n--; }
    else if (dr->n < MAX_LOCK_BANDS) { dr->bands[dr->n++] = band; norm_bands(dr->bands, &dr->n); }
    int o[MAX_LOCK_BANDS], no = observed_draft_locked(rat, o);
    if (same_bands(dr->bands, dr->n, o, no)) dr->edited = 0;    /* back to the device value: pristine again */
    generation++;
}
/* Lock covers every supported band = this firmware's "auto" ("All bands" in bands.ts describeBandLock).
 * After nwinfo_rest_band_rat the device reports nr5g_sa_band_lock = all 21 NR bands and
 * lte_band_lock = 0x87e29a0e00df: every supported LTE band up to B48 -- the firmware's 64-bit LTE mask
 * cannot hold B66/B71 (bits 65/70), so supported bands above 64 are not required here. */
static int lock_is_all_locked(int rat) {
    const struct bandlock *l = observed_lock_locked(rat);
    const int *caps = rat ? caps_lte : caps_nr;
    int n = rat ? ncaps_lte : ncaps_nr, need = 0;
    if (l->kind != LK_LOCKED || !caps_ok || !n) return 0;
    for (int i = 0; i < n; i++) {
        if (rat && caps[i] > 64) continue;
        need++;
        if (!has_band(l->bands, l->n, caps[i])) return 0;
    }
    return need > 0;
}
static void band_list_text(int rat, const int *b, int n, char *out, size_t len) {
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < n && k + 8 < len; i++) k += (size_t)snprintf(out + k, len - k, "%s%s%d", i ? ", " : "", rat ? "B" : "n", b[i]);
}
/* "自动" / "已锁 n41, n78" / "全部频段" / "未知" */
static void describe_lock_locked(int rat, char *out, size_t len) {
    const struct bandlock *l = observed_lock_locked(rat);
    if (!dash.ok || l->kind == LK_UNKNOWN) { snprintf(out, len, "未知"); return; }
    if (l->kind == LK_AUTO) { snprintf(out, len, "自动"); return; }
    if (lock_is_all_locked(rat)) { snprintf(out, len, "自动（全部频段）"); return; }
    char b[160];
    band_list_text(rat, l->bands, l->n, b, sizeof b);
    if (l->n > 4) snprintf(out, len, "已锁 %d 个", l->n); else snprintf(out, len, "已锁 %s", b);
}
static const struct carrier *pcc_of_locked(int rat) {
    if (rat == RAT_NR) return dash.nnr ? &dash.nr[0] : NULL;
    if (rat == RAT_LTE) return dash.nlte ? &dash.lte[0] : NULL;
    return NULL;
}
static const struct carrier *primary_locked(void) { return dash.ok ? pcc_of_locked(dash.prim) : NULL; }
/* bands of `rat` the modem serves on right now (PCC + SCC) */
static int in_use_locked(int rat, int band) {
    const struct carrier *c = rat ? dash.lte : dash.nr;
    int n = !dash.ok ? 0 : rat ? dash.nlte : dash.nnr;
    for (int i = 0; i < n; i++) if (c[i].bandnum == band) return 1;
    return 0;
}

/* -- cell candidates: the serving carriers only (the agent has no neighbour-cell API) -- */
struct cand { int rat, pci, bandnum, has_rsrp; long long arfcn; double rsrp; char label[8], band[12]; };
static int candidates_locked(struct cand *out, int max) {
    int n = 0;
    if (!dash.ok) return 0;
    for (int r = 0; r < 2; r++) {                 /* NR first, then LTE (web ServingCells order) */
        const struct carrier *c = r ? dash.lte : dash.nr;
        int cn = r ? dash.nlte : dash.nnr;
        for (int i = 0; i < cn && n < max; i++, n++) {
            out[n].rat = r ? RAT_LTE : RAT_NR;
            out[n].pci = c[i].pci;
            out[n].bandnum = c[i].bandnum;
            out[n].arfcn = c[i].arfcn;
            out[n].has_rsrp = c[i].has_rsrp;
            out[n].rsrp = c[i].rsrp;
            snprintf(out[n].label, sizeof out[n].label, "%s", c[i].label);
            snprintf(out[n].band, sizeof out[n].band, "%s", c[i].band);
        }
    }
    return n;
}
static int cell_sel_rat, cell_sel_pci = -1;      /* selected candidate (UI, under lock) */
static long long cell_sel_arfcn = -1;

/* "n41, n28 等 20 个" / "n1, n3" / "无": supported bands the lock leaves out, serving ones first */
static void exclusion_text_locked(int rat, const int *keep, int nk, char *out, size_t len) {
    const int *caps = rat ? caps_lte : caps_nr;
    int nc = rat ? ncaps_lte : ncaps_nr, ex[MAX_LOCK_BANDS * 2], ne = 0;
    const struct carrier *c = rat ? dash.lte : dash.nr;
    int cn = !dash.ok ? 0 : rat ? dash.nlte : dash.nnr;
    for (int i = 0; i < cn; i++)                                   /* serving bands first */
        if (c[i].bandnum > 0 && !has_band(keep, nk, c[i].bandnum) && has_band(caps, nc, c[i].bandnum) && !has_band(ex, ne, c[i].bandnum))
            ex[ne++] = c[i].bandnum;
    for (int i = 0; i < nc; i++) if (!has_band(keep, nk, caps[i]) && !has_band(ex, ne, caps[i])) ex[ne++] = caps[i];
    if (!ne) { snprintf(out, len, "无"); return; }
    char b[160];
    if (ne <= 4) { band_list_text(rat, ex, ne, out, len); return; }
    band_list_text(rat, ex, 3, b, sizeof b);
    snprintf(out, len, "%s 等 %d 个", b, ne);
}
/* -- confirmation text (web confirmations.ts, in Chinese); built once when the dialog opens -- */
#define WAN_CONSEQUENCE "调制解调器会重连，移动上网可能断开几十秒；Wi-Fi 和本屏不受影响。"
static void build_dialog_locked(const struct lockop *op) {
    char cur[96], b[200], use[48] = "";
    const struct carrier *p = primary_locked();
    if (p) snprintf(use, sizeof use, " · 在用 %s", p->band);
    snprintf(dlg_warn, sizeof dlg_warn, "！网络会中断");
    dlg_warn_bad = 0;
    switch (op->kind) {
    case LK_BAND_NR: case LK_BAND_LTE: {
        int rat = op->kind == LK_BAND_LTE;
        band_list_text(rat, op->bands, op->nb, b, sizeof b);
        describe_lock_locked(rat, cur, sizeof cur);
        snprintf(dlg_title, sizeof dlg_title, "锁定 %s 频段？", rat ? "LTE" : "NR");
        snprintf(dlg_lead, sizeof dlg_lead, "只使用 %s", b);
        char ex[160];
        exclusion_text_locked(rat, op->bands, op->nb, ex, sizeof ex);
        snprintf(dlg_details, sizeof dlg_details, "操作  锁定 %s 频段\n只使用  %s\n排除  %s\n当前  %s%s", rat ? "LTE" : "NR (5G SA)", b, ex, cur, use);
        int hit = 0, any = 0, seen = 0;
        const struct carrier *c = rat ? dash.lte : dash.nr;
        int cn = !dash.ok ? 0 : rat ? dash.nlte : dash.nnr;
        for (int i = 0; i < cn; i++) { any = 1; if (has_band(op->bands, op->nb, c[i].bandnum)) hit = 1; }
        for (int i = 0; i < op->nb; i++) if (op->bands[i] > 0 && op->bands[i] < 1024 && seen_band[rat][op->bands[i]]) seen = 1;
        if (!hit && !seen) { snprintf(dlg_warn, sizeof dlg_warn, "！本机未见过该频段信号，可能断网"); dlg_warn_bad = 1; }
        char extra[96] = "";
        if (any && !hit) snprintf(extra, sizeof extra, "当前在用的 %s 不在所选范围内，切换后会重新搜网。", c[0].band);
        snprintf(dlg_cons, sizeof dlg_cons, WAN_CONSEQUENCE "若所选频段这里没有覆盖，会一直无服务，直到解除锁定。%s", extra);
        snprintf(dlg_recovery, sizeof dlg_recovery, "恢复方法：回到「锁频」页点「恢复自动」。");
        break;
    }
    case LK_BAND_RESET: {
        char nr[64], lte[64];
        describe_lock_locked(0, nr, sizeof nr);
        describe_lock_locked(1, lte, sizeof lte);
        snprintf(dlg_title, sizeof dlg_title, "恢复自动频段？");
        snprintf(dlg_lead, sizeof dlg_lead, "将清除频段锁定，回到自动选择。");
        snprintf(dlg_details, sizeof dlg_details, "操作  恢复自动频段\n影响  LTE 和 NR (5G SA) 频段锁定\n当前  NR %s · LTE %s", nr, lte);
        snprintf(dlg_cons, sizeof dlg_cons, WAN_CONSEQUENCE);
        snprintf(dlg_recovery, sizeof dlg_recovery, "如仍需限制频段，可在本页重新锁定。");
        break;
    }
    case LK_CELL_NR: case LK_CELL_LTE: {
        int nr = op->kind == LK_CELL_NR;
        snprintf(dlg_title, sizeof dlg_title, "锁定到这个小区？");
        snprintf(dlg_lead, sizeof dlg_lead, "将向调制解调器下发小区锁定。");
        if (nr) snprintf(dlg_details, sizeof dlg_details, "操作  锁定 NR (5G SA) 小区\nPCI  %d\nNR-ARFCN  %lld\n频段  n%d", op->pci, op->arfcn, op->bandnum);
        else snprintf(dlg_details, sizeof dlg_details, "操作  锁定 LTE 小区\nPCI  %d\nEARFCN  %lld", op->pci, op->arfcn);
        snprintf(dlg_cons, sizeof dlg_cons, WAN_CONSEQUENCE "若这个小区不可用，会一直无服务，直到解除锁定。");
        snprintf(dlg_recovery, sizeof dlg_recovery, "恢复方法：回到「锁小区」页点「解除锁定」。");
        break;
    }
    case LK_CELL_RESET:
        snprintf(dlg_title, sizeof dlg_title, "解除小区锁定？");
        snprintf(dlg_lead, sizeof dlg_lead, "将清除小区锁定，回到自动选择。");
        snprintf(dlg_details, sizeof dlg_details, "操作  解除小区锁定\n影响  LTE 和 NR 小区锁定");
        snprintf(dlg_cons, sizeof dlg_cons, WAN_CONSEQUENCE);
        snprintf(dlg_recovery, sizeof dlg_recovery, "如仍需锁定，可在候选列表里重新选择。");
        break;
    }
}
/* UI thread, lock held. Opens the dialog with a frozen snapshot. Refused while any op runs. */
static int lock_open_locked(const struct lockop *op) {
    if (busy_op != OP_NONE || lock_busy_locked() || op->kind == LK_NONE || st_state == SP_RUN) return 0;
    if ((op->kind == LK_BAND_NR || op->kind == LK_BAND_LTE) && op->nb <= 0) return 0;
    if ((op->kind == LK_CELL_NR || op->kind == LK_CELL_LTE) && (op->pci < 0 || op->arfcn <= 0)) return 0;
    lk_op = *op;
    lk_state = LS_CONFIRM;
    lk_token++;
    lk_confirmed = 0;
    lk_reason[0] = lk_readback[0] = 0;
    build_dialog_locked(op);
    generation++;
    return 1;
}
static int lock_cancel_locked(void) {
    if (lk_state != LS_CONFIRM) return 0;
    lk_state = LS_IDLE;
    generation++;
    return 1;
}
/* The ONLY way to arm a POST: the confirm button while the dialog is open. Idempotent. */
static int lock_confirm_locked(void) {
    if (lk_state != LS_CONFIRM || busy_op != OP_NONE) return 0;
    lk_confirmed = lk_token;
    lk_state = LS_SENDING;
    lk_state_at = now_ms();
    if (lk_op.kind == LK_BAND_NR || lk_op.kind == LK_BAND_LTE) {   /* the draft follows the submitted value */
        struct draft *dr = &drafts[lk_op.kind == LK_BAND_LTE];
        dr->edited = 1;
        dr->n = lk_op.nb;
        memcpy(dr->bands, lk_op.bands, sizeof(int) * (size_t)lk_op.nb);
    }
    generation++;
    wake_flag = 1;
    pthread_cond_signal(&wake_cond);
    return 1;
}
static int lock_dismiss_locked(void) {
    if (lk_state != LS_FAIL && lk_state != LS_SENT && lk_state != LS_OK) return 0;
    lk_state = LS_IDLE;
    generation++;
    return 1;
}
static void u128_dec(unsigned __int128 v, char *out, size_t len) {
    char t[48];
    int n = 0;
    do { t[n++] = (char)('0' + (int)(v % 10)); v /= 10; } while (v && n < 47);
    size_t k = 0;
    while (n && k + 1 < len) out[k++] = t[--n];
    out[k] = 0;
}
static void lock_request(const struct lockop *op, char *path, size_t plen, char *body, size_t blen) {
    body[0] = 0;
    switch (op->kind) {
    case LK_BAND_NR: {
        char list[200];
        size_t k = 0;
        list[0] = 0;
        for (int i = 0; i < op->nb && k + 8 < sizeof list; i++) k += (size_t)snprintf(list + k, sizeof list - k, "%s%d", i ? "," : "", op->bands[i]);
        snprintf(path, plen, "/api/cell/band/nr");
        snprintf(body, blen, "{\"nr5g_type\":\"SA\",\"nr5g_band\":\"%s\"}", list);
        break;
    }
    case LK_BAND_LTE: {
        unsigned __int128 mask = 0;
        for (int i = 0; i < op->nb; i++) if (op->bands[i] >= 1 && op->bands[i] <= 128) mask |= (unsigned __int128)1 << (op->bands[i] - 1);
        char dec[48];
        u128_dec(mask, dec, sizeof dec);
        snprintf(path, plen, "/api/cell/band/lte");
        snprintf(body, blen, "{\"is_lte_band\":\"1\",\"lte_band_mask\":\"%s\",\"is_gw_band\":\"0\",\"gw_band_mask\":\"0\"}", dec);
        break;
    }
    case LK_BAND_RESET: snprintf(path, plen, "/api/cell/band/reset"); break;
    case LK_CELL_NR:
        snprintf(path, plen, "/api/cell/lock/nr");
        snprintf(body, blen, "{\"lock_nr_pci\":\"%d\",\"lock_nr_earfcn\":\"%lld\",\"lock_nr_cell_band\":\"%d\"}", op->pci, op->arfcn, op->bandnum);
        break;
    case LK_CELL_LTE:
        snprintf(path, plen, "/api/cell/lock/lte");
        snprintf(body, blen, "{\"lock_lte_pci\":\"%d\",\"lock_lte_earfcn\":\"%lld\"}", op->pci, op->arfcn);
        break;
    case LK_CELL_RESET: snprintf(path, plen, "/api/cell/lock/reset"); break;
    default: path[0] = 0;
    }
}
enum { PR_OK, PR_REJECTED, PR_UNKNOWN, PR_NOT_SENT };
/* One POST, never retried. PR_UNKNOWN = request left but no (parseable) answer: the device may have applied it. */
static int lock_post(const char *path, const char *body, char *reason, size_t rlen) {
    char resp[2048];
    if (ensure_agent()) { snprintf(reason, rlen, "登录 agent 失败，未下发。"); return PR_NOT_SENT; }
    int st = http_req(agent_port, "POST", path, body, agent_token, lock_timeout_ms, resp, sizeof resp, 0);
    if (st < 0) {
        if (!http_last_sent) { snprintf(reason, rlen, "连不上 agent（%s:%d），未下发。", api_host, agent_port); return PR_NOT_SENT; }
        snprintf(reason, rlen, "agent %d 秒内没有应答，设备可能已执行。", lock_timeout_ms / 1000);
        return PR_UNKNOWN;
    }
    if (st == 401 || st == 403) { agent_token[0] = 0; snprintf(reason, rlen, "agent 登录已过期，未下发；请重试。"); return PR_REJECTED; }
    int ok = jbool(jget(resp, "ok"));
    if (ok2xx(st) && ok == 1) return PR_OK;
    if (ok2xx(st) && ok < 0) { snprintf(reason, rlen, "agent 返回 HTTP %d 但响应无法解析。", st); return PR_UNKNOWN; }
    char err[160];
    jstr(jget(resp, "error"), err, sizeof err);
    snprintf(reason, rlen, "agent 拒绝（HTTP %d）%s%s", st, err[0] ? "：" : "。", err);
    return PR_REJECTED;
}
static int cell_matches_locked(const struct lockop *op) {
    const struct carrier *c = pcc_of_locked(op->kind == LK_CELL_NR ? RAT_NR : RAT_LTE);
    return dash.ok && c && c->pci == op->pci && c->arfcn == op->arfcn;
}
/* worker: runs the confirmed snapshot once. Returns 1 if a POST was made. */
static int lock_step(void) {
    struct lockop op;
    pthread_mutex_lock(&lock);
    int go = lk_state == LS_SENDING && lk_confirmed && lk_confirmed == lk_token;
    if (go) { op = lk_op; lk_confirmed = 0; }       /* consumed: one POST per confirm tap */
    pthread_mutex_unlock(&lock);
    if (!go) return 0;
    char path[48], body[320], reason[256] = "";
    lock_request(&op, path, sizeof path, body, sizeof body);
    int r = path[0] ? lock_post(path, body[0] ? body : "", reason, sizeof reason) : PR_REJECTED;
    pthread_mutex_lock(&lock);
    int64_t t = now_ms();
    snprintf(lk_reason, sizeof lk_reason, "%s", reason);
    lk_polls = 0;
    lk_readback[0] = 0;
    lk_state_at = t;
    if (op.kind == LK_CELL_NR || op.kind == LK_CELL_LTE) {
        lk_preset = cell_matches_locked(&op);
        if (r == PR_OK || r == PR_UNKNOWN) snprintf(cell_last_req, sizeof cell_last_req, "%s PCI %d · %lld", op.kind == LK_CELL_NR ? "NR" : "LTE", op.pci, op.arfcn);
    }
    if (r == PR_OK && op.kind == LK_CELL_RESET) {
        lk_state = LS_SENT;                        /* nothing to read back: say so instead of claiming success */
        cell_last_req[0] = 0;
    } else if (r == PR_OK || (r == PR_UNKNOWN && op.kind != LK_CELL_RESET)) {
        lk_state = LS_VERIFY;
        lk_next_poll = t + verify_every_ms;
    } else {
        lk_state = LS_FAIL;
        if (r == PR_UNKNOWN) snprintf(lk_reason, sizeof lk_reason, "%s设备不返回小区锁定状态，结果未知。", reason);
    }
    generation++;
    pthread_mutex_unlock(&lock);
    return 1;
}
/* readback predicate for the current dash; fills `seen` with what the device reports. 1 = confirmed. */
static int lock_matches_locked(const struct lockop *op, char *seen, size_t len) {
    if (!dash.ok) { snprintf(seen, len, "暂无数据（重连中属正常）"); return 0; }
    switch (op->kind) {
    case LK_BAND_NR: case LK_BAND_LTE: {
        int rat = op->kind == LK_BAND_LTE;
        const struct bandlock *l = observed_lock_locked(rat);
        char d[96];
        describe_lock_locked(rat, d, sizeof d);
        snprintf(seen, len, "%s %s", rat ? "LTE" : "NR", d);
        return l->kind == LK_LOCKED && same_bands(l->bands, l->n, op->bands, op->nb);   /* draft.ts bandSpec.confirms */
    }
    case LK_BAND_RESET: {
        char a[64], b[64];
        describe_lock_locked(0, a, sizeof a);
        describe_lock_locked(1, b, sizeof b);
        snprintf(seen, len, "NR %s · LTE %s", a, b);
        return (dash.sa_lock.kind == LK_AUTO || lock_is_all_locked(0)) && (dash.lte_lock.kind == LK_AUTO || lock_is_all_locked(1));
    }
    case LK_CELL_NR: case LK_CELL_LTE: {
        const struct carrier *c = pcc_of_locked(op->kind == LK_CELL_NR ? RAT_NR : RAT_LTE);
        if (c) snprintf(seen, len, "主小区 PCI %d · %lld", c->pci, c->arfcn);
        else snprintf(seen, len, "没有 %s 主小区", op->kind == LK_CELL_NR ? "NR" : "LTE");
        return cell_matches_locked(op);
    }
    }
    return 0;
}
/* worker: one readback poll (the caller does the 2 s pacing). */
static void verify_step(void) {
    refresh_dash();
    pthread_mutex_lock(&lock);
    if (lk_state == LS_VERIFY) {
        int64_t t = now_ms();
        lk_polls++;
        if (lock_matches_locked(&lk_op, lk_readback, sizeof lk_readback)) {
            lk_state = LS_OK;
            lk_state_at = t;
            if (lk_op.kind == LK_BAND_NR || lk_op.kind == LK_BAND_LTE) drafts[lk_op.kind == LK_BAND_LTE].edited = 0;
            if (lk_op.kind == LK_BAND_RESET) drafts[0].edited = drafts[1].edited = 0;
        } else if (lk_polls >= VERIFY_POLLS) {
            lk_state = LS_FAIL;
            lk_state_at = t;
            snprintf(lk_reason, sizeof lk_reason, "读回 %d 秒后仍为「%s」，没有生效。接口已返回成功，但设备状态未变化。",
                     VERIFY_POLLS * verify_every_ms / 1000, lk_readback);
        } else lk_next_poll = t + verify_every_ms;
        generation++;
    }
    pthread_mutex_unlock(&lock);
}

static int dash_page(int pg) { return pg == PG_OVERVIEW || pg == PG_BAND || pg == PG_CELL; }
#define POLL_FAST 2000
#define POLL_SLOW 10000
static void *worker(void *unused) {
    (void)unused;
    int64_t due_proxy = 0, due_dash = 0, due_clients = 0, due_caps = 0;
    int last_vp = -1;
    for (;;) {
        int ran = apply_pending();
        int sent = lock_step();
        int64_t t = now_ms();
        pthread_mutex_lock(&lock);
        int vp = visible_page, ls = lk_state;
        int64_t np = lk_next_poll;
        pthread_mutex_unlock(&lock);
        int paused = modal_on();                  /* screen off / stock popup: no background polling */
        if (vp != last_vp) {                       /* page change: fetch what the new page shows now */
            if (vp == PG_PROXY) due_proxy = t;
            if (dash_page(vp)) due_dash = t;
            last_vp = vp;
        }
        if (ran || (!paused && t >= due_proxy)) { refresh_state(); due_proxy = now_ms() + (vp == PG_PROXY ? POLL_FAST : POLL_SLOW); }
        if (ls == LS_VERIFY) {
            if (t >= np) { verify_step(); due_dash = now_ms() + POLL_FAST; }
        } else if (sent || (!paused && t >= due_dash)) {
            refresh_dash();
            due_dash = now_ms() + (dash_page(vp) ? POLL_FAST : POLL_SLOW);
        }
        if (!paused && t >= due_clients) { refresh_clients(); due_clients = now_ms() + POLL_SLOW; }
        if (!paused && !caps_ok && t >= due_caps) { refresh_caps(); due_caps = now_ms() + POLL_SLOW; }
        pthread_mutex_lock(&lock);
        if (ran) { busy_op = OP_NONE; busy_mode = busy_tun = -1; busy_name[0] = 0; generation++; }
        int64_t next = due_proxy;
        if (lk_state == LS_VERIFY) { if (lk_next_poll < next) next = lk_next_poll; }
        else if (due_dash < next) next = due_dash;
        if (due_clients < next) next = due_clients;
        if (!caps_ok && due_caps < next) next = due_caps;
        int64_t wait = next - now_ms();
        if (wait < 20) wait = 20;
        if (wait > 2000) wait = 2000;
        struct timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_sec += wait / 1000;
        until.tv_nsec += (wait % 1000) * 1000000;
        if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
        while (!wake_flag) if (pthread_cond_timedwait(&wake_cond, &lock, &until) == ETIMEDOUT) break;
        wake_flag = 0;
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* ---- navigation: 6 pages (stock 1-2, then 概览 / 代理 / 锁频 / 锁小区) ---- */
static void *chip_scroll, *cell_list;      /* vertical lists: a drag inside never flips the page */
static void render_all(void);
static void pos_raw(void *o, int x, int y) { F(0x537450, void, void *, int, int)(o, x, y); }
static void update_dots(void *form, int page) {
    if (form != mainform || !dots[PAGE_COUNT - 1]) { F(0x4bc1d0, void, void *, int)(form, page); return; }
    current = page;
    pthread_mutex_lock(&lock);
    if (visible_page != page) { visible_page = page; wake_flag = 1; pthread_cond_signal(&wake_cond); }
    pthread_mutex_unlock(&lock);
    /* dots[0..1] are stock objects: raw setters only (no per-object cache on them) */
    for (int i = 0; i < PAGE_COUNT; i++) {
        int active = i == page - 1;
        flag_raw(dots[i], HIDDEN, 0);
        pos_raw(dots[i], 160 - 8 * PAGE_COUNT + 16 * i + (active ? 3 : 4), active ? 439 : 440);
        size(dots[i], active ? 8 : 6, active ? 8 : 6);
        F(0x537868, void, void *, int)(dots[i], active ? 4 : 3);
        color(dots[i], active ? 168 : 172, 0);
    }
}
static void goto_page(int page, int anim) {
    if (!mainform) return;
    if (page < 1) page = 1;
    if (page > page_count) page = page_count;
    *((unsigned char *)mainform + 840) = (unsigned char)page;
    update_dots(mainform, page);
    F(0x538080, void, void *, int, int)(P(mainform, 88), (page - 1) * 320, anim);
}
static void scroll_release(void *form) {
    if (form != mainform) { F(0x4bd9ec, void, void *)(form); return; }
    void *indev = F(0x42a8c0, void *, void)();
    if (!indev) return;
    void *scrolling = F(0x42ad14, void *, void *)(indev);
    if ((F(0x42ace8, unsigned, void *)(indev) & 12) || (list_scroll && scrolling == P(list_scroll, 8)) ||
        (chip_scroll && scrolling == P(chip_scroll, 8)) || (cell_list && scrolling == P(cell_list, 8))) return;
    *(uint32_t *)((char *)indev + 36) = 0;
    int x = (int16_t)F(0x5384a0, int, void *)(P(form, 88));
    int old = *((unsigned char *)form + 840);
    pthread_mutex_lock(&lock);
    int dialog = lk_state == LS_CONFIRM;
    pthread_mutex_unlock(&lock);
    if (dialog) { goto_page(old, 1); return; }      /* the confirm dialog pins the page */
    int delta = x - (old - 1) * 320;
    goto_page(old + (delta > 24) - (delta < -24), 1);
}
#define PENDING_TEXT "切换中..."
static int mode_index(const char *m) { return !strcmp(m, "global") ? 1 : !strcmp(m, "direct") ? 2 : 0; }
/* Everything render() puts on screen, computed under the lock without any LVGL call
 * (so the format is unit-testable); render() then only applies it on the UI thread. */
enum { ST_NORMAL, ST_SELECTED, ST_PENDING };
struct view {
    char node_text[NAME_MAX + 24], state_text[192];
    int state_tone;                                   /* 0 normal, 1 busy (amber), 2 failure (red), 3 notice */
    char mode_text[3][24]; int mode_style[3];
    int rows; char row_text[LIST_ROWS][NAME_MAX + 32]; int row_style[LIST_ROWS], row_tone[LIST_ROWS];
    char tun_text[64]; int tun_style;                 /* 0 off, 1 on, 2 pending */
    char test_text[24]; int test_style;               /* ST_NORMAL / ST_PENDING */
};
static void build_view(struct view *v) {
    static const char *const mode_names[3] = {"规则", "全局", "直连"};
    char d[24];
    pthread_mutex_lock(&lock);
    int ok = have_state, bop = busy_op;
    int failed = bop == OP_NONE && note[0] && now_ms() < note_until;
    if (node_index >= 0 && node_index < node_count) {
        delay_text(delay_lookup_locked(nodes[node_index]), d, sizeof d);
        if (d[0]) snprintf(v->node_text, sizeof v->node_text, "%s  %s", nodes[node_index], d);
        else snprintf(v->node_text, sizeof v->node_text, "%s", nodes[node_index]);
    } else snprintf(v->node_text, sizeof v->node_text, "%s", ok ? "未选择" : "—");
    if (bop == OP_TEST) snprintf(v->state_text, sizeof v->state_text, "%s", TXT_TESTING);
    else if (bop != OP_NONE) snprintf(v->state_text, sizeof v->state_text, "%s", PENDING_TEXT);
    else if (failed) snprintf(v->state_text, sizeof v->state_text, "%s", note);
    else snprintf(v->state_text, sizeof v->state_text, "%s", status[0] ? status : (tun_on ? "运行中 · 透明代理开" : "运行中 · 透明代理关"));
    v->state_tone = bop != OP_NONE ? 1 : failed ? (note_soft ? 3 : 2) : 0;
    int active_mode = mode_index(mode_name);
    for (int i = 0; i < 3; i++) {
        int pending = bop == OP_MODE && busy_mode == i;
        v->mode_style[i] = pending ? ST_PENDING : i == active_mode ? ST_SELECTED : ST_NORMAL;
        snprintf(v->mode_text[i], sizeof v->mode_text[i], "%s", pending ? PENDING_TEXT : mode_names[i]);
    }
    v->rows = node_count < LIST_ROWS ? node_count : LIST_ROWS;
    for (int i = 0; i < v->rows; i++) {
        int pending = bop == OP_NODE && !strcmp(nodes[i], busy_name);
        int ms = delay_lookup_locked(nodes[i]);
        v->row_style[i] = pending ? ST_PENDING : i == node_index ? ST_SELECTED : ST_NORMAL;
        v->row_tone[i] = delay_tone(ms);
        if (pending) snprintf(v->row_text[i], sizeof v->row_text[i], PENDING_TEXT " %s", nodes[i]);
        else row_text(nodes[i], ms, v->row_text[i], sizeof v->row_text[i]);
    }
    v->tun_style = bop == OP_TUN ? 2 : tun_on ? 1 : 0;
    snprintf(v->tun_text, sizeof v->tun_text, "%s", v->tun_style == 2 ? "透明代理    " PENDING_TEXT : tun_on ? "透明代理    已开启" : "透明代理    已关闭");
    v->test_style = bop == OP_TEST ? ST_PENDING : ST_NORMAL;
    snprintf(v->test_text, sizeof v->test_text, "%s", bop == OP_TEST ? TXT_TESTING : TXT_TEST_ALL);
    pthread_mutex_unlock(&lock);
}
static void render_proxy(void) {
    static struct view v;                 /* UI thread only (~15 KB, kept off the stack) */
    static const unsigned char tone_rgb[4][3] = {{232, 241, 242}, {120, 220, 140}, {250, 200, 90}, {255, 120, 110}};
    build_view(&v);
    paint(page, 8, 20, 40, 0);
    paint(title, 232, 241, 242, 1);
    if (v.state_tone == 1) paint(state_label, 250, 200, 90, 1);        /* amber: in flight */
    else if (v.state_tone == 2) paint(state_label, 255, 120, 110, 1);  /* red: failure */
    else if (v.state_tone == 3) paint(state_label, 245, 215, 140, 1);  /* soft amber: notice, not a failure */
    else paint(state_label, 154, 176, 204, 1);
    paint(node, 232, 241, 242, 1);
    text(title, "代理");
    text(node, v.node_text);
    text(state_label, v.state_text);
    for (int i = 0; i < 3; i++) {
        int st = v.mode_style[i], hi = st != ST_NORMAL;
        text(mode_captions[i], v.mode_text[i]);
        if (st == ST_PENDING) fill(mode_buttons[i], 150, 105, 20, 150);
        else fill(mode_buttons[i], hi ? 47 : 16, hi ? 128 : 36, hi ? 237 : 62, hi ? 105 : 66);
        paint(mode_captions[i], hi ? 255 : 154, hi ? 255 : 176, hi ? 255 : 204, 1);
    }
    if (test_button) {
        int busy = v.test_style == ST_PENDING;
        text(test_caption, v.test_text);
        if (busy) fill(test_button, 150, 105, 20, 150);
        else fill(test_button, 16, 36, 62, 66);
        paint(test_caption, busy ? 255 : 154, busy ? 255 : 176, busy ? 255 : 204, 1);
    }
    paint(tun_label, 255, 255, 255, 1);
    fill(list_scroll, 10, 28, 48, 31);
    for (int i = 0; i < LIST_ROWS; i++) {
        int visible = i < v.rows;
        flag(row_buttons[i], HIDDEN, !visible);
        flag(row_buttons[i], CLICKABLE, visible);
        if (!visible) continue;
        int st = v.row_style[i], selected = st == ST_SELECTED;
        if (st == ST_PENDING) {
            if (bg_on) fill(row_buttons[i], 150, 105, 20, 150);
            else paint(row_buttons[i], 150, 105, 20, 0);
        } else if (bg_on) fill(row_buttons[i], selected ? 47 : (i % 2 ? 20 : 14), selected ? 128 : (i % 2 ? 48 : 32), selected ? 237 : (i % 2 ? 79 : 54), selected ? 102 : (i % 2 ? 56 : 38));
        else paint(row_buttons[i], selected ? 47 : (i % 2 ? 20 : 14), selected ? 128 : (i % 2 ? 44 : 32), selected ? 237 : (i % 2 ? 74 : 54), 0);
        if (st != ST_NORMAL) paint(row_captions[i], 255, 255, 255, 1);
        else { const unsigned char *c = tone_rgb[v.row_tone[i]]; paint(row_captions[i], c[0], c[1], c[2], 1); }
        text(row_captions[i], v.row_text[i]);
    }
    text(tun_label, v.tun_text);
    if (bg_on) {
        if (v.tun_style == 2) fill(tun_button, 150, 105, 20, 150);
        else fill(tun_button, v.tun_style ? 12 : 16, v.tun_style ? 110 : 36, v.tun_style ? 190 : 62, v.tun_style ? 87 : 66);
        fill(tun_caption, 0, 0, 0, 0);  /* caption label fully transparent, button shows through */
    } else {
        if (v.tun_style == 2) paint(tun_caption, 150, 105, 20, 0);
        else paint(tun_caption, v.tun_style ? 12 : 16, v.tun_style ? 110 : 36, v.tun_style ? 190 : 62, 0);
    }
}
/* Queues one op and wakes the worker at once. Taps while an op is in flight are ignored
 * (no queue of stale taps); tapping the mode/node that is already active does nothing. */
static int queue_tap(int id) {
    int accepted = 0;
    pthread_mutex_lock(&lock);
    if (busy_op == OP_NONE && lk_state != LS_CONFIRM && lk_state != LS_SENDING) {   /* no proxy change under an open dialog */
        if (id >= 10 && id <= 12) {
            if (!have_state || id - 10 != mode_index(mode_name)) {
                busy_op = OP_MODE; busy_mode = id - 10; accepted = 1;
            }
        } else if (id >= 100 && id < 100 + LIST_ROWS) {
            int i = id - 100;
            if (i < node_count && i != node_index) {
                /* capture name + group now: the list may be refreshed before the worker runs */
                snprintf(busy_name, sizeof busy_name, "%s", nodes[i]);
                snprintf(busy_group, sizeof busy_group, "%s", group_name);
                busy_op = OP_NODE; accepted = 1;
            }
        } else if (id == 3) {
            busy_op = OP_TUN; busy_tun = !tun_on; accepted = 1;
        } else if (id == 4) {
            if (have_state && node_count > 0) {
                snprintf(busy_group, sizeof busy_group, "%s", group_name);
                test_count = node_count;
                for (int i = 0; i < node_count; i++) memcpy(test_names[i], nodes[i], NAME_MAX);
                busy_op = OP_TEST; accepted = 1;
            }
        }
        if (accepted) { note[0] = 0; wake_flag = 1; pthread_cond_signal(&wake_cond); }
    }
    pthread_mutex_unlock(&lock);
    return accepted;
}
/* LVGL thread: shows the pending state right away (render() reuses the existing labels). */

/* ==== views of the new pages: pure, built under the lock, unit-tested; render_*() only applies them ==== */
enum { T_TEXT, T_GOOD, T_WARN, T_BAD, T_DIM, T_NR, T_GREY, T_BLUE };
static const unsigned char tone_rgbs[8][3] = {
    {232, 241, 242}, {61, 214, 140}, {242, 181, 68}, {240, 96, 90}, {154, 176, 204}, {183, 156, 255}, {110, 124, 140}, {120, 180, 255}};
static const char *const rate_words[4] = {"优秀", "良好", "一般", "差"};
/* web-app signalQuality.ts: inclusive lower edges, same for LTE and NR */
static int rate_level(double v, double ex, double gd, double fr) { return v >= ex ? 0 : v >= gd ? 1 : v >= fr ? 2 : 3; }
static int rsrp_level(double v) { return rate_level(v, -80, -90, -100); }
static int sinr_level(double v) { return rate_level(v, 20, 10, 0); }
static int level_tone(int lvl) { return lvl <= 1 ? T_GOOD : lvl == 2 ? T_WARN : T_BAD; }
static void fmt_val(double v, const char *unit, char *o, size_t n) {
    if (v == (double)(long long)v) snprintf(o, n, "%lld %s", (long long)v, unit);
    else snprintf(o, n, "%.1f %s", v, unit);
}
static void fmt_rate(double Bps, char *o, size_t n) {
    double m = Bps * 8 / 1e6;
    snprintf(o, n, m < 10 ? "%.2f Mbps" : "%.1f Mbps", m);
}
static void fmt_bytes(double b, char *o, size_t n) {
    if (b >= 1073741824.0) snprintf(o, n, "%.2f GB", b / 1073741824.0);
    else if (b >= 1048576.0) snprintf(o, n, "%.1f MB", b / 1048576.0);
    else snprintf(o, n, "%.0f KB", b / 1024.0);
}
static const char *mode_label(int m, const char *raw) {
    return m == NM_SA ? "5G SA" : m == NM_NSA ? "5G NSA" : m == NM_LTE ? "4G LTE" : raw[0] ? raw : "—";
}
static void pci_text(int pci, char *o, size_t n) { if (pci >= 0) snprintf(o, n, "%d", pci); else snprintf(o, n, "—"); }

/* ==== 测速 (overview): download + latency over the cellular link itself ====================
 * Runs on its own thread (never the LVGL thread, never the proxy/lock worker so polling and lock
 * readback keep going). One test at a time; interruptible; 30 s cooldown; results kept in memory.
 *
 * Proxy bypass: mihomo's TUN policy rules send locally generated traffic whose source is still
 * 0.0.0.0 into table 2022 (rule "from 0.0.0.0 iif lo lookup 2022" -> dev mihomo), and its DNS
 * hijack may answer with fake-ip 198.18.x.x. So every socket here (DNS, latency, download) is
 * (1) SO_BINDTODEVICE'd to the interface that holds the MAIN-table default route (rmnet_data0 on
 *     the U60; read from /proc/net/route, which only shows the main table, skipping mihomo/tun/lo),
 * (2) bind()'ed to that interface's own IPv4 address (so the 0.0.0.0-source rule cannot match), and
 * (3) names are resolved by our own UDP query, also bound to the WAN, to the carrier's resolvers
 *     from /etc/resolv.conf (then 223.5.5.5); answers inside 198.18.0.0/15 are refused.
 * If the interface cannot be found or bound, the test fails instead of silently using the proxy. */
#include <net/if.h>
#include <sys/ioctl.h>
#include <poll.h>
#define ST_HIST 5
#define ST_COOLDOWN_MS 30000
static const char *const st_urls_default[] = {
    "http://mirrors.aliyun.com/ubuntu/dists/noble/Contents-amd64.gz",        /* 51.3 MB, frozen since 2024-04 */
    "http://mirrors.huaweicloud.com/ubuntu/dists/noble/Contents-amd64.gz",   /* same file, Huawei Cloud CDN */
    NULL,
};
static const char *const *st_urls = st_urls_default;
static const char *st_ping_ip = "223.5.5.5";      /* AliDNS anycast; TCP 53 handshake = latency */
static int st_ping_port = 53, st_dns_port = 53;
static const char *st_route_path = "/proc/net/route", *st_resolv_path = "/etc/resolv.conf";
static const char *st_iface_override;             /* tests only ("lo") */
static int st_max_ms = 10000, st_cooldown_ms = ST_COOLDOWN_MS;
static long long st_max_bytes = 50LL * 1000 * 1000;
struct st_result { int64_t wall; double mbps; int ping_ms, ms, stopped; long long bytes; char radio[96], host[64]; };
static struct st_result st_hist[ST_HIST];          /* [0] newest */
static int st_nhist, st_req, st_cancel, st_ping, st_elapsed, st_kept;   /* st_kept: last run added a result */
static int64_t st_cool_until;
static long long st_bytes;
static double st_live;
static char st_phase[64], st_err[128];
static pthread_cond_t st_cond = PTHREAD_COND_INITIALIZER;

static int st_cancelled(void) { pthread_mutex_lock(&lock); int c = st_cancel; pthread_mutex_unlock(&lock); return c; }
static void st_set_phase(const char *p) { pthread_mutex_lock(&lock); snprintf(st_phase, sizeof st_phase, "%s", p); generation++; pthread_mutex_unlock(&lock); }

/* interface of the main-table default route (not mihomo / tun / lo) and its IPv4 address */
static int st_wan(char *ifn, size_t n, struct in_addr *src) {
    if (st_iface_override) { snprintf(ifn, n, "%s", st_iface_override); inet_pton(AF_INET, "127.0.0.1", src); return 1; }
    FILE *f = fopen(st_route_path, "r");
    if (!f) return 0;
    char line[256], name[IFNAMSIZ + 1];
    unsigned dst, gw, flags, mask;
    int found = 0;
    if (!fgets(line, sizeof line, f)) { fclose(f); return 0; }
    while (!found && fgets(line, sizeof line, f)) {
        if (sscanf(line, "%16s %x %x %x %*d %*d %*d %x", name, &dst, &gw, &flags, &mask) != 5) continue;
        if (dst || mask || !(flags & 1)) continue;                              /* default route, RTF_UP */
        if (!strcmp(name, "lo") || !strncmp(name, "mihomo", 6) || !strncmp(name, "tun", 3) || !strncmp(name, "utun", 4)) continue;
        snprintf(ifn, n, "%s", name);
        found = 1;
    }
    fclose(f);
    if (!found) return 0;
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof ifr);
    snprintf(ifr.ifr_name, sizeof ifr.ifr_name, "%s", ifn);
    int ok = ioctl(fd, SIOCGIFADDR, &ifr) == 0;
    close(fd);
    if (!ok) return 0;
    *src = ((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr;
    return src->s_addr != 0;
}
static int st_socket(int type, const char *ifn, struct in_addr src) {
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr = src};
    if (setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifn, (socklen_t)strlen(ifn) + 1) || bind(fd, (struct sockaddr *)&a, sizeof a)) {
        close(fd);
        return -1;
    }
    return fd;
}
static int st_fake_ip(struct in_addr a) { uint32_t h = ntohl(a.s_addr); return (h & 0xfffe0000u) == 0xc6120000u || h == 0; }   /* 198.18/15, 0.0.0.0 */
/* plain A query over a WAN-bound UDP socket; 1 = got an address */
static int st_resolve(const char *host, const char *ifn, struct in_addr src, struct in_addr *out) {
    if (inet_pton(AF_INET, host, out) == 1) return !st_fake_ip(*out);
    char servers[4][48]; int ns = 0;
    FILE *f = fopen(st_resolv_path, "r");
    if (f) {
        char line[160], ip[48];
        struct in_addr t;
        while (ns < 3 && fgets(line, sizeof line, f))
            if (sscanf(line, "nameserver %47s", ip) == 1 && inet_pton(AF_INET, ip, &t) == 1) snprintf(servers[ns++], 48, "%s", ip);
        fclose(f);
    }
    if (ns < 4) snprintf(servers[ns++], 48, "223.5.5.5");
    unsigned char q[300], r[1500];
    size_t qn = 12;
    uint16_t id = (uint16_t)(now_ms() ^ 0x5a5a);
    memset(q, 0, 12);
    q[0] = (unsigned char)(id >> 8); q[1] = (unsigned char)id; q[2] = 1; q[5] = 1;     /* RD, QDCOUNT 1 */
    for (const char *p = host; *p;) {
        const char *dot = strchr(p, '.');
        size_t l = dot ? (size_t)(dot - p) : strlen(p);
        if (!l || l > 63 || qn + l + 6 > sizeof q) return 0;
        q[qn++] = (unsigned char)l; memcpy(q + qn, p, l); qn += l;
        p += l + (dot ? 1 : 0);
    }
    q[qn++] = 0; q[qn++] = 0; q[qn++] = 1; q[qn++] = 0; q[qn++] = 1;                  /* A, IN */
    for (int s = 0; s < ns; s++) {
        if (st_cancelled()) return 0;
        int fd = st_socket(SOCK_DGRAM, ifn, src);
        if (fd < 0) return 0;
        struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons((uint16_t)st_dns_port)};
        inet_pton(AF_INET, servers[s], &to.sin_addr);
        struct timeval tv = {2, 0};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        ssize_t n = -1;
        if (sendto(fd, q, qn, 0, (struct sockaddr *)&to, sizeof to) == (ssize_t)qn) n = recv(fd, r, sizeof r, 0);
        close(fd);
        if (n < 12 || r[0] != q[0] || r[1] != q[1] || (r[3] & 15)) continue;
        int an = r[6] << 8 | r[7];
        size_t o = qn;                                                               /* answers follow our question */
        if ((size_t)n < o) continue;
        for (int i = 0; i < an && o < (size_t)n; i++) {
            while (o < (size_t)n && r[o] && (r[o] & 0xc0) != 0xc0) o += (size_t)r[o] + 1;
            o += o < (size_t)n && r[o] ? 2 : 1;
            if (o + 10 > (size_t)n) break;
            int type = r[o] << 8 | r[o + 1], rdl = r[o + 8] << 8 | r[o + 9];
            o += 10;
            if (o + (size_t)rdl > (size_t)n) break;
            if (type == 1 && rdl == 4) {
                memcpy(&out->s_addr, r + o, 4);
                if (!st_fake_ip(*out)) return 1;
            }
            o += (size_t)rdl;
        }
    }
    return 0;
}
static int st_connect(struct in_addr ip, int port, const char *ifn, struct in_addr src, int timeout_ms) {
    int fd = st_socket(SOCK_STREAM, ifn, src);
    if (fd < 0) return -2;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr = ip};
    if (connect(fd, (struct sockaddr *)&a, sizeof a) && errno != EINPROGRESS) { close(fd); return -1; }
    int64_t end = now_ms() + timeout_ms;
    for (;;) {
        struct pollfd p = {fd, POLLOUT, 0};
        int left = (int)(end - now_ms());
        if (left <= 0 || st_cancelled()) { close(fd); return -1; }
        int r = poll(&p, 1, left < 200 ? left : 200);
        if (r > 0) {
            int e = 0; socklen_t el = sizeof e;
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el);
            if (e) { close(fd); return -1; }
            return fd;
        }
    }
}
/* latency: median TCP handshake time to st_ping_ip (3 tries), -1 if none answered */
static int st_latency(const char *ifn, struct in_addr src) {
    struct in_addr ip;
    if (inet_pton(AF_INET, st_ping_ip, &ip) != 1) return -1;
    int v[3], n = 0;
    for (int i = 0; i < 3 && !st_cancelled(); i++) {
        int64_t t0 = now_ms();
        int fd = st_connect(ip, st_ping_port, ifn, src, 2000);
        if (fd >= 0) { v[n++] = (int)(now_ms() - t0); close(fd); }
    }
    if (!n) return -1;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (v[j] < v[i]) { int t = v[i]; v[i] = v[j]; v[j] = t; }
    return v[n / 2];
}
static int st_parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn) {
    if (strncmp(url, "http://", 7)) return 0;                                       /* plain HTTP only */
    const char *h = url + 7, *slash = strchr(h, '/'), *end = slash ? slash : h + strlen(h);
    const char *colon = memchr(h, ':', (size_t)(end - h));
    size_t l = (size_t)((colon ? colon : end) - h);
    if (!l || l >= hn) return 0;
    memcpy(host, h, l); host[l] = 0;
    *port = colon ? atoi(colon + 1) : 80;
    snprintf(path, pn, "%s", slash ? slash : "/");
    return *port > 0 && *port < 65536;
}
static char st_buf[65536];                         /* speed thread only */
/* one URL (following up to 3 http redirects). 1 = body bytes were measured, 0 = failed before the body. */
static int st_download(const char *url0, const char *ifn, struct in_addr src, struct st_result *res) {
    char url[512], host[128], path[384];
    int port;
    snprintf(url, sizeof url, "%s", url0);
    for (int hop = 0; hop < 4; hop++) {
        if (!st_parse_url(url, host, sizeof host, &port, path, sizeof path)) { snprintf(st_err, sizeof st_err, "地址无效"); return 0; }
        if (hop == 0) snprintf(res->host, sizeof res->host, "%s", host);
        st_set_phase("解析域名…");
        struct in_addr ip;
        if (!st_resolve(host, ifn, src, &ip)) { snprintf(st_err, sizeof st_err, "解析 %s 失败", host); return 0; }
        st_set_phase("连接中…");
        int fd = st_connect(ip, port, ifn, src, 4000);
        if (fd < 0) { snprintf(st_err, sizeof st_err, "连接 %s 失败", host); return 0; }
        char req[700];
        int rl = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: u60-screen-speedtest\r\nAccept: */*\r\nConnection: close\r\n\r\n", path, host);
        if (send(fd, req, (size_t)rl, MSG_NOSIGNAL) != rl) { close(fd); snprintf(st_err, sizeof st_err, "发送请求失败"); return 0; }
        /* headers (5 s) */
        size_t got = 0;
        char *eoh = NULL;
        int64_t hend = now_ms() + 5000;
        while (!eoh && got < 8191) {
            struct pollfd p = {fd, POLLIN, 0};
            int left = (int)(hend - now_ms());
            if (left <= 0 || st_cancelled()) break;
            if (poll(&p, 1, left < 200 ? left : 200) <= 0) continue;
            ssize_t n = recv(fd, st_buf + got, 8191 - got, 0);
            if (n <= 0) break;
            got += (size_t)n; st_buf[got] = 0;
            eoh = strstr(st_buf, "\r\n\r\n");
        }
        if (!eoh) { close(fd); snprintf(st_err, sizeof st_err, "%s 没有应答", host); return 0; }
        int status = 0;
        sscanf(st_buf, "HTTP/1.%*d %d", &status);
        if (status >= 301 && status <= 308 && status != 304) {
            char *loc = NULL;
            for (char *h = st_buf; h < eoh && !loc; h++) if (h[0] == '\r' && h[1] == '\n' && !strncasecmp(h + 2, "Location:", 9)) loc = h;
            close(fd);
            if (!loc || loc > eoh) { snprintf(st_err, sizeof st_err, "HTTP %d 无跳转地址", status); return 0; }
            loc += 11;
            while (*loc == ' ') loc++;
            size_t l = strcspn(loc, "\r\n");
            if (l >= sizeof url) l = sizeof url - 1;
            memmove(url, loc, l); url[l] = 0;
            continue;
        }
        if (status != 200 && status != 206) { close(fd); snprintf(st_err, sizeof st_err, "HTTP %d", status); return 0; }
        /* body: measure from the first body byte; stop at st_max_ms, st_max_bytes, cancel or EOF */
        st_set_phase("下载中");
        long long bytes = (long long)(got - (size_t)(eoh + 4 - st_buf));
        int64_t t0 = now_ms(), last_ui = t0;
        int64_t wt[8]; long long wb[8]; int wn = 0;
        int stopped = 0;
        for (;;) {
            int64_t t = now_ms();
            if (t - t0 >= st_max_ms || bytes >= st_max_bytes) break;
            if (st_cancelled()) { stopped = 1; break; }
            struct pollfd p = {fd, POLLIN, 0};
            int pr = poll(&p, 1, 200);
            if (pr < 0) break;
            if (pr > 0) {
                ssize_t n = recv(fd, st_buf, sizeof st_buf, 0);
                if (n <= 0) break;
                bytes += n;
            }
            t = now_ms();
            if (t - last_ui >= 250) {
                last_ui = t;
                wt[wn % 8] = t; wb[wn % 8] = bytes; wn++;
                int k = wn >= 5 ? (wn - 5) % 8 : 0;                                    /* ~1 s window */
                double dt = wn >= 5 ? (double)(t - wt[k]) : (double)(t - t0);
                long long db = wn >= 5 ? bytes - wb[k] : bytes;
                pthread_mutex_lock(&lock);
                st_bytes = bytes; st_elapsed = (int)(t - t0);
                st_live = dt > 0 ? (double)db * 8.0 / dt / 1000.0 : 0;
                generation++;
                pthread_mutex_unlock(&lock);
            }
        }
        int ms = (int)(now_ms() - t0);
        close(fd);
        res->bytes = bytes; res->ms = ms > 0 ? ms : 1; res->stopped = stopped;
        res->mbps = (double)bytes * 8.0 / (double)res->ms / 1000.0;
        if (bytes <= 0) { snprintf(st_err, sizeof st_err, "%s 没有数据", host); return 0; }
        return 1;
    }
    snprintf(st_err, sizeof st_err, "跳转次数过多");
    return 0;
}
static void st_radio_locked(char *o, size_t n) {
    const struct carrier *p = primary_locked();
    if (!dash.ok || !p) { snprintf(o, n, "%s", dash.ok ? "无服务小区" : "信号未知"); return; }
    char pci[8], sinr[16] = "—";
    pci_text(p->pci, pci, sizeof pci);
    if (p->has_sinr) fmt_val(p->sinr, "dB", sinr, sizeof sinr);
    snprintf(o, n, "%s %s · PCI %s · SINR %s", mode_label(dash.mode, dash.type), p->band, pci, sinr);
}
/* the whole test; speed thread (or a test) only */
static void st_run(void) {
    struct st_result res;
    memset(&res, 0, sizeof res);
    pthread_mutex_lock(&lock);
    st_radio_locked(res.radio, sizeof res.radio);
    st_bytes = 0; st_elapsed = 0; st_live = 0; st_ping = -1; st_err[0] = 0;
    pthread_mutex_unlock(&lock);
    char ifn[IFNAMSIZ + 1];
    struct in_addr src;
    int ok = 0;
    st_set_phase("查找蜂窝接口…");
    if (!st_wan(ifn, sizeof ifn, &src)) snprintf(st_err, sizeof st_err, "找不到蜂窝网络接口");
    else {
        int fd = st_socket(SOCK_DGRAM, ifn, src);
        if (fd < 0) snprintf(st_err, sizeof st_err, "无法绑定到 %s", ifn);
        else {
            close(fd);
            st_set_phase("测延迟…");
            int ping = st_latency(ifn, src);
            pthread_mutex_lock(&lock); st_ping = ping; generation++; pthread_mutex_unlock(&lock);
            res.ping_ms = ping;
            for (int i = 0; st_urls[i] && !ok && !st_cancelled(); i++) ok = st_download(st_urls[i], ifn, src, &res);
        }
    }
    pthread_mutex_lock(&lock);
    int stopped = st_cancel;
    res.wall = (int64_t)time(NULL);
    if (ok && (!res.stopped || res.ms >= 1000)) {
        memmove(st_hist + 1, st_hist, sizeof st_hist[0] * (ST_HIST - 1));
        st_hist[0] = res;
        if (st_nhist < ST_HIST) st_nhist++;
        st_kept = 1;
        st_state = res.stopped ? SP_STOPPED : SP_DONE;
    } else { st_kept = 0; st_state = stopped ? SP_STOPPED : SP_FAIL; }
    if (stopped && !st_err[0]) snprintf(st_err, sizeof st_err, "已停止");
    st_cancel = 0;
    st_cool_until = now_ms() + st_cooldown_ms;
    generation++;
    pthread_mutex_unlock(&lock);
}
static void *speed_thread(void *unused) {
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!st_req) pthread_cond_wait(&st_cond, &lock);
        st_req = 0;
        pthread_mutex_unlock(&lock);
        st_run();
    }
    return NULL;
}
/* UI thread, lock held: tap on 测速 / 停止 */
static int st_tap_locked(void) {
    if (st_state == SP_RUN) { if (st_cancel) return 0; st_cancel = 1; generation++; return 1; }
    if (now_ms() < st_cool_until || lock_busy_locked()) return 0;
    st_state = SP_RUN; st_req = 1; st_cancel = 0;
    snprintf(st_phase, sizeof st_phase, "准备中…");
    st_bytes = 0; st_elapsed = 0; st_live = 0; st_ping = -1; st_err[0] = 0;
    generation++;
    pthread_cond_signal(&st_cond);
    return 1;
}

struct ov_view {
    char header[96]; int header_tone;
    char rsrp[24], rsrp_rate[16], sinr[24], sinr_rate[16]; int rsrp_tone, sinr_tone, value_tone;
    int bars;
    char carrier[128]; int carrier_tone;
    char dl[32], ul[32];
    char card_title[4][32], card_val[4][32];
    char proxy[NAME_MAX + 64];
    char st_val[32], st_detail[160], st_radio[96], st_hist[128], st_note[96], st_btn[24];
    int st_val_tone, st_btn_style;                    /* btn: 0 disabled, 1 primary, 2 stop */
};
static void st_view_locked(struct ov_view *v) {
    int64_t t = now_ms();
    char tbuf[16] = "";
    v->st_val_tone = T_GREY; v->st_btn_style = 1;
    snprintf(v->st_btn, sizeof v->st_btn, "测速");
    snprintf(v->st_note, sizeof v->st_note, "每次约消耗几十 MB 流量");
    if (st_state == SP_RUN) {
        v->st_btn_style = st_cancel ? 0 : 2;
        snprintf(v->st_btn, sizeof v->st_btn, "%s", st_cancel ? "停止中…" : "停止");
        v->st_val_tone = T_BLUE;
        if (st_elapsed > 0) snprintf(v->st_val, sizeof v->st_val, "%.1f Mbps", st_live);
        else snprintf(v->st_val, sizeof v->st_val, "测速中");
        char ping[24] = "";
        if (st_ping >= 0) snprintf(ping, sizeof ping, " · 延迟 %d ms", st_ping);
        if (!strcmp(st_phase, "下载中"))
            snprintf(v->st_detail, sizeof v->st_detail, "下载中 %.1f s · %.1f MB%s", st_elapsed / 1000.0, (double)st_bytes / 1e6, ping);
        else snprintf(v->st_detail, sizeof v->st_detail, "%s%s", st_phase, ping);
        snprintf(v->st_note, sizeof v->st_note, "直连蜂窝，不经代理 · 最多 10 秒 / 50 MB");
    } else {
        const struct st_result *r = st_nhist ? &st_hist[0] : NULL;
        if (st_state == SP_FAIL || (st_state == SP_STOPPED && !st_kept)) {
            snprintf(v->st_val, sizeof v->st_val, "%s", st_state == SP_STOPPED ? "已停止" : "测速失败");
            v->st_val_tone = st_state == SP_STOPPED ? T_DIM : T_BAD;
            snprintf(v->st_detail, sizeof v->st_detail, "%s", st_err);
        } else if (r) {
            struct tm tm; time_t w = (time_t)r->wall;
            localtime_r(&w, &tm);
            strftime(tbuf, sizeof tbuf, "%H:%M", &tm);
            snprintf(v->st_val, sizeof v->st_val, "%.1f Mbps", r->mbps);
            v->st_val_tone = r->mbps >= 50 ? T_GOOD : r->mbps >= 10 ? T_TEXT : T_WARN;
            char ping[24];
            if (r->ping_ms >= 0) snprintf(ping, sizeof ping, "延迟 %d ms", r->ping_ms); else snprintf(ping, sizeof ping, "延迟 —");
            snprintf(v->st_detail, sizeof v->st_detail, "%s%s · %s 测", r->stopped ? "已停止 · " : "", ping, tbuf);
            snprintf(v->st_radio, sizeof v->st_radio, "%s", r->radio);
        } else {
            snprintf(v->st_val, sizeof v->st_val, "—");
            snprintf(v->st_detail, sizeof v->st_detail, "测下载速度和延迟（直连蜂窝，不经代理）");
        }
        if (st_nhist > 1) {
            size_t k = (size_t)snprintf(v->st_hist, sizeof v->st_hist, "之前");
            for (int i = 1; i < st_nhist && k + 16 < sizeof v->st_hist; i++)
                k += (size_t)snprintf(v->st_hist + k, sizeof v->st_hist - k, "%s%.1f", i == 1 ? " " : " · ", st_hist[i].mbps);
            snprintf(v->st_hist + k, sizeof v->st_hist - k, " Mbps");
        }
        if (r && st_state != SP_FAIL) snprintf(v->st_note, sizeof v->st_note, "每次约消耗几十 MB 流量 · 上次 %.1f MB", (double)r->bytes / 1e6);
        if (t < st_cool_until) { v->st_btn_style = 0; snprintf(v->st_btn, sizeof v->st_btn, "%d 秒后可测", (int)((st_cool_until - t + 999) / 1000)); }
        else if (lock_busy_locked()) { v->st_btn_style = 0; snprintf(v->st_btn, sizeof v->st_btn, "锁频中"); }
    }
}
static void build_ov(struct ov_view *v) {
    memset(v, 0, sizeof *v);
    pthread_mutex_lock(&lock);
    const struct dash *d = &dash;
    if (!d->ok) {
        snprintf(v->header, sizeof v->header, "%s", dash_err == 1 ? "agent 无响应" : dash_err == 2 ? "agent 登录失败" : dash_err == 3 ? "agent 返回异常" : "正在读取");
        v->header_tone = dash_err ? T_BAD : T_DIM;
    } else if (d->stale) {
        snprintf(v->header, sizeof v->header, "数据已过期");
        v->header_tone = T_WARN;
    } else {
        snprintf(v->header, sizeof v->header, "%s%s%s", mode_label(d->mode, d->type), d->carrier_name[0] ? " · " : "", d->carrier_name);
        v->header_tone = T_DIM;
    }
    v->value_tone = d->ok && !d->stale ? T_TEXT : T_GREY;
    const struct carrier *p = primary_locked();
    snprintf(v->rsrp, sizeof v->rsrp, "—"); snprintf(v->rsrp_rate, sizeof v->rsrp_rate, "不可用"); v->rsrp_tone = T_DIM;
    snprintf(v->sinr, sizeof v->sinr, "—"); snprintf(v->sinr_rate, sizeof v->sinr_rate, "不可用"); v->sinr_tone = T_DIM;
    if (p && p->has_rsrp) { int l = rsrp_level(p->rsrp); fmt_val(p->rsrp, "dBm", v->rsrp, sizeof v->rsrp); snprintf(v->rsrp_rate, sizeof v->rsrp_rate, "%s", rate_words[l]); v->rsrp_tone = level_tone(l); }
    if (p && p->has_sinr) { int l = sinr_level(p->sinr); fmt_val(p->sinr, "dB", v->sinr, sizeof v->sinr); snprintf(v->sinr_rate, sizeof v->sinr_rate, "%s", rate_words[l]); v->sinr_tone = level_tone(l); }
    if (d->stale) v->rsrp_tone = v->sinr_tone = T_GREY;
    v->bars = d->ok ? d->bars : -1;
    if (p) {
        char pci[8];
        pci_text(p->pci, pci, sizeof pci);
        snprintf(v->carrier, sizeof v->carrier, "%s · PCI %s · ARFCN %lld · %s", p->band, pci, p->arfcn, p->bw);
        v->carrier_tone = d->stale ? T_GREY : d->prim == RAT_NR ? T_NR : T_TEXT;
    } else {
        snprintf(v->carrier, sizeof v->carrier, "%s", d->ok ? "没有服务小区" : "—");
        v->carrier_tone = d->ok ? T_WARN : T_DIM;
    }
    if (d->ok && d->has_speed) { fmt_rate(d->rx_Bps, v->dl, sizeof v->dl); fmt_rate(d->tx_Bps, v->ul, sizeof v->ul); }
    else { snprintf(v->dl, sizeof v->dl, "—"); snprintf(v->ul, sizeof v->ul, "—"); }
    snprintf(v->card_title[0], 32, d->ok && d->has_batt && d->charging ? "电量 充电" : "电量");
    if (d->ok && d->has_batt) snprintf(v->card_val[0], 32, "%d %%", d->batt_pct); else snprintf(v->card_val[0], 32, "—");
    snprintf(v->card_title[1], 32, d->ok && d->has_cpu_temp ? "CPU 温度" : d->ok && d->has_batt_temp ? "电池温度" : "温度");
    if (d->ok && (d->has_cpu_temp || d->has_batt_temp)) snprintf(v->card_val[1], 32, "%.0f °C", d->has_cpu_temp ? d->cpu_temp : d->batt_temp);
    else snprintf(v->card_val[1], 32, "—");
    snprintf(v->card_title[2], 32, "今日流量");
    if (d->ok && d->has_usage) fmt_bytes(d->day_bytes, v->card_val[2], 32); else snprintf(v->card_val[2], 32, "—");
    snprintf(v->card_title[3], 32, "设备");
    if (clients_n >= 0) snprintf(v->card_val[3], 32, "%d 台", clients_n); else snprintf(v->card_val[3], 32, "—");
    static const char *const mode_cn[3] = {"规则", "全局", "直连"};
    if (have_state)
        snprintf(v->proxy, sizeof v->proxy, "代理  %s · %s · TUN %s", mode_cn[mode_index(mode_name)],
                 node_index >= 0 && node_index < node_count ? nodes[node_index] : "未选择", tun_on ? "开" : "关");
    else snprintf(v->proxy, sizeof v->proxy, "代理  %s", status[0] ? status : "—");
    st_view_locked(v);
    pthread_mutex_unlock(&lock);
}

struct band_view {
    char legend[64]; int legend_tone;
    char info1[128], info2[128], pill[32]; int pill_tone;     /* pill_tone 0 = hidden */
    int tab;
    int nchips, chip_band[BAND_CHIPS]; char chip[BAND_CHIPS][8];
    unsigned char chip_sel[BAND_CHIPS], chip_use[BAND_CHIPS], chip_lock[BAND_CHIPS];
    int chips_enabled;
    char empty[64];
    char reset_text[24]; int reset_enabled;
    char apply_text[40]; int apply_style;                     /* 0 disabled, 1 primary, 2 retry */
};
static void build_band(struct band_view *v) {
    memset(v, 0, sizeof *v);
    pthread_mutex_lock(&lock);
    int64_t t = now_ms();
    int busy = lock_busy_locked() || busy_op != OP_NONE;
    int rat = v->tab = band_tab;
    int mine = lock_is_band(lk_op.kind);
    if (rat == 0 && dash.ok && dash.mode == NM_NSA && !caps_nsa_supported) {
        snprintf(v->legend, sizeof v->legend, "NSA 下 NR(SA) 锁频暂不生效");
        v->legend_tone = T_WARN;
    } else if (dash.ok && caps_ok && lock_is_all_locked(rat) && !draft_locked(rat, (int[MAX_LOCK_BANDS]){0})) {
        snprintf(v->legend, sizeof v->legend, "点选要使用的频段（当前自动）");
        v->legend_tone = T_BLUE;
    } else if (dash.ok && caps_ok && lock_is_all_locked(rat)) {
        snprintf(v->legend, sizeof v->legend, "只用蓝底频段，其余全部排除");
        v->legend_tone = T_BLUE;
    } else { snprintf(v->legend, sizeof v->legend, "绿点 在用 · 黄线 已锁 · 蓝底 草稿"); v->legend_tone = T_DIM; }
    const struct carrier *p = primary_locked();
    if (p) {
        char pci[8], more[64] = "";
        pci_text(p->pci, pci, sizeof pci);
        const struct carrier *c = dash.prim == RAT_NR ? dash.nr : dash.lte;
        int cn = dash.prim == RAT_NR ? dash.nnr : dash.nlte;
        size_t k = 0;
        for (int i = 1; i < cn && k + 12 < sizeof more; i++) k += (size_t)snprintf(more + k, sizeof more - k, " +%s", c[i].band);
        snprintf(v->info1, sizeof v->info1, "实际服务 %s PCC · PCI %s%s", p->band, pci, more);
    } else snprintf(v->info1, sizeof v->info1, "实际服务 %s", dash.ok ? "无" : "—");
    char nr[64], lte[64];
    describe_lock_locked(0, nr, sizeof nr);
    describe_lock_locked(1, lte, sizeof lte);
    snprintf(v->info2, sizeof v->info2, "锁定 NR %s · LTE %s", nr, lte);
    int changes = draft_changes_locked(rat);
    if (mine && lk_state == LS_SENDING) { snprintf(v->pill, sizeof v->pill, "下发中"); v->pill_tone = T_BLUE; }
    else if (mine && lk_state == LS_VERIFY) { snprintf(v->pill, sizeof v->pill, "读回中 %d/%d", lk_polls, VERIFY_POLLS); v->pill_tone = T_BLUE; }
    else if (mine && lk_state == LS_OK && t - lk_state_at < RESULT_OK_MS) { snprintf(v->pill, sizeof v->pill, "设备已确认"); v->pill_tone = T_GOOD; }
    else if (mine && lk_state == LS_FAIL) { snprintf(v->pill, sizeof v->pill, "未生效"); v->pill_tone = T_BAD; }
    else if (changes) { snprintf(v->pill, sizeof v->pill, "草稿未下发"); v->pill_tone = T_WARN; }
    int dr[MAX_LOCK_BANDS], nd = draft_locked(rat, dr);
    if (caps_ok) {
        int all[MAX_LOCK_BANDS * 2], na = 0;
        const int *caps = rat ? caps_lte : caps_nr;
        int nc = rat ? ncaps_lte : ncaps_nr;
        for (int i = 0; i < nc; i++) all[na++] = caps[i];
        for (int i = 0; i < nd; i++) all[na++] = dr[i];   /* show every selected band, even one caps lacks */
        norm_bands(all, &na);
        const struct bandlock *l = observed_lock_locked(rat);
        for (int i = 0; i < na && v->nchips < BAND_CHIPS; i++) {
            int k = v->nchips++, b = all[i];
            v->chip_band[k] = b;
            snprintf(v->chip[k], sizeof v->chip[k], "%s%d", rat ? "B" : "n", b);
            v->chip_sel[k] = (unsigned char)has_band(dr, nd, b);
            v->chip_use[k] = (unsigned char)in_use_locked(rat, b);
            v->chip_lock[k] = (unsigned char)(dash.ok && l->kind == LK_LOCKED && !lock_is_all_locked(rat) && has_band(l->bands, l->n, b));
        }
        if (!v->nchips) snprintf(v->empty, sizeof v->empty, "固件没有报告可锁的%s频段", rat ? " LTE " : " NR ");
    } else snprintf(v->empty, sizeof v->empty, "%s", dash_err ? "读不到频段表（agent 无响应）" : "正在读取频段表…");
    v->chips_enabled = !busy && caps_ok;
    snprintf(v->reset_text, sizeof v->reset_text, "恢复自动");
    v->reset_enabled = !busy;
    int my_kind = rat ? LK_BAND_LTE : LK_BAND_NR;
    if (mine && lk_state == LS_SENDING) snprintf(v->apply_text, sizeof v->apply_text, "下发中…");
    else if (mine && lk_state == LS_VERIFY) snprintf(v->apply_text, sizeof v->apply_text, "读回中…");
    else if (lk_state == LS_FAIL && lk_op.kind == my_kind && same_bands(dr, nd, lk_op.bands, lk_op.nb)) {
        snprintf(v->apply_text, sizeof v->apply_text, "重试下发");
        v->apply_style = busy ? 0 : 2;
    } else if (changes && !nd) snprintf(v->apply_text, sizeof v->apply_text, "至少选 1 个频段");
    else if (changes) {
        snprintf(v->apply_text, sizeof v->apply_text, "应用锁频 (%d)", changes);
        v->apply_style = busy ? 0 : 1;
    } else snprintf(v->apply_text, sizeof v->apply_text, "应用锁频");
    pthread_mutex_unlock(&lock);
}

struct cell_view {
    char tag[8]; int tag_tone;
    char pcc_title[48], pcc[96], lockline[128], caption[96];
    int nrows;
    char row_main[CELL_ROWS][48], row_sub[CELL_ROWS][64], row_rsrp[CELL_ROWS][24];
    unsigned char row_tone[CELL_ROWS], row_sel[CELL_ROWS], row_ok[CELL_ROWS];
    int row_rat[CELL_ROWS], row_pci[CELL_ROWS], row_band[CELL_ROWS]; long long row_arfcn[CELL_ROWS];
    char empty[64];
    char reset_text[24]; int reset_enabled;
    char lock_text[40]; int lock_style;                          /* 0 disabled, 1 primary, 2 retry */
};
static void build_cell(struct cell_view *v) {
    memset(v, 0, sizeof *v);
    pthread_mutex_lock(&lock);
    int busy = lock_busy_locked() || busy_op != OP_NONE;
    int mine = lk_op.kind == LK_CELL_NR || lk_op.kind == LK_CELL_LTE || lk_op.kind == LK_CELL_RESET;
    const struct carrier *p = primary_locked();
    snprintf(v->tag, sizeof v->tag, "%s", p ? (dash.prim == RAT_NR ? "NR" : "LTE") : "—");
    v->tag_tone = p && dash.prim == RAT_NR ? T_NR : T_DIM;
    snprintf(v->pcc_title, sizeof v->pcc_title, "当前主小区 (PCC)");
    if (p) { char pci[8]; pci_text(p->pci, pci, sizeof pci); snprintf(v->pcc, sizeof v->pcc, "PCI %s · %lld · %s", pci, p->arfcn, p->band); }
    else snprintf(v->pcc, sizeof v->pcc, "%s", dash.ok ? "没有服务小区" : "—");
    snprintf(v->lockline, sizeof v->lockline, "锁定状态 agent 不提供 · 本屏上次请求 %s", cell_last_req[0] ? cell_last_req : "无");
    snprintf(v->caption, sizeof v->caption, "候选小区 · 仅当前服务载波（agent 无邻区接口）");
    struct cand c[CELL_ROWS];
    v->nrows = candidates_locked(c, CELL_ROWS);
    for (int i = 0; i < v->nrows; i++) {
        char pci[8];
        pci_text(c[i].pci, pci, sizeof pci);
        snprintf(v->row_main[i], sizeof v->row_main[i], "PCI %s  %s", pci, c[i].label);
        snprintf(v->row_sub[i], sizeof v->row_sub[i], "%s · ARFCN %lld", c[i].band, c[i].arfcn);
        if (c[i].has_rsrp) { fmt_val(c[i].rsrp, "dBm", v->row_rsrp[i], sizeof v->row_rsrp[i]); v->row_tone[i] = (unsigned char)level_tone(rsrp_level(c[i].rsrp)); }
        else { snprintf(v->row_rsrp[i], sizeof v->row_rsrp[i], "—"); v->row_tone[i] = T_DIM; }
        v->row_rat[i] = c[i].rat; v->row_pci[i] = c[i].pci; v->row_arfcn[i] = c[i].arfcn; v->row_band[i] = c[i].bandnum;
        v->row_ok[i] = (unsigned char)(c[i].pci >= 0 && c[i].arfcn > 0 && (c[i].rat == RAT_LTE || c[i].bandnum > 0));
        v->row_sel[i] = (unsigned char)(c[i].rat == cell_sel_rat && c[i].pci == cell_sel_pci && c[i].arfcn == cell_sel_arfcn);
    }
    if (!dash.ok) snprintf(v->empty, sizeof v->empty, "%s", dash_err ? "读不到小区信息（agent 无响应）" : "正在读取…");
    else if (!v->nrows) snprintf(v->empty, sizeof v->empty, "没有服务小区");
    snprintf(v->reset_text, sizeof v->reset_text, "解除锁定");
    v->reset_enabled = !busy;
    int sel = -1;
    for (int i = 0; i < v->nrows; i++) if (v->row_sel[i] && v->row_ok[i]) sel = i;
    int sel_kind = sel >= 0 ? (v->row_rat[sel] == RAT_NR ? LK_CELL_NR : LK_CELL_LTE) : LK_NONE;
    if (mine && lk_state == LS_SENDING) snprintf(v->lock_text, sizeof v->lock_text, "下发中…");
    else if (mine && lk_state == LS_VERIFY) snprintf(v->lock_text, sizeof v->lock_text, "读回中…");
    else if (sel >= 0 && lk_state == LS_FAIL && lk_op.kind == sel_kind && lk_op.pci == v->row_pci[sel] && lk_op.arfcn == v->row_arfcn[sel]) {
        snprintf(v->lock_text, sizeof v->lock_text, "重试下发");
        v->lock_style = busy ? 0 : 2;
    } else if (sel >= 0) {
        snprintf(v->lock_text, sizeof v->lock_text, "锁定 PCI %d", v->row_pci[sel]);
        v->lock_style = busy ? 0 : 1;
    } else snprintf(v->lock_text, sizeof v->lock_text, "先选择一个小区");
    pthread_mutex_unlock(&lock);
}

struct overlay_view {
    int confirm, confirm_page;
    char title[64], lead[128], details[320], cons[400], recovery[160], warn[96];
    int warn_bad;
    int result, result_page, result_tone, dismiss;
    char rtitle[64], rbody[640];
};
static void build_overlay(struct overlay_view *v) {
    memset(v, 0, sizeof *v);
    pthread_mutex_lock(&lock);
    int64_t t = now_ms();
    v->confirm = lk_state == LS_CONFIRM;
    v->confirm_page = v->result_page = lk_op.page;
    if (v->confirm) {
        snprintf(v->title, sizeof v->title, "%s", dlg_title);
        snprintf(v->lead, sizeof v->lead, "%s", dlg_lead);
        snprintf(v->details, sizeof v->details, "%s", dlg_details);
        snprintf(v->cons, sizeof v->cons, "%s", dlg_cons);
        snprintf(v->recovery, sizeof v->recovery, "%s", dlg_recovery);
        snprintf(v->warn, sizeof v->warn, "%s", dlg_warn);
        v->warn_bad = dlg_warn_bad;
    }
    int cell = lk_op.kind == LK_CELL_NR || lk_op.kind == LK_CELL_LTE;
    switch (lk_state) {
    case LS_SENDING:
        v->result = 1; v->result_tone = T_BLUE;
        snprintf(v->rtitle, sizeof v->rtitle, "正在下发…");
        snprintf(v->rbody, sizeof v->rbody, "等待 agent 应答（最长 %d 秒）。", lock_timeout_ms / 1000);
        break;
    case LS_VERIFY:
        v->result = 1; v->result_tone = T_BLUE;
        snprintf(v->rtitle, sizeof v->rtitle, "已下发 · 等待设备确认");
        snprintf(v->rbody, sizeof v->rbody, "%s%s第 %d/%d 次读回（每 %d 秒）。%s%s\n网络重连时信号暂时为空属正常。", lk_reason, lk_reason[0] ? "\n" : "",
                 lk_polls, VERIFY_POLLS, verify_every_ms / 1000, lk_readback[0] ? "读回：" : "", lk_readback);
        break;
    case LS_OK:
        if (t - lk_state_at >= RESULT_OK_MS) break;            /* auto-dismiss after 5 s */
        v->result = 1; v->result_tone = T_GOOD;
        snprintf(v->rtitle, sizeof v->rtitle, "%s", cell ? "主小区已切到目标" : "设备已确认");
        snprintf(v->rbody, sizeof v->rbody, "读回：%s%s", lk_readback, cell && lk_preset ? "\n锁定前主小区就是它，无法区分是否真正锁定。" : "");
        break;
    case LS_FAIL:
        v->result = 1; v->result_tone = T_BAD; v->dismiss = 1;
        snprintf(v->rtitle, sizeof v->rtitle, "%s", lk_polls ? "未生效" : "下发失败");
        snprintf(v->rbody, sizeof v->rbody, "%s", lk_reason);
        break;
    case LS_SENT:
        v->result = 1; v->result_tone = T_DIM; v->dismiss = 1;
        snprintf(v->rtitle, sizeof v->rtitle, "已下发（无法读回）");
        snprintf(v->rbody, sizeof v->rbody, "agent 已接受解除请求；设备不返回小区锁定状态，无法确认是否生效。");
        break;
    }
    pthread_mutex_unlock(&lock);
}

/* ==== taps (LVGL thread). ui_tap() holds all logic so tests can drive it without LVGL. ==== */
static int chip_band_shown[BAND_CHIPS];            /* what render_band() put on each chip */
static struct { int rat, pci, band; long long arfcn; } cell_row_shown[CELL_ROWS];
static int ui_tap(int id) {
    int changed = 0;
    if ((id >= 3 && id <= 4) || (id >= 10 && id <= 12) || (id >= 100 && id < 100 + LIST_ROWS)) return queue_tap(id);
    if (id == 20) { goto_page(PG_PROXY, 1); return 1; }
    if (id == 21) { pthread_mutex_lock(&lock); int r = st_tap_locked(); pthread_mutex_unlock(&lock); return r; }
    pthread_mutex_lock(&lock);
    int busy = lock_busy_locked() || busy_op != OP_NONE;
    if (id == 30 || id == 31) { if (band_tab != id - 30) { band_tab = id - 30; generation++; changed = 1; } }
    else if (id >= 200 && id < 200 + BAND_CHIPS) {
        int b = chip_band_shown[id - 200];
        if (!busy && caps_ok && b > 0) { draft_toggle_locked(band_tab, b); changed = 1; }
    } else if (id == 32 || id == 40) {
        struct lockop op = {0};
        op.kind = id == 32 ? LK_BAND_RESET : LK_CELL_RESET;
        op.page = id == 32 ? PG_BAND : PG_CELL;
        changed = lock_open_locked(&op);
    } else if (id == 33) {
        int rat = band_tab, kind = rat ? LK_BAND_LTE : LK_BAND_NR;
        struct lockop op = {0};
        op.kind = kind;
        op.page = PG_BAND;
        op.nb = draft_locked(rat, op.bands);                  /* the snapshot frozen for the dialog */
        int retry = lk_state == LS_FAIL && lk_op.kind == kind && same_bands(op.bands, op.nb, lk_op.bands, lk_op.nb);
        if (retry || draft_changes_locked(rat)) changed = lock_open_locked(&op);
    } else if (id >= 300 && id < 300 + CELL_ROWS) {
        int i = id - 300;
        if (!busy && cell_row_shown[i].pci >= 0 && cell_row_shown[i].arfcn > 0) {
            cell_sel_rat = cell_row_shown[i].rat;
            cell_sel_pci = cell_row_shown[i].pci;
            cell_sel_arfcn = cell_row_shown[i].arfcn;
            generation++;
            changed = 1;
        }
    } else if (id == 41) {
        struct cand c[CELL_ROWS];
        int n = candidates_locked(c, CELL_ROWS);              /* the selection must still be a serving cell */
        for (int i = 0; i < n; i++) {
            if (c[i].rat != cell_sel_rat || c[i].pci != cell_sel_pci || c[i].arfcn != cell_sel_arfcn) continue;
            if (c[i].rat == RAT_NR && c[i].bandnum <= 0) break;
            struct lockop op = {0};
            op.kind = c[i].rat == RAT_NR ? LK_CELL_NR : LK_CELL_LTE;
            op.page = PG_CELL;
            op.pci = c[i].pci;
            op.arfcn = c[i].arfcn;
            op.bandnum = c[i].bandnum;
            changed = lock_open_locked(&op);
            break;
        }
    } else if (id == 50) changed = lock_cancel_locked();
    else if (id == 51) changed = lock_confirm_locked();
    else if (id == 52) changed = lock_dismiss_locked();
    pthread_mutex_unlock(&lock);
    return changed;
}

/* ==== objects of the new pages + render (LVGL thread only) ==== */
static void *ov_page, *ov_header, *ov_sig, *ov_rsrp, *ov_rsrp_rate, *ov_sinr, *ov_sinr_rate, *ov_bars[5], *ov_carrier;
static void *ov_dl, *ov_ul, *ov_card_title[4], *ov_card_val[4], *ov_proxy, *ov_proxy_cap;
static void *ov_st_val, *ov_st_radio, *ov_st_detail, *ov_st_hist, *ov_st_note, *ov_st_btn, *ov_st_btn_cap;
static void *bd_page, *bd_legend, *bd_info1, *bd_info2, *bd_pill, *bd_tab[2], *bd_tab_cap[2], *bd_empty;
static void *bd_chip[BAND_CHIPS], *bd_chip_cap[BAND_CHIPS], *bd_chip_dot[BAND_CHIPS], *bd_chip_bar[BAND_CHIPS];
static void *bd_reset, *bd_reset_cap, *bd_apply, *bd_apply_cap;
static void *cl_page, *cl_tag, *cl_pcc_title, *cl_pcc, *cl_lockline, *cl_caption, *cl_empty;
static void *cl_row[CELL_ROWS], *cl_row_main[CELL_ROWS], *cl_row_sub[CELL_ROWS], *cl_row_rsrp[CELL_ROWS];
static void *cl_reset, *cl_reset_cap, *cl_lock, *cl_lock_cap;
static void *rs_card, *rs_title, *rs_body, *rs_dismiss, *rs_dismiss_cap;
static void *dl_block, *dl_card, *dl_title, *dl_lead, *dl_details, *dl_warn, *dl_cons, *dl_recov, *dl_cancel, *dl_cancel_cap, *dl_ok, *dl_ok_cap;
static int obj_count_proxy, obj_count_overview, obj_count_band, obj_count_cell, obj_count_overlay, obj_count_dots;

static void tone_text(void *o, int tone) { const unsigned char *c = tone_rgbs[tone]; paint(o, c[0], c[1], c[2], 1); }
static void pos_c(void *o, int x, int y) {          /* cached pos (render-time moves) */
    struct objcache *c = oc(o);
    if (c->has_pos && c->px == x && c->py == y) return;
    c->has_pos = 1; c->px = (int16_t)x; c->py = (int16_t)y;
    pos(o, x, y);
}
/* rough pixel width of s at font px (CJK ~1 em, ASCII ~0.56 em): used to centre captions */
static int text_px(const char *s, int font) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; ) {
        if (*p < 0x80) { w += font * 56 / 100; p++; }
        else { w += font; p += *p >= 0xF0 ? 4 : *p >= 0xE0 ? 3 : 2; }
    }
    return w;
}
static void caption_centre(void *cap, int w, int h, const char *s, int font) {
    int tw = text_px(s, font);
    if (tw > w - 8) tw = w - 8;
    pos_c(cap, (w - tw) / 2, (h - 22) / 2);
    text(cap, s);
}
static void card(void *o) {
    curve(o, 12);
    fill(o, 14, 32, 54, 120);
    outline(o);
}
/* action-bar button styles: 0 disabled, 1 primary, 2 retry, 3 secondary */
static void action_style(void *b, void *cap, int style) {
    if (style == 1) { fill(b, 47, 128, 237, 230); paint(cap, 255, 255, 255, 1); }
    else if (style == 2) { fill(b, 201, 138, 18, 230); paint(cap, 255, 255, 255, 1); }
    else if (style == 3) { fill(b, 16, 36, 62, 120); paint(cap, 232, 241, 242, 1); }
    else { fill(b, 16, 36, 62, 70); paint(cap, 110, 124, 140, 1); }
    flag(b, CLICKABLE, style != 0);
    flag(cap, CLICKABLE, style != 0);
}
static void note_band_shown(const struct band_view *b) {
    for (int i = 0; i < BAND_CHIPS; i++) chip_band_shown[i] = i < b->nchips ? b->chip_band[i] : 0;
}
static void note_cell_shown(const struct cell_view *c) {
    for (int i = 0; i < CELL_ROWS; i++) {
        int ok = i < c->nrows && c->row_ok[i];
        cell_row_shown[i].rat = ok ? c->row_rat[i] : 0;
        cell_row_shown[i].pci = ok ? c->row_pci[i] : -1;
        cell_row_shown[i].arfcn = ok ? c->row_arfcn[i] : -1;
        cell_row_shown[i].band = ok ? c->row_band[i] : 0;
    }
}
static void render_ov(void) {
    static struct ov_view v;
    build_ov(&v);
    text(ov_header, v.header); tone_text(ov_header, v.header_tone);
    text(ov_rsrp, v.rsrp); tone_text(ov_rsrp, v.value_tone);
    text(ov_rsrp_rate, v.rsrp_rate); tone_text(ov_rsrp_rate, v.rsrp_tone);
    text(ov_sinr, v.sinr); tone_text(ov_sinr, v.value_tone);
    text(ov_sinr_rate, v.sinr_rate); tone_text(ov_sinr_rate, v.sinr_tone);
    for (int i = 0; i < 5; i++) {
        int on = v.bars > i;
        if (on) fill(ov_bars[i], 61, 214, 140, 255); else fill(ov_bars[i], 50, 66, 90, 255);
    }
    text(ov_carrier, v.carrier); tone_text(ov_carrier, v.carrier_tone);
    text(ov_dl, v.dl); tone_text(ov_dl, v.value_tone);
    text(ov_ul, v.ul); tone_text(ov_ul, v.value_tone);
    for (int i = 0; i < 4; i++) { text(ov_card_title[i], v.card_title[i]); text(ov_card_val[i], v.card_val[i]); tone_text(ov_card_val[i], i == 3 ? T_TEXT : v.value_tone); }
    text(ov_proxy_cap, v.proxy);
    text(ov_st_val, v.st_val); tone_text(ov_st_val, v.st_val_tone);
    text(ov_st_detail, v.st_detail);
    text(ov_st_radio, v.st_radio);
    text(ov_st_hist, v.st_hist);
    text(ov_st_note, v.st_note);
    flag(ov_st_btn, CLICKABLE, v.st_btn_style != 0); flag(ov_st_btn_cap, CLICKABLE, v.st_btn_style != 0);
    if (v.st_btn_style == 2) fill(ov_st_btn, 160, 60, 50, 230);
    else if (v.st_btn_style == 1) fill(ov_st_btn, 47, 128, 237, 200);
    else fill(ov_st_btn, 16, 36, 62, 120);
    paint(ov_st_btn_cap, v.st_btn_style ? 255 : 154, v.st_btn_style ? 255 : 176, v.st_btn_style ? 255 : 204, 1);
    caption_centre(ov_st_btn_cap, 96, 34, v.st_btn, 16);
}
static void render_band(void) {
    static struct band_view v;
    build_band(&v);
    text(bd_legend, v.legend); tone_text(bd_legend, v.legend_tone);
    text(bd_info1, v.info1);
    text(bd_info2, v.info2);
    flag(bd_pill, HIDDEN, !v.pill_tone);
    if (v.pill_tone) {
        const unsigned char *c = tone_rgbs[v.pill_tone];
        text(bd_pill, v.pill); paint(bd_pill, c[0], c[1], c[2], 1); fill(bd_pill, c[0] / 4, c[1] / 4, c[2] / 4, 200);
    }
    for (int i = 0; i < 2; i++) {
        int sel = v.tab == i;
        fill(bd_tab[i], sel ? 47 : 16, sel ? 128 : 36, sel ? 237 : 62, sel ? 150 : 66);
        paint(bd_tab_cap[i], sel ? 255 : 154, sel ? 255 : 176, sel ? 255 : 204, 1);
        caption_centre(bd_tab_cap[i], 144, 40, i ? "LTE" : "NR (SA)", 16);
    }
    for (int i = 0; i < BAND_CHIPS; i++) {
        int vis = i < v.nchips;
        flag(bd_chip[i], HIDDEN, !vis);
        if (!vis) continue;
        flag(bd_chip[i], CLICKABLE, v.chips_enabled);
        flag(bd_chip_cap[i], CLICKABLE, v.chips_enabled);
        if (v.chip_sel[i]) fill(bd_chip[i], 47, 128, 237, 170); else fill(bd_chip[i], 14, 32, 54, 110);
        if (!v.chips_enabled) paint(bd_chip_cap[i], 120, 134, 150, 1);
        else paint(bd_chip_cap[i], v.chip_sel[i] ? 255 : 232, v.chip_sel[i] ? 255 : 241, v.chip_sel[i] ? 255 : 242, 1);
        caption_centre(bd_chip_cap[i], 52, 40, v.chip[i], 16);
        flag(bd_chip_dot[i], HIDDEN, !v.chip_use[i]);
        flag(bd_chip_bar[i], HIDDEN, !v.chip_lock[i]);
    }
    flag(bd_empty, HIDDEN, !v.empty[0]);
    text(bd_empty, v.empty);
    caption_centre(bd_reset_cap, 104, 44, v.reset_text, 16);
    action_style(bd_reset, bd_reset_cap, v.reset_enabled ? 3 : 0);
    caption_centre(bd_apply_cap, 184, 44, v.apply_text, 16);
    action_style(bd_apply, bd_apply_cap, v.apply_style);
    note_band_shown(&v);
}
static void render_cell(void) {
    static struct cell_view v;
    build_cell(&v);
    text(cl_tag, v.tag); tone_text(cl_tag, v.tag_tone);
    text(cl_pcc_title, v.pcc_title);
    text(cl_pcc, v.pcc);
    text(cl_lockline, v.lockline);
    text(cl_caption, v.caption);
    for (int i = 0; i < CELL_ROWS; i++) {
        int vis = i < v.nrows;
        flag(cl_row[i], HIDDEN, !vis);
        if (!vis) continue;
        flag(cl_row[i], CLICKABLE, v.row_ok[i]);
        flag(cl_row_main[i], CLICKABLE, v.row_ok[i]);
        if (v.row_sel[i]) fill(cl_row[i], 47, 128, 237, 150);
        else fill(cl_row[i], i % 2 ? 20 : 14, i % 2 ? 48 : 32, i % 2 ? 79 : 54, i % 2 ? 56 : 38);
        text(cl_row_main[i], v.row_main[i]);
        tone_text(cl_row_main[i], v.row_ok[i] ? T_TEXT : T_GREY);
        text(cl_row_sub[i], v.row_sub[i]);
        tone_text(cl_row_sub[i], v.row_rat[i] == RAT_NR ? T_NR : T_DIM);
        text(cl_row_rsrp[i], v.row_rsrp[i]);
        tone_text(cl_row_rsrp[i], v.row_tone[i]);
    }
    flag(cl_empty, HIDDEN, !v.empty[0]);
    text(cl_empty, v.empty);
    caption_centre(cl_reset_cap, 104, 44, v.reset_text, 16);
    action_style(cl_reset, cl_reset_cap, v.reset_enabled ? 3 : 0);
    caption_centre(cl_lock_cap, 184, 44, v.lock_text, 16);
    action_style(cl_lock, cl_lock_cap, v.lock_style);
    note_cell_shown(&v);
}
static void render_overlay(void) {
    static struct overlay_view v;
    build_overlay(&v);
    flag(rs_card, HIDDEN, !v.result);
    if (v.result) {
        pos_c(rs_card, PAGE_X(v.result_page) + 12, 240);
        const unsigned char *c = tone_rgbs[v.result_tone];
        fill(rs_card, c[0] / 6, c[1] / 6, c[2] / 6 + 20, 235);
        text(rs_title, v.rtitle); paint(rs_title, c[0], c[1], c[2], 1);
        text(rs_body, v.rbody);
        flag(rs_dismiss, HIDDEN, !v.dismiss);
    }
    flag(dl_block, HIDDEN, !v.confirm);
    if (v.confirm) {
        pos_c(dl_block, PAGE_X(v.confirm_page), 0);
        text(dl_title, v.title);
        text(dl_lead, v.lead);
        text(dl_details, v.details);
        text(dl_cons, v.cons);
        text(dl_recov, v.recovery);
        text(dl_warn, v.warn);
        if (v.warn_bad) paint(dl_warn, 240, 96, 90, 1); else paint(dl_warn, 242, 181, 68, 1);
    }
}
static int64_t last_render_ms;
static void render_all(void) {
    if (!mainform) return;
    render_ov();
    render_proxy();
    render_band();
    render_cell();
    render_overlay();
    last_render_ms = now_ms();
}
/* LVGL thread: shows the pending state right away (render reuses the existing objects). */
static void clicked(void *event) {
    int id = (int)(intptr_t)F(0x429128, void *, void *)(event);
    if (ui_tap(id) && mainform && !modal_on()) render_all();
}
static void *button(void *parent, int x, int y, int w, int h, int id, void **caption) {
    void *o = panel(parent, x, y, w, h, 1);
    flag(o, CLICKABLE, 1);
    F(0x4291a0, void *, void *, void (*)(void *), int, void *)(P(o, 8), clicked, 7, (void *)(intptr_t)id);
    *caption = label(o, 8, (h - 22) / 2, w - 16, 22, 16, "");
    flag(*caption, CLICKABLE, 1);
    F(0x4291a0, void *, void *, void (*)(void *), int, void *)(P(*caption, 8), clicked, 7, (void *)(intptr_t)id);
    return o;
}
static void tick(void *unused) {
    (void)unused;
    static int seen = -1;
    if (!mainform || modal_on()) return;
    int gen, ok_showing;
    pthread_mutex_lock(&lock);
    gen = generation;
    ok_showing = lk_state == LS_OK || now_ms() < st_cool_until;   /* 5 s result / cooldown countdown */
    pthread_mutex_unlock(&lock);
    if (gen != seen || (ok_showing && now_ms() - last_render_ms > 1000)) { seen = gen; render_all(); }
}
static void destroy(void *form) {
    if (form == mainform) {
        if (timer) { F(0x481540, void, void *)(timer); timer = NULL; }
        mainform = NULL;
        memset(dots, 0, sizeof dots);
        memset(stock_dot, 0, sizeof stock_dot);
        list_scroll = chip_scroll = cell_list = NULL;
        /* vtable[0] is a runtime pointer; it must be called directly. The old code passed it through
         * launcher_abi_address(), which returns 0 for addresses outside the 34-entry table
         * (label dtor 0x5303b4, panel dtor 0x534290), i.e. it jumped to NULL. */
        for (int i = nalloc - 1; i >= 0; i--) { void *o = allocs[i]; ((void (*)(void *))P(P(o, 0), 0))(o); free(o); }
        nalloc = 0;
    }
    F(0x4b7ffc, void, void *)(form);
}
static void vlist(void *o) {                       /* vertical scrolling list, excluded from page swipes */
    curve(o, 12);
    fill(o, 10, 28, 48, 31);
    outline(o);
    flag(o, CLICKABLE | SCROLLABLE | CHAIN_HOR, 1);
    flag(o, 512 | BUBBLE, 0);
    F(0x433198, void, void *, unsigned)(P(o, 8), 12);
    F(0x433134, void, void *, unsigned)(P(o, 8), 3);
}
static void deco(void *o) { flag(o, CLICKABLE, 0); }   /* decorations never take a tap */
static void *page_panel(void *parent, int pg) {
    void *o = panel(parent, PAGE_X(pg), 0, 320, 432, 0);
    paint(o, 8, 20, 40, 0);
    if (bg_on) BGF(BG_SET_BG_IMG_SRC, void, void *, const void *)(o, &bg_dsc);
    return o;
}
static void create_proxy(void *parent) {
    page = page_panel(parent, PG_PROXY);
    title = label(page, 16, 8, 188, 30, 22, "代理");
    test_button = button(page, 212, 6, 96, 30, 4, &test_caption);
    curve(test_button, 10); outline(test_button);
    state_label = label(page, 16, 38, 288, 20, 14, "正在读取");
    mode_buttons[0] = button(page, 12, 64, 96, 36, 10, &mode_captions[0]);
    mode_buttons[1] = button(page, 112, 64, 96, 36, 11, &mode_captions[1]);
    mode_buttons[2] = button(page, 212, 64, 96, 36, 12, &mode_captions[2]);
    for (int i = 0; i < 3; i++) { curve(mode_buttons[i], 10); outline(mode_buttons[i]); }
    node = label(page, 16, 106, 288, 24, 16, "—");
    list_scroll = panel(page, 8, 136, 304, 240, 0);
    vlist(list_scroll);
    for (int i = 0; i < LIST_ROWS; i++) {
        row_buttons[i] = button(list_scroll, 6, 6 + i * ROW_H, 292, ROW_H - 4, 100 + i, &row_captions[i]);
        curve(row_buttons[i], 8);
        pos(row_captions[i], 10, 4);
        size(row_captions[i], 272, 22);
        F(0x52fa04, void, void *, int)(row_captions[i], 16);
        flag(row_buttons[i], HIDDEN, 1);
    }
    tun_button = button(page, 12, 384, 296, 40, 3, &tun_caption);
    curve(tun_caption, 12);
    outline(tun_button);
    tun_label = tun_caption;
}
static void create_overview(void *parent) {
    ov_page = page_panel(parent, PG_OVERVIEW);
    label(ov_page, 16, 8, 100, 30, 22, "概览");
    ov_header = label(ov_page, 112, 14, 196, 20, 14, "正在读取");
    ov_sig = panel(ov_page, 12, 48, 296, 118, 0); card(ov_sig);
    label(ov_sig, 14, 6, 120, 18, 12, "RSRP");
    ov_rsrp = label(ov_sig, 14, 24, 132, 30, 22, "—");
    ov_rsrp_rate = label(ov_sig, 14, 58, 120, 20, 14, "");
    label(ov_sig, 150, 6, 80, 18, 12, "SINR");
    ov_sinr = label(ov_sig, 150, 24, 96, 30, 22, "—");
    ov_sinr_rate = label(ov_sig, 150, 58, 80, 20, 14, "");
    for (int i = 0; i < 5; i++) {
        int h = 6 + i * 6;
        ov_bars[i] = panel(ov_sig, 240 + i * 9, 50 - h, 6, h, 0);
        curve(ov_bars[i], 2);
        deco(ov_bars[i]);
    }
    ov_carrier = label(ov_sig, 14, 88, 270, 22, 14, "");
    void *speed = panel(ov_page, 12, 172, 296, 46, 0); card(speed);
    label(speed, 14, 4, 130, 16, 12, "↓ 下行");
    ov_dl = label(speed, 14, 20, 134, 22, 16, "—");
    label(speed, 156, 4, 130, 16, 12, "↑ 上行");
    ov_ul = label(speed, 156, 20, 134, 22, 16, "—");
    for (int i = 0; i < 4; i++) {
        void *c = panel(ov_page, 12 + i * 75, 224, 71, 50, 0); card(c);
        ov_card_title[i] = label(c, 7, 4, 62, 16, 12, "");
        ov_card_val[i] = label(c, 7, 22, 62, 22, 16, "—");
    }
    /* 测速 card: result / live rate, detail, history, data note; button top right */
    void *st = panel(ov_page, 12, 280, 296, 112, 0); card(st);
    ov_st_val = label(st, 14, 4, 170, 30, 22, "—");
    ov_st_btn = button(st, 188, 6, 96, 34, 21, &ov_st_btn_cap);
    curve(ov_st_btn, 10); outline(ov_st_btn);
    ov_st_detail = label(st, 14, 40, 272, 18, 12, "");
    ov_st_radio = label(st, 14, 56, 272, 18, 12, "");
    ov_st_hist = label(st, 14, 74, 272, 18, 12, "");
    paint(ov_st_hist, 154, 176, 204, 1);
    ov_st_note = label(st, 14, 92, 272, 18, 12, "");
    paint(ov_st_note, 154, 176, 204, 1);
    ov_proxy = button(ov_page, 12, 398, 296, 34, 20, &ov_proxy_cap);
    curve(ov_proxy, 12); outline(ov_proxy);
    fill(ov_proxy, 16, 36, 62, 120);
    size(ov_proxy_cap, 280, 22);
}
static void create_band(void *parent) {
    bd_page = page_panel(parent, PG_BAND);
    label(bd_page, 16, 8, 90, 30, 22, "锁频");
    bd_legend = label(bd_page, 100, 16, 208, 18, 12, "");
    void *info = panel(bd_page, 12, 48, 296, 66, 0); card(info);
    bd_info1 = label(info, 12, 8, 272, 20, 14, "");
    bd_info2 = label(info, 12, 36, 196, 20, 14, "");
    bd_pill = label(info, 206, 34, 82, 22, 12, "");
    curve(bd_pill, 6);
    for (int i = 0; i < 2; i++) { bd_tab[i] = button(bd_page, i ? 164 : 12, 122, 144, 40, 30 + i, &bd_tab_cap[i]); curve(bd_tab[i], 10); outline(bd_tab[i]); }
    chip_scroll = panel(bd_page, 12, 170, 296, 204, 0);
    vlist(chip_scroll);
    for (int i = 0; i < BAND_CHIPS; i++) {
        bd_chip[i] = button(chip_scroll, 6 + (i % 5) * 58, 6 + (i / 5) * 46, 52, 40, 200 + i, &bd_chip_cap[i]);
        curve(bd_chip[i], 10); outline(bd_chip[i]);
        bd_chip_dot[i] = panel(bd_chip[i], 42, 4, 6, 6, 0);       /* green dot: serving now */
        curve(bd_chip_dot[i], 3); fill(bd_chip_dot[i], 61, 214, 140, 255); deco(bd_chip_dot[i]);
        bd_chip_bar[i] = panel(bd_chip[i], 18, 34, 16, 3, 0);     /* amber bar: locked on the device */
        curve(bd_chip_bar[i], 1); fill(bd_chip_bar[i], 242, 181, 68, 255); deco(bd_chip_bar[i]);
        flag(bd_chip_dot[i], HIDDEN, 1); flag(bd_chip_bar[i], HIDDEN, 1);
        flag(bd_chip[i], HIDDEN, 1);
    }
    bd_empty = label(chip_scroll, 12, 12, 270, 20, 14, "");
    bd_reset = button(bd_page, 12, 380, 104, 44, 32, &bd_reset_cap); curve(bd_reset, 12); outline(bd_reset);
    bd_apply = button(bd_page, 124, 380, 184, 44, 33, &bd_apply_cap); curve(bd_apply, 12);
}
static void create_cell(void *parent) {
    cl_page = page_panel(parent, PG_CELL);
    label(cl_page, 16, 8, 140, 30, 22, "锁小区");
    cl_tag = label(cl_page, 250, 14, 58, 20, 14, "");
    void *info = panel(cl_page, 12, 48, 296, 78, 0); card(info);
    cl_pcc_title = label(info, 12, 6, 272, 18, 12, "");
    cl_pcc = label(info, 12, 26, 272, 22, 16, "—");
    cl_lockline = label(info, 12, 52, 272, 20, 12, "");
    cl_caption = label(cl_page, 16, 132, 292, 18, 12, "");
    cell_list = panel(cl_page, 12, 152, 296, 222, 0);
    vlist(cell_list);
    for (int i = 0; i < CELL_ROWS; i++) {
        cl_row[i] = button(cell_list, 6, 6 + i * 56, 284, 52, 300 + i, &cl_row_main[i]);
        curve(cl_row[i], 8);
        pos(cl_row_main[i], 10, 4); size(cl_row_main[i], 176, 22);
        cl_row_sub[i] = label(cl_row[i], 10, 28, 190, 18, 12, "");
        cl_row_rsrp[i] = label(cl_row[i], 190, 14, 88, 22, 16, "");
        flag(cl_row[i], HIDDEN, 1);
    }
    cl_empty = label(cell_list, 12, 12, 270, 20, 14, "");
    cl_reset = button(cl_page, 12, 380, 104, 44, 40, &cl_reset_cap); curve(cl_reset, 12); outline(cl_reset);
    cl_lock = button(cl_page, 124, 380, 184, 44, 41, &cl_lock_cap); curve(cl_lock, 12);
}
/* result card + confirm dialog: one shared set, children of the page scroller, created LAST so they
 * draw above every page; moved to the page of the operation when shown. */
static void create_overlay(void *parent) {
    rs_card = panel(parent, PAGE_X(PG_BAND) + 12, 240, 296, 132, 0);
    curve(rs_card, 12); outline(rs_card); deco(rs_card);
    rs_title = label(rs_card, 14, 10, 180, 24, 16, "");
    rs_body = label(rs_card, 14, 40, 268, 84, 14, "");
    rs_dismiss = button(rs_card, 196, 6, 88, 34, 52, &rs_dismiss_cap);
    curve(rs_dismiss, 10); fill(rs_dismiss, 16, 36, 62, 200); outline(rs_dismiss);
    caption_centre(rs_dismiss_cap, 88, 34, "知道了", 16);
    flag(rs_card, HIDDEN, 1);
    dl_block = panel(parent, PAGE_X(PG_BAND), 0, 320, 432, 0);
    fill(dl_block, 3, 8, 16, 215);
    flag(dl_block, CHAIN_HOR | BUBBLE, 0);              /* a drag on the dialog never reaches the pager */
    flag(dl_block, CLICKABLE, 1);                       /* swallows taps: only the two buttons act */
    F(0x4291a0, void *, void *, void (*)(void *), int, void *)(P(dl_block, 8), clicked, 7, (void *)(intptr_t)53);
    dl_card = panel(dl_block, 12, 36, 296, 388, 0);
    curve(dl_card, 14); fill(dl_card, 18, 38, 64, 250); border(dl_card, 1, 90, 130, 180, 120);
    dl_title = label(dl_card, 16, 14, 264, 30, 22, "");
    dl_lead = label(dl_card, 16, 48, 264, 20, 14, "");
    void *dp = panel(dl_card, 16, 72, 264, 84, 0);
    curve(dp, 8); fill(dp, 8, 20, 40, 230); deco(dp);
    dl_details = label(dp, 10, 4, 246, 78, 14, "");
    dl_warn = label(dl_card, 16, 160, 264, 22, 16, "！网络会中断");
    paint(dl_warn, 242, 181, 68, 1);
    dl_cons = label(dl_card, 16, 184, 264, 96, 14, "");
    dl_recov = label(dl_card, 16, 282, 264, 40, 14, "");
    paint(dl_recov, 154, 176, 204, 1);
    dl_cancel = button(dl_card, 12, 328, 130, 48, 50, &dl_cancel_cap);
    curve(dl_cancel, 12); fill(dl_cancel, 22, 52, 90, 255); border(dl_cancel, 2, 96, 165, 250, 255);
    caption_centre(dl_cancel_cap, 130, 48, "取消", 16);
    dl_ok = button(dl_card, 154, 328, 130, 48, 51, &dl_ok_cap);
    curve(dl_ok, 12); fill(dl_ok, 201, 138, 18, 255); paint(dl_ok_cap, 255, 255, 255, 1);
    caption_centre(dl_ok_cap, 130, 48, "确认下发", 16);
    flag(dl_block, HIDDEN, 1);
}
static void create(void *form) {
    F(0x4bd900, void, void *)(form);
    if (mainform == form) return;
    mainform = form;
    current = 1;
    void *parent = P(form, 88);
    bg_decide();
    int n0 = nalloc;
    create_overview(parent);  obj_count_overview = nalloc - n0; n0 = nalloc;
    create_proxy(parent);     obj_count_proxy = nalloc - n0; n0 = nalloc;
    create_band(parent);      obj_count_band = nalloc - n0; n0 = nalloc;
    create_cell(parent);      obj_count_cell = nalloc - n0; n0 = nalloc;
    create_overlay(parent);   obj_count_overlay = nalloc - n0; n0 = nalloc;
    dots[0] = stock_dot[0] = P(form, 848);
    dots[1] = stock_dot[1] = P(form, 856);
    for (int i = 2; i < PAGE_COUNT; i++) dots[i] = label(form, 0, 0, 8, 8, 12, "");
    obj_count_dots = nalloc - n0;
    update_dots(form, 1);
    pthread_once(&wake_once, wake_init);
    static int worker_started;              /* one worker for the process; it never touches LVGL */
#ifdef U60_FAKE_LVGL
    worker_started = 1;                     /* host UI tests drive the logic themselves */
#endif
    if (!worker_started) {
        pthread_t thread;
        if (!pthread_create(&thread, NULL, worker, NULL)) { pthread_detach(thread); worker_started = 1; }
        if (worker_started && !pthread_create(&thread, NULL, speed_thread, NULL)) pthread_detach(thread);
    }
    timer = F(0x481428, void *, void (*)(void *), unsigned, void *)(tick, 200, NULL);
#ifndef U60_FAKE_LVGL
    int fd = open("/tmp/u60-screen-ready", O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd >= 0) { dprintf(fd, "%ld\n", (long)getpid()); close(fd); }
#endif
}

static int hook(uintptr_t addr, uintptr_t before, void *after) {
    if (*(uintptr_t *)addr != before) return -1;
    uintptr_t page_base = addr & ~(uintptr_t)4095;
    if (mprotect((void *)page_base, 4096, PROT_READ | PROT_WRITE)) return -1;
    *(void **)addr = after;
    return mprotect((void *)page_base, 4096, PROT_READ);
}
static uintptr_t read_pointer(uintptr_t address) { return *(uintptr_t *)address; }
static uint32_t read_word(uintptr_t address) { return *(uint32_t *)address; }
__attribute__((constructor)) static void init(void) {
    if (!getenv("U60_SCREEN")) return;
    unsetenv("LD_PRELOAD");
    unsetenv("U60_SCREEN");
    abi = launcher_abi_select(getauxval(AT_PHDR), getauxval(AT_ENTRY), read_pointer, read_word);
    if (!launcher_abi_valid(abi, read_pointer, read_word) || strcmp(abi->name, "b31")) { abi = NULL; return; }
    void *callbacks[5] = {create, scroll_release, update_dots, destroy, destroy};
    for (size_t i = 0; i < 5; i++) if (hook(abi->hooks[i].slot, abi->hooks[i].target, callbacks[i])) _exit(71);
    bg_abi_ok = bg_abi_valid(read_pointer, read_word);
#ifdef BG_SELFTEST
    /* offline qemu check only (never shipped): report and stop before the launcher's main() */
    bg_decide();
    dprintf(2, "selftest: abi=%s hooks=ok bg_abi_ok=%d bg_on=%d hdr=%#x size=%u px0=%#x slot0=%p\n", abi->name, bg_abi_ok, bg_on,
            bg_dsc.header, bg_dsc.data_size, bg_pixels ? (unsigned)(bg_pixels[0] | bg_pixels[1] << 8) : 0u,
            *(void **)abi->hooks[0].slot);
    _exit(bg_on ? 0 : 3);
#endif
}
