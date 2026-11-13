/* Recording fake of the launcher's LVGL wrapper calls (host tests + previews only).
 * Include AFTER ../screen.c built with -DU60_FAKE_LVGL. Each b31 address screen.c calls maps to a stub
 * with the same signature; the stubs record a scene graph (geometry, text, colours, flags, z-order). */
#include <assert.h>
#define FMAX 4096
enum { FK_PANEL, FK_LABEL, FK_STOCK };
struct fobj {
    int idx, kind, x, y, w, h, font, radius, has_bg, bg[4], has_tc, tc[4], border_w, border[3], border_opa, bgimg, has_cb, freed;
    unsigned flags;
    struct fobj *parent;
    void *wrapper;
    void (*cb)(void *);
    intptr_t user;
    char text[1024];
};
static struct fobj fobjs[FMAX];
static int nfobj;
static long fake_setter_calls;                /* LVGL style/text/flag/geometry calls (render cost) */
static int fake_scroll_x;
static void *fake_scrolling;                  /* what the fake indev reports as being scrolled */
static char fake_indev[64];
static void fake_dtor(void *o) { struct fobj *f = P(o, 8); if (f) f->freed = 1; }
static void (*fake_vtbl[1])(void *) = {fake_dtor};
static struct fobj *fnew(void *wrapper, void *parent_wrapper, int kind) {
    assert(nfobj < FMAX);
    struct fobj *f = &fobjs[nfobj];
    memset(f, 0, sizeof *f);
    f->idx = nfobj++;
    f->kind = kind;
    f->wrapper = wrapper;
    f->parent = parent_wrapper ? P(parent_wrapper, 8) : NULL;
    f->flags = CLICKABLE | 0x0400 /*SCROLL_ELASTIC etc: irrelevant*/;
    P(wrapper, 0) = fake_vtbl;
    P(wrapper, 8) = f;
    return f;
}
static struct fobj *W(void *o) { return (struct fobj *)P(o, 8); }
static void s_pos(void *o, int x, int y) { fake_setter_calls++; W(o)->x = x; W(o)->y = y; }
static void s_size(void *o, int w, int h) { fake_setter_calls++; W(o)->w = w; W(o)->h = h; }
static void s_add_flag(void *lv, unsigned f) { fake_setter_calls++; ((struct fobj *)lv)->flags |= f; }
static void s_clear_flag(void *lv, unsigned f) { fake_setter_calls++; ((struct fobj *)lv)->flags &= ~f; }
static void s_text_color(void *o, int r, int g, int b, int a) { fake_setter_calls++; struct fobj *f = W(o); f->has_tc = 1; f->tc[0] = r; f->tc[1] = g; f->tc[2] = b; f->tc[3] = a; }
static void s_bg(void *o, int r, int g, int b, int a) { fake_setter_calls++; struct fobj *f = W(o); f->has_bg = 1; f->bg[0] = r; f->bg[1] = g; f->bg[2] = b; f->bg[3] = a; }
static void s_radius(void *o, int r) { fake_setter_calls++; W(o)->radius = r; }
struct fstr { const char *p; size_t n, cap, pad; };
static void s_set_text(void *o, void *s, int z) {
    fake_setter_calls++;
    struct fstr *str = s;
    size_t n = str->n < 1023 ? str->n : 1023;
    memcpy(W(o)->text, str->p, n);
    W(o)->text[n] = 0;
    assert(utf8_valid_len(W(o)->text) == strlen(W(o)->text));   /* never hand LVGL a broken UTF-8 tail */
}
static void s_label_ctor(void *o, void *parent, void *s, int z) { struct fobj *f = fnew(o, parent, FK_LABEL); f->flags = CLICKABLE; s_set_text(o, s, z); }
static void s_font(void *o, int font) { fake_setter_calls++; W(o)->font = font; }
static void s_longmode(void *o, int m) { fake_setter_calls++; }
static void s_panel_ctor(void *o, void *parent, int z) { struct fobj *f = fnew(o, parent, FK_PANEL); f->flags = CLICKABLE | SCROLLABLE | CHAIN_HOR | 512; }
static void *s_add_event(void *lv, void (*cb)(void *), int code, void *user) { struct fobj *f = lv; f->has_cb = 1; f->cb = cb; f->user = (intptr_t)user; return f; }
static void *s_event_user(void *e) { return e; }   /* tests pass the id as the "event" */
static void s_lv_noop(void *lv, unsigned v) { fake_setter_calls++; }
static int fake_timer;
static void *s_timer_create(void (*cb)(void *), unsigned ms, void *u) { return &fake_timer; }
static void s_timer_del(void *t) {}
static void s_stock_form(void *form) {}
static void s_stock_dots(void *form, int page) {}
static void *s_indev_act(void) { return fake_indev; }
static void *s_indev_scroll_obj(void *indev) { return fake_scrolling; }
static unsigned s_indev_dir(void *indev) { return 0; }
static int s_get_scroll_x(void *o) { return fake_scroll_x; }
static void s_scroll_to_x(void *o, int x, int anim) { fake_scroll_x = x; }
static void s_bg_img(void *o, const void *src) { fake_setter_calls++; W(o)->bgimg = 1; }
static void s_border_w(void *o, int w) { fake_setter_calls++; W(o)->border_w = w; }
static void s_border_rgb(void *o, int r, int g, int b) { fake_setter_calls++; W(o)->border[0] = r; W(o)->border[1] = g; W(o)->border[2] = b; }
static void s_border_opa(void *lv, int opa, unsigned sel) { fake_setter_calls++; ((struct fobj *)lv)->border_opa = opa; }
static void *fake_fn(uintptr_t a) {
    switch (a) {
    case 0x537450: return (void *)s_pos;
    case 0x5382b4: return (void *)s_size;
    case 0x42e3c0: return (void *)s_add_flag;
    case 0x42e4f4: return (void *)s_clear_flag;
    case 0x52f904: return (void *)s_text_color;
    case 0x53793c: return (void *)s_bg;
    case 0x537868: return (void *)s_radius;
    case 0x52fa80: return (void *)s_set_text;
    case 0x52fcc4: return (void *)s_label_ctor;
    case 0x52fa04: return (void *)s_font;
    case 0x52f73c: return (void *)s_longmode;
    case 0x533dcc: return (void *)s_panel_ctor;
    case 0x4291a0: return (void *)s_add_event;
    case 0x429128: return (void *)s_event_user;
    case 0x433198: case 0x433134: return (void *)s_lv_noop;
    case 0x481428: return (void *)s_timer_create;
    case 0x481540: return (void *)s_timer_del;
    case 0x4bd900: case 0x4bd9ec: case 0x4b7ffc: return (void *)s_stock_form;
    case 0x4bc1d0: return (void *)s_stock_dots;
    case 0x42a8c0: return (void *)s_indev_act;
    case 0x42ad14: return (void *)s_indev_scroll_obj;
    case 0x42ace8: return (void *)s_indev_dir;
    case 0x5384a0: return (void *)s_get_scroll_x;
    case 0x538080: return (void *)s_scroll_to_x;
    case BG_SET_BG_IMG_SRC: return (void *)s_bg_img;
    case BG_SET_BORDER_WIDTH: return (void *)s_border_w;
    case BG_SET_BORDER_RGB: return (void *)s_border_rgb;
    case BG_LV_BORDER_OPA: return (void *)s_border_opa;
    }
    fprintf(stderr, "fake_lvgl: unmapped launcher address %#lx\n", (unsigned long)a);
    abort();
}
/* a fake TUGL form: +88 page scroller, +840 page byte, +848/+856 the stock page dots */
static char fake_form[1024];
/* Stock wrappers are allocated at their real sizes so ASan flags any write past them
 * (zte_topsw_devui 0x4bd62c/0x4bd66c: operator new(0x50) per page dot; 0x4bc3d0: new(0x30) scroller). */
#define STOCK_DOT_BYTES 0x50
#define STOCK_SCROLLER_BYTES 0x30
static void *fake_stock(size_t n) { void *w = calloc(1, n); struct fobj *f = fnew(w, NULL, FK_STOCK); f->flags = 0; return w; }
static void *fake_form_new(void) {
    memset(fake_form, 0, sizeof fake_form);
    P(fake_form, 88) = fake_stock(STOCK_SCROLLER_BYTES);
    W(P(fake_form, 88))->w = 1920; W(P(fake_form, 88))->h = 432;
    P(fake_form, 848) = fake_stock(STOCK_DOT_BYTES);
    P(fake_form, 856) = fake_stock(STOCK_DOT_BYTES);
    for (int i = 0; i < 256; i += 4) { fake_palette[i] = 30; fake_palette[i + 1] = 60; fake_palette[i + 2] = 90; }
    fake_palette[168] = 232; fake_palette[169] = 241; fake_palette[170] = 242;   /* active dot */
    fake_palette[172] = 80; fake_palette[173] = 96; fake_palette[174] = 120;    /* inactive dot */
    fake_palette[24] = 232; fake_palette[25] = 241; fake_palette[26] = 242;     /* text */
    fake_palette[12] = 14; fake_palette[13] = 32; fake_palette[14] = 54;        /* card */
    return fake_form;
}
/* Scene dump for tools/render_preview.py: one JSON object per live object, creation order (= z-order). */
static void json_out(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(f, "\\%c", *s);
        else if (*s == '\n') fputs("\\n", f);
        else if ((unsigned char)*s < 0x20) fprintf(f, "\\u%04x", *s);
        else fputc(*s, f);
    }
    fputc('"', f);
}
static void fake_dump(const char *path, int page, const char *title) {
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(2); }
    fprintf(f, "{\"page\":%d,\"page_x\":%d,\"bg_on\":%d,\"title\":", page, PAGE_X(page), bg_on);
    json_out(f, title);
    fprintf(f, ",\"scroller\":%d,\"objects\":[\n", W(P(fake_form, 88))->idx);
    int first = 1;
    for (int i = 0; i < nfobj; i++) {
        struct fobj *o = &fobjs[i];
        if (o->freed) continue;
        fprintf(f, "%s{\"i\":%d,\"p\":%d,\"k\":%d,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"font\":%d,\"r\":%d,\"hidden\":%d,", first ? "" : ",\n",
                o->idx, o->parent ? o->parent->idx : -1, o->kind, o->x, o->y, o->w, o->h, o->font, o->radius, !!(o->flags & HIDDEN));
        fprintf(f, "\"bg\":%s[%d,%d,%d,%d],\"tc\":[%d,%d,%d],\"bw\":%d,\"bc\":[%d,%d,%d],\"bo\":%d,\"img\":%d,\"text\":",
                o->has_bg ? "" : "null,\"_bg\":", o->bg[0], o->bg[1], o->bg[2], o->bg[3], o->has_tc ? o->tc[0] : 232, o->has_tc ? o->tc[1] : 241,
                o->has_tc ? o->tc[2] : 242, o->border_w, o->border[0], o->border[1], o->border[2], o->border_opa, o->bgimg);
        json_out(f, o->text);
        fputc('}', f);
        first = 0;
    }
    fprintf(f, "]}\n");
    fclose(f);
}
