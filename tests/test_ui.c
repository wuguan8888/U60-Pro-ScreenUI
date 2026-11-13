/* Host tests of the overview / band-lock / cell-lock pages: screen.c is built with a recording fake
 * of the launcher's LVGL wrappers (fake_lvgl.h) and talks to tests/mock_api.py's agent model.
 * Also writes preview scenes (tests/preview/NAME.json) for tools/render_preview.py. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static const char *test_pw_file;
static FILE *test_fopen(const char *p, const char *m) {
    return fopen(!strcmp(p, "/data/local/tmp/start_zte_agent.sh") ? test_pw_file : p, m);
}
#define fopen test_fopen
#include "../screen.c"
#undef fopen
#include "fake_lvgl.h"

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char logbuf[1 << 21];
static void ctl(const char *path, const char *body) {
    char out[4096];
    int st = http_req(mihomo_port, "POST", path, body ? body : "{}", NULL, 3000, out, sizeof out, 0);
    if (st != 200) { printf("mock control %s failed (%d)\n", path, st); exit(2); }
}
static const char *mock_log(void) {
    if (http_req(mihomo_port, "GET", "/__log", NULL, NULL, 3000, logbuf, sizeof logbuf, 0) != 200) { printf("no log\n"); exit(2); }
    return logbuf;
}
static int count_of(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p++) n++; return n; }
static int posts(void) { return count_of(mock_log(), "\"key\":\"POST /api/cell/"); }
static void world(const char *preset) {
    char b[128];
    ctl("/__reset", "{\"scenario\":\"default\"}");
    snprintf(b, sizeof b, "{\"preset\":\"%s\"}", preset);
    ctl("/__radio", b);
    agent_token[0] = 0; login_retry_at = 0; rule_group_at = -1; rule_group[0] = 0;
    pthread_mutex_lock(&lock);
    lk_state = LS_IDLE; lk_confirmed = 0; lk_op.kind = LK_NONE; lk_polls = 0; lk_reason[0] = lk_readback[0] = 0;
    memset(drafts, 0, sizeof drafts); memset(seen_band, 0, sizeof seen_band); band_tab = 0; cell_sel_rat = 0; cell_sel_pci = -1; cell_sel_arfcn = -1; cell_last_req[0] = 0;
    busy_op = OP_NONE; note[0] = 0;
    sc_state = SC_IDLE; sc_req = 0; sc_n = 0; sc_calls = 0; sc_phase[0] = 0; sc_err[0] = 0;
    memset(sc_list, 0, sizeof sc_list);
    ubus_hook = NULL; scan_wait_hook = NULL;
    pthread_mutex_unlock(&lock);
    refresh_state(); refresh_caps(); refresh_dash(); refresh_clients();
    render_all();
    ctl("/__clearlog", NULL);
}
static int tap(int id) { int r = ui_tap(id); render_all(); return r; }
static int find_chip(const char *name) {
    for (int i = 0; i < BAND_CHIPS; i++) if (!(W(bd_chip[i])->flags & HIDDEN) && !strcmp(W(bd_chip_cap[i])->text, name)) return i;
    return -1;
}
static const char *T(void *o) { return W(o)->text; }
static int hidden(void *o) { for (struct fobj *f = W(o); f; f = f->parent) if (f->flags & HIDDEN) return 1; return 0; }
static int rgb_is(void *o, int r, int g, int b) { return W(o)->tc[0] == r && W(o)->tc[1] == g && W(o)->tc[2] == b; }
static void dump(const char *name, int pg, const char *title) {
    char path[256];
    snprintf(path, sizeof path, "preview/%s.json", name);
    goto_page(pg, 0);
    render_all();
    fake_dump(path, pg, title);
}
static int64_t wall_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
static void finish_verify(int max) { for (int i = 0; i < max; i++) { pthread_mutex_lock(&lock); int s = lk_state; pthread_mutex_unlock(&lock); if (s != LS_VERIFY) break; verify_step(); } render_all(); }

static int test_scan_n, test_get_lte_n, test_get_nr_n, test_wait_n, test_fail_scan, test_empty_both;
static const char *test_lte_blob, *test_nr_blob;
static int test_ubus_hook(const char *svc, const char *method, const char *args, char *out, size_t outlen, int timeout_s) {
    (void)args; (void)timeout_s;
    if (strcmp(svc, "zte_nwinfo_api")) return -1;
    if (!strcmp(method, "nwinfo_scan_nbr")) {
        test_scan_n++;
        if (test_fail_scan) return -1;
        snprintf(out, outlen, "{}");
        return 0;
    }
    if (!strcmp(method, "nwinfo_get_lte_nbr_contents")) {
        test_get_lte_n++;
        if (test_empty_both) { snprintf(out, outlen, "{\"lte_nbr_contents\":\"\"}"); return 0; }
        snprintf(out, outlen, "{\"lte_nbr_contents\":\"%s\"}", test_lte_blob ? test_lte_blob : "");
        return 0;
    }
    if (!strcmp(method, "nwinfo_get_nr5g_nbr_contents")) {
        test_get_nr_n++;
        if (test_empty_both) { snprintf(out, outlen, "{\"nr5g_nbr_contents\":\"\"}"); return 0; }
        snprintf(out, outlen, "{\"nr5g_nbr_contents\":\"%s\"}", test_nr_blob ? test_nr_blob : "");
        return 0;
    }
    return -1;
}
static void test_scan_wait(int ms) { (void)ms; test_wait_n++; }
static void test_scan_hook_reset(void) {
    test_scan_n = test_get_lte_n = test_get_nr_n = test_wait_n = 0;
    test_fail_scan = test_empty_both = 0;
    test_lte_blob = "253,3500,8,-75,-10,12;252,3500,8,-85,-12,5;";
    test_nr_blob = "412,627264,78,-86,-11,18;";
    ubus_hook = test_ubus_hook;
    scan_wait_hook = test_scan_wait;
}
static int sc_go(void) {
    if (!tap(42)) return 0;
    if (lk_state != LS_CONFIRM) return 0;
    if (!tap(51)) return 0;
    pthread_mutex_lock(&lock); int ok = sc_state == SC_RUN && sc_req; sc_req = 0; pthread_mutex_unlock(&lock);
    if (ok) scan_run();
    render_all();
    return ok;
}

/* ---------- 测速 (overview speed test) ---------- */
static void *st_bg(void *u) { (void)u; st_run(); return NULL; }
static int st_go(void) {          /* tap 测速 then run the job synchronously (no speed thread under the fake) */
    if (!tap(21)) return 0;
    pthread_mutex_lock(&lock); int ok = st_state == SP_RUN && st_req; st_req = 0; pthread_mutex_unlock(&lock);
    if (ok) st_run();
    render_all();
    return ok;
}
static void st_ready(void) { pthread_mutex_lock(&lock); st_cool_until = 0; pthread_mutex_unlock(&lock); render_all(); }
static const char *speedlog(void) {
    if (http_req(mihomo_port, "GET", "/__speedlog", NULL, NULL, 3000, logbuf, sizeof logbuf, 0) != 200) { printf("no speedlog\n"); exit(2); }
    return logbuf;
}
static void speed_tests(int dns_port) {
    if (!dns_port) { printf("FAIL: no dns port for speed tests\n"); fails++; return; }
    char u_blob[128], u_redir[128], u_dead[128], u_ip[128];
    snprintf(u_blob, sizeof u_blob, "http://speed.mock:%d/speed/blob", mihomo_port);
    snprintf(u_redir, sizeof u_redir, "http://speed.mock:%d/speed/redirect", mihomo_port);
    snprintf(u_dead, sizeof u_dead, "http://127.0.0.1:1/x");
    snprintf(u_ip, sizeof u_ip, "http://127.0.0.1:%d/speed/blob", mihomo_port);
    const char *urls[4] = { u_blob, NULL };
    FILE *rf = fopen("/tmp/u60-test-resolv.conf", "w");
    fprintf(rf, "# Interface zte_wan\nnameserver 127.0.0.1\n"); fclose(rf);
    const char *const *saved_urls = st_urls;
    st_urls = urls; st_resolv_path = "/tmp/u60-test-resolv.conf"; st_dns_port = dns_port; st_iface_override = "lo";
    st_ping_ip = "127.0.0.1"; st_ping_port = mihomo_port; st_max_ms = 1500; st_cooldown_ms = 400;
    world("lte"); goto_page(PG_OVERVIEW, 0); render_all();
    CHECK(!strcmp(T(ov_st_val), "—") && !strcmp(T(ov_st_btn_cap), "测速") && (W(ov_st_btn)->flags & CLICKABLE) && W(ov_st_btn)->user == 21 &&
          !strcmp(T(ov_st_note), "每次约消耗几十 MB 流量") && strstr(T(ov_st_detail), "不经代理"), "speed idle: %s / %s", T(ov_st_val), T(ov_st_detail));

    /* a normal run: 2 MB/s -> ~16 Mbps, 1.5 s cap, result + radio snapshot + history */
    ctl("/__speed", "{\"cfg\":{\"rate\":2000000}}");
    CHECK(tap(21), "tap 测速");
    pthread_mutex_lock(&lock); int run1 = st_state == SP_RUN && st_req == 1; pthread_mutex_unlock(&lock);
    CHECK(run1 && !strcmp(T(ov_st_btn_cap), "停止") && strstr(T(ov_st_note), "直连蜂窝"), "running: %s", T(ov_st_btn_cap));
    /* the band lock refuses while a test runs */
    goto_page(PG_BAND, 0); render_all();
    int chip = find_chip("B3"); if (chip < 0) chip = 0;
    tap(200 + chip);
    CHECK(!tap(33) && lk_state == LS_IDLE && hidden(dl_block), "apply refused during 测速");
    pthread_mutex_lock(&lock); memset(drafts, 0, sizeof drafts); pthread_mutex_unlock(&lock);
    goto_page(PG_OVERVIEW, 0);
    pthread_mutex_lock(&lock); st_req = 0; pthread_mutex_unlock(&lock);
    pthread_t th; int64_t t0 = now_ms();
    pthread_create(&th, NULL, st_bg, NULL);
    usleep(900 * 1000); render_all();
    CHECK(strstr(T(ov_st_val), "Mbps") && strstr(T(ov_st_detail), "下载中") && strstr(T(ov_st_detail), "MB") && rgb_is(ov_st_val, tone_rgbs[T_BLUE][0], tone_rgbs[T_BLUE][1], tone_rgbs[T_BLUE][2]),
          "live progress: %s / %s", T(ov_st_val), T(ov_st_detail));
    dump("f-speed-running", PG_OVERVIEW, "概览 · 测速中");
    pthread_join(th, NULL);
    int64_t took = now_ms() - t0;
    render_all();
    CHECK(st_state == SP_DONE && st_nhist == 1 && st_hist[0].mbps > 11 && st_hist[0].mbps < 20 && took < 3000 && st_hist[0].ms >= 1400 && st_hist[0].ms < 1700,
          "done: %.1f Mbps in %d ms (took %lld) err=%s", st_hist[0].mbps, st_hist[0].ms, (long long)took, st_err);
    CHECK(st_hist[0].ping_ms >= 0 && st_hist[0].ping_ms < 100 && !strcmp(st_hist[0].host, "speed.mock"), "latency %d ms host %s", st_hist[0].ping_ms, st_hist[0].host);
    CHECK(strstr(T(ov_st_val), " Mbps") && strstr(T(ov_st_detail), "延迟 ") && strstr(T(ov_st_radio), "PCI ") && strstr(T(ov_st_radio), "SINR") &&
          strstr(T(ov_st_radio), "LTE B3"), "result lines: %s / %s", T(ov_st_detail), T(ov_st_radio));
    CHECK(strstr(T(ov_st_note), "上次") && strstr(T(ov_st_note), "MB"), "data used: %s", T(ov_st_note));
    /* cooldown: button disabled with a countdown, taps ignored */
    CHECK(!(W(ov_st_btn)->flags & CLICKABLE) && strstr(T(ov_st_btn_cap), "秒后可测") && !tap(21) && st_state == SP_DONE, "cooldown: %s", T(ov_st_btn_cap));
    dump("f-speed-done", PG_OVERVIEW, "概览 · 测速结果");
    usleep(450 * 1000); render_all();
    CHECK((W(ov_st_btn)->flags & CLICKABLE) && !strcmp(T(ov_st_btn_cap), "测速"), "cooldown over");

    /* never two at once: a second tap while running is a stop, not a second test */
    ctl("/__speed", "{\"cfg\":{\"rate\":1000000}}");
    st_max_ms = 8000;
    CHECK(tap(21), "start 2");
    pthread_mutex_lock(&lock); st_req = 0; pthread_mutex_unlock(&lock);
    pthread_create(&th, NULL, st_bg, NULL);
    usleep(1300 * 1000);
    t0 = now_ms();
    CHECK(tap(21) && st_cancel && st_req == 0 && !strcmp(T(ov_st_btn_cap), "停止中…"), "second tap = stop (%s)", T(ov_st_btn_cap));
    pthread_join(th, NULL); took = now_ms() - t0; render_all();
    CHECK(st_state == SP_STOPPED && took < 800 && st_nhist == 2 && st_hist[0].stopped && strstr(T(ov_st_detail), "已停止"), "stopped in %lld ms: %s", (long long)took, T(ov_st_detail));
    CHECK(strstr(T(ov_st_hist), "之前 ") && strstr(T(ov_st_hist), "Mbps"), "history line: %s", T(ov_st_hist));
    /* stop during the first second: no result kept */
    st_ready();
    tap(21); pthread_mutex_lock(&lock); st_req = 0; pthread_mutex_unlock(&lock);
    pthread_create(&th, NULL, st_bg, NULL); usleep(300 * 1000); tap(21); pthread_join(th, NULL); render_all();
    CHECK(st_state == SP_STOPPED && st_nhist == 2 && !strcmp(T(ov_st_val), "已停止"), "early stop keeps no result (%d): %s", st_nhist, T(ov_st_val));
    st_max_ms = 1500;

    /* byte cap */
    st_ready(); st_max_bytes = 1000000;
    ctl("/__speed", "{\"cfg\":{\"rate\":50000000}}");
    CHECK(st_go() && st_state == SP_DONE && st_hist[0].bytes >= 1000000 && st_hist[0].bytes < 1100000 && st_hist[0].ms < 1500,
          "byte cap: %lld B in %d ms", (long long)st_hist[0].bytes, st_hist[0].ms);
    st_max_bytes = 50000000;

    /* fallbacks: connect refused -> next URL; redirect followed */
    st_ready(); ctl("/__speed", "{\"cfg\":{\"rate\":4000000}}");
    urls[0] = u_dead; urls[1] = u_redir; urls[2] = NULL;
    int ok = st_go();
    const char *sl = speedlog();
    CHECK(ok && st_state == SP_DONE && strstr(sl, "/speed/redirect") && strstr(sl, "/speed/blob") && !strcmp(st_hist[0].host, "speed.mock"),
          "fallback + redirect: %s %s", st_err, sl);
    /* HTTP error on every URL */
    st_ready(); ctl("/__speed", "{\"cfg\":{\"status\":503}}");
    urls[0] = u_blob; urls[1] = NULL;
    CHECK(st_go() && st_state == SP_FAIL && strstr(st_err, "HTTP 503") && !strcmp(T(ov_st_val), "测速失败") &&
          rgb_is(ov_st_val, tone_rgbs[T_BAD][0], tone_rgbs[T_BAD][1], tone_rgbs[T_BAD][2]) && strstr(T(ov_st_detail), "503"), "503: %s", T(ov_st_detail));
    /* slow headers: give up after 5 s, then the next URL works */
    st_ready(); ctl("/__speed", "{\"cfg\":{\"hang\":8}}");
    t0 = now_ms(); st_go(); took = now_ms() - t0;
    CHECK(st_state == SP_FAIL && took < 7000 && took > 4500, "header timeout %lld ms: %s", (long long)took, st_err);
    /* DNS: mihomo fake-ip answers are refused (they would go through the proxy) */
    st_ready(); ctl("/__speed", "{\"cfg\":{\"rate\":4000000},\"dns\":\"fake\"}");
    CHECK(st_go() && st_state == SP_FAIL && strstr(st_err, "解析") && !strstr(speedlog(), "/speed/blob"), "fake-ip refused: %s", st_err);
    st_ready(); ctl("/__speed", "{\"cfg\":{\"rate\":4000000},\"dns\":\"fake_first\"}");
    CHECK(st_go() && st_state == SP_DONE, "fake-ip skipped, real A used: %s", st_err);
    st_ready(); ctl("/__speed", "{\"cfg\":{\"rate\":4000000},\"dns\":\"fail\"}");
    CHECK(st_go() && st_state == SP_FAIL, "SERVFAIL -> fail: %s", st_err);
    st_ready(); urls[1] = u_ip;
    CHECK(st_go() && st_state == SP_DONE && !strcmp(st_hist[0].host, "127.0.0.1"), "DNS down -> next URL (IP literal): %s", st_err);
    urls[1] = NULL;
    /* cannot bind the WAN interface -> fail, never fall back to the default (proxied) route */
    st_ready(); ctl("/__speed", "{\"cfg\":{\"rate\":4000000}}");
    st_iface_override = "nosuch0";
    CHECK(st_go() && st_state == SP_FAIL && !strstr(speedlog(), "/speed/blob"), "unbindable interface fails: %s", st_err);
    st_iface_override = "lo";
    /* history keeps 5, newest first */
    st_cooldown_ms = 0; st_max_ms = 300;
    for (int i = 0; i < 6; i++) { st_ready(); st_go(); }
    CHECK(st_nhist == 5 && st_hist[0].wall >= st_hist[4].wall && count_of(T(ov_st_hist), " · ") == 3, "history 5: %s", T(ov_st_hist));
    /* WAN discovery: skip the mihomo TUN default and loopback; take the main-table default route */
    {
        char real[IFNAMSIZ] = "", line[256];
        FILE *pr = fopen("/proc/net/route", "r");
        while (pr && fgets(line, sizeof line, pr)) { char ifn[32]; unsigned dst; if (sscanf(line, "%31s %x", ifn, &dst) == 2 && dst == 0 && strcmp(ifn, "lo")) { snprintf(real, sizeof real, "%s", ifn); break; } }
        if (pr) fclose(pr);
        FILE *wf = fopen("/tmp/u60-test-route", "w");
        fprintf(wf, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                    "mihomo\t00000000\t010012C6\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"
                    "lo\t00000000\t00000000\t0001\t0\t0\t0\t00000000\t0\t0\t0\n"
                    "%s\t00000000\t3EB4AE0A\t0003\t0\t0\t0\t00000000\t0\t0\t0\n", real[0] ? real : "eth0");
        fclose(wf);
        const char *sr = st_route_path; st_route_path = "/tmp/u60-test-route"; st_iface_override = NULL;
        char ifn[IFNAMSIZ]; struct in_addr src;
        int w = st_wan(ifn, sizeof ifn, &src);
        CHECK(!real[0] || (w == 1 && !strcmp(ifn, real) && src.s_addr), "wan = %s (want %s, rc %d)", ifn, real, w);
        wf = fopen("/tmp/u60-test-route", "w");
        fprintf(wf, "Iface\tDestination\tGateway\nmihomo\t00000000\t010012C6\t0003\t0\t0\t0\t00000000\t0\t0\t0\n"); fclose(wf);
        CHECK(st_wan(ifn, sizeof ifn, &src) == 0, "only a TUN default -> no WAN, test refuses");
        st_route_path = sr; st_iface_override = "lo";
        unlink("/tmp/u60-test-route");
    }
    /* back to a clean idle overview */
    pthread_mutex_lock(&lock); st_state = SP_IDLE; st_nhist = 0; st_cool_until = 0; st_err[0] = 0; pthread_mutex_unlock(&lock);
    st_urls = saved_urls; st_max_ms = 10000; st_cooldown_ms = 30000; st_dns_port = 53; st_iface_override = NULL;
    ctl("/__speed", "{}");
    unlink("/tmp/u60-test-resolv.conf");
}

int main(int argc, char **argv) {
    mihomo_port = atoi(argv[1]); agent_port = atoi(argv[2]); test_pw_file = argv[3];
    int dns_port = argc > 4 ? atoi(argv[4]) : 0;
    pthread_once(&wake_once, wake_init);
    char b[512];
    struct bandlock bl;

    /* ---------- parsers (bands.ts / api.ts semantics) ---------- */
    parse_lte_lock("\"0x87e29a0e00df\"", &bl);
    CHECK(bl.kind == LK_LOCKED && bl.n > 10 && bl.bands[0] == 1 && has_band(bl.bands, bl.n, 40), "lte hex mask (n=%d)", bl.n);
    parse_lte_lock("\"5\"", &bl);   CHECK(bl.kind == LK_LOCKED && bl.n == 2 && bl.bands[0] == 1 && bl.bands[1] == 3, "lte decimal 5 = B1,B3");
    parse_lte_lock("\"0\"", &bl);   CHECK(bl.kind == LK_AUTO, "lte '0' automatic");
    parse_lte_lock("\"\"", &bl);    CHECK(bl.kind == LK_AUTO, "lte '' automatic");
    parse_lte_lock("5", &bl);       CHECK(bl.kind == LK_UNKNOWN, "lte non-string unknown");
    parse_lte_lock(NULL, &bl);      CHECK(bl.kind == LK_UNKNOWN, "lte missing unknown");
    parse_lte_lock("\"0x1ffffffffffffffffffffffffffffffff\"", &bl); CHECK(bl.kind == LK_UNKNOWN, "lte >128 bits unknown");
    parse_lte_lock("\"340282366920938463463374607431768211456\"", &bl); CHECK(bl.kind == LK_UNKNOWN, "lte decimal 2^128 unknown");
    parse_lte_lock("\"0x400000000000000000\"", &bl); CHECK(bl.kind == LK_LOCKED && bl.n == 1 && bl.bands[0] == 71, "lte B71 = bit 70");
    parse_lte_lock("\"zz\"", &bl);  CHECK(bl.kind == LK_UNKNOWN, "lte garbage unknown");
    parse_nr_lock("\"78, 41,78\"", &bl); CHECK(bl.kind == LK_LOCKED && bl.n == 2 && bl.bands[0] == 41 && bl.bands[1] == 78, "nr list sorted+dedup");
    parse_nr_lock("\"0\"", &bl);    CHECK(bl.kind == LK_AUTO, "nr '0' automatic");
    parse_nr_lock("\"41,,78\"", &bl); CHECK(bl.kind == LK_UNKNOWN, "nr empty token unknown");
    parse_nr_lock("\"2000\"", &bl); CHECK(bl.kind == LK_UNKNOWN, "nr >1024 unknown");
    CHECK(classify_network_type("SA") == NM_SA && classify_network_type("5G SA") == NM_SA && classify_network_type("NR5G-SA") == NM_SA, "SA");
    CHECK(classify_network_type("ENDC") == NM_NSA && classify_network_type("EN-DC") == NM_NSA && classify_network_type("5G NSA") == NM_NSA, "NSA");
    CHECK(classify_network_type("LTE") == NM_LTE && classify_network_type("4G") == NM_LTE && classify_network_type("lte-a") == NM_LTE, "LTE");
    CHECK(classify_network_type("5G") == NM_UNKNOWN && classify_network_type("SAM") == NM_UNKNOWN && classify_network_type("") == NM_UNKNOWN, "unknown");
    u128_dec(((unsigned __int128)1 << 70) | 5, b, sizeof b);
    CHECK(!strcmp(b, "1180591620717411303429"), "u128 decimal (B1,B3,B71): %s", b);
    double d; long long ll;
    CHECK(!jnum("\" -86 \"", &d) && d == -86 && jnum("\"-86dBm\"", &d) && jnum("null", &d) && !jnum("1e3", &d) && d == 1000, "finiteNumber semantics");
    CHECK(!jint("\"412\"", 0, 1007, &ll) && ll == 412 && jint("\"1008\"", 0, 1007, &ll) && jint("41.5", 0, 99, &ll), "intInRange semantics");
    const char *J = "{\"a\":{\"s\":\"x\\\"}y\",\"n\":[1,{\"q\":2}],\"b\":{\"c\":true}},\"z\":3}";
    CHECK(jbool(jpath(J, "a.b.c")) == 1 && !jint(jget(J, "z"), 0, 9, &ll) && ll == 3 && jarr_len(jpath(J, "a.n")) == 2, "json walker skips strings/nesting");
    CHECK(!strcmp("ab", (snprintf(b, sizeof b, "ab\xE5\x9B"), b[utf8_valid_len(b)] = 0, b)), "utf8 tail trimmed");

    /* ---------- create(): object pool budget, z-order, flags ---------- */
    bg_abi_ok = 1; bg_path = "../bg.bin";
    void *form = fake_form_new();
    world("sa");
    create(form);
    printf("info: objects overview %d proxy %d band %d cell %d overlay %d dots %d total %d / pool %d\n", obj_count_overview, obj_count_proxy,
           obj_count_band, obj_count_cell, obj_count_overlay, obj_count_dots, nalloc, POOL_MAX);
    CHECK(obj_count_overview <= BUDGET_OVERVIEW && obj_count_proxy <= BUDGET_PROXY && obj_count_band <= BUDGET_BAND && obj_count_cell <= BUDGET_CELL &&
          obj_count_overlay <= BUDGET_OVERLAY && obj_count_dots <= BUDGET_DOTS, "each page within its budget");
    CHECK(nalloc <= POOL_MAX && nalloc == obj_count_overview + obj_count_proxy + obj_count_band + obj_count_cell + obj_count_overlay + obj_count_dots,
          "pool %d <= %d", nalloc, POOL_MAX);
    CHECK(obj_count_proxy == BUDGET_PROXY, "proxy page object count unchanged (175): %d", obj_count_proxy);
    CHECK(bg_on == 1, "bg on for previews");
    int pages_last = W(cl_lock_cap)->idx;
    CHECK(W(rs_card)->idx > pages_last && W(dl_block)->idx > W(rs_card)->idx && W(dl_ok_cap)->idx > W(dl_block)->idx, "result card + dialog drawn above all pages");
    CHECK(W(dl_block)->parent == W(P(form, 88)) && W(rs_card)->parent == W(P(form, 88)), "overlay lives in the page scroller");
    CHECK((W(dl_block)->flags & CLICKABLE) && !(W(dl_block)->flags & (CHAIN_HOR | BUBBLE)) && W(dl_block)->has_cb && W(dl_block)->user == 53,
          "dialog backdrop swallows taps and drags");
    CHECK(W(ov_page)->x == 640 && W(page)->x == 960 && W(bd_page)->x == 1280 && W(cl_page)->x == 1600, "pages at 640/960/1280/1600");
    CHECK(!(W(bd_chip_dot[0])->flags & CLICKABLE) && !(W(bd_chip_bar[0])->flags & CLICKABLE), "chip decorations take no taps");
    int ids_ok = 1;
    for (int i = 0; i < BAND_CHIPS; i++) ids_ok &= W(bd_chip[i])->user == 200 + i && W(bd_chip_cap[i])->user == 200 + i;
    for (int i = 0; i < CELL_ROWS; i++) ids_ok &= W(cl_row[i])->user == 300 + i;
    CHECK(ids_ok && W(bd_apply)->user == 33 && W(bd_reset)->user == 32 && W(cl_lock)->user == 41 && W(cl_reset)->user == 40 &&
          W(cl_scan)->user == 42 && W(dl_ok)->user == 51 && W(dl_cancel)->user == 50 && W(ov_proxy)->user == 20, "tap ids wired");

    /* ---------- navigation: 6 pages, dots, swipe exclusion ---------- */
    goto_page(PG_CELL, 0);
    CHECK(fake_scroll_x == 1600 && *(unsigned char *)(fake_form + 840) == 6 && visible_page == PG_CELL, "goto page 6");
    int dots_ok = 1;
    for (int i = 0; i < PAGE_COUNT; i++) dots_ok &= !(W(dots[i])->flags & HIDDEN) && W(dots[i])->w == (i == 5 ? 8 : 6);
    CHECK(dots_ok, "6 dots, page 6 active");
    goto_page(PG_PROXY, 0);
    fake_scroll_x = PAGE_X(PG_PROXY) + 60; fake_scrolling = NULL;
    scroll_release(form);
    CHECK(current == PG_BAND && fake_scroll_x == PAGE_X(PG_BAND), "swipe left 60px -> band page");
    fake_scroll_x = PAGE_X(PG_BAND) - 10; scroll_release(form);
    CHECK(current == PG_BAND && fake_scroll_x == PAGE_X(PG_BAND), "10px drag snaps back");
    fake_scroll_x = PAGE_X(PG_BAND) + 80; fake_scrolling = P(chip_scroll, 8); scroll_release(form);
    CHECK(current == PG_BAND, "drag inside the chip list never flips the page");
    fake_scrolling = P(cell_list, 8); scroll_release(form);
    CHECK(current == PG_BAND, "drag inside the cell list never flips the page");
    fake_scrolling = NULL; fake_scroll_x = PAGE_X(PG_BAND);
    goto_page(PG_OVERVIEW, 0);
    CHECK(tap(20) && current == PG_PROXY, "overview proxy row jumps to the proxy page");

    /* regression (device reboot on swipe, build 0a0d4681): update_dots() used color() on the two
     * stock page dots, and color() cleared a byte of our per-object cache at +96+18 -- 34 bytes past
     * the 0x50-byte stock wrapper. The fake allocates stock wrappers at their real size, so ASan
     * aborts on any such write. Swipe across all 6 pages both ways, many times. */
    {
        int ok = 1;
        for (int round = 0; round < 20; round++)
            for (int k = 0; k < 2 * (PAGE_COUNT - 1); k++) {
                int from = current, dir = k < PAGE_COUNT - 1 ? 1 : -1;
                fake_scrolling = NULL; fake_scroll_x = PAGE_X(from) + 60 * dir;
                scroll_release(form); render_all();
                ok &= current == from + dir || (from == PAGE_COUNT && dir > 0) || (from == 1 && dir < 0);
                ok &= W(dots[0])->has_bg && W(dots[1])->has_bg;
                ok &= W(dots[current - 1])->w == 8;
            }
        CHECK(ok, "swipe through all pages (stock dots repainted, no write past stock wrappers)");
        CHECK(oc(dots[0]) != (struct objcache *)((char *)dots[0] + OBJ_CACHE) &&
              oc(dots[1]) != (struct objcache *)((char *)dots[1] + OBJ_CACHE), "stock dots never get an in-object cache");
        goto_page(PG_OVERVIEW, 0);
    }

    /* ---------- overview (SA) ---------- */
    world("sa");
    CHECK(!strcmp(T(ov_header), "5G SA · 中国移动"), "header: %s", T(ov_header));
    CHECK(!strcmp(T(ov_rsrp), "-86 dBm") && !strcmp(T(ov_rsrp_rate), "良好") && rgb_is(ov_rsrp_rate, 61, 214, 140), "rsrp %s %s", T(ov_rsrp), T(ov_rsrp_rate));
    CHECK(!strcmp(T(ov_sinr), "18 dB") && !strcmp(T(ov_sinr_rate), "良好"), "sinr %s %s", T(ov_sinr), T(ov_sinr_rate));
    CHECK(!strcmp(T(ov_carrier), "n78 · PCI 412 · ARFCN 627264 · 100 MHz") && rgb_is(ov_carrier, 183, 156, 255), "carrier %s", T(ov_carrier));
    CHECK(!strcmp(T(ov_dl), "182.4 Mbps") && !strcmp(T(ov_ul), "12.4 Mbps"), "speed x8: %s / %s", T(ov_dl), T(ov_ul));
    CHECK(!strcmp(T(ov_card_title[0]), "电量 充电") && !strcmp(T(ov_card_val[0]), "78 %"), "battery %s %s", T(ov_card_title[0]), T(ov_card_val[0]));
    CHECK(!strcmp(T(ov_card_val[1]), "41 °C") && !strcmp(T(ov_card_title[1]), "CPU 温度"), "temp %s", T(ov_card_val[1]));
    CHECK(!strcmp(T(ov_card_val[2]), "3.27 GB"), "usage today rx+tx: %s", T(ov_card_val[2]));
    CHECK(!strcmp(T(ov_card_val[3]), "4 台"), "clients: %s", T(ov_card_val[3]));
    CHECK(strstr(T(ov_proxy_cap), "代理  规则 · 🇯🇵AWS日本05 · TUN 开") != NULL, "proxy row: %s", T(ov_proxy_cap));
    int lit = 0; for (int i = 0; i < 5; i++) lit += W(ov_bars[i])->bg[1] == 214;
    CHECK(lit == 4, "4 signal bars lit (%d)", lit);
    struct dash dd; pthread_mutex_lock(&lock); dd = dash; pthread_mutex_unlock(&lock);
    CHECK(dd.nlte == 0 && dd.nnr == 3 && dd.prim == RAT_NR && !dd.nr[2].has_rsrp && dd.nr[1].pci == 118 && dd.nr[1].bandnum == 41,
          "SA: ghost LTE skipped, nrca PCC duplicate skipped, -140 SCC unmeasured (lte %d nr %d)", dd.nlte, dd.nnr);
    dump("a-overview", PG_OVERVIEW, "概览 (SA)");
    dump("b-proxy", PG_PROXY, "代理");
    speed_tests(dns_port);
    /* NSA: LTE anchor is the primary even though NR is up; numbers arrive as JSON numbers */
    world("nsa");
    pthread_mutex_lock(&lock); dd = dash; pthread_mutex_unlock(&lock);
    CHECK(dd.prim == RAT_LTE && dd.nlte == 2 && dd.lte[0].pci == 100 && dd.lte[0].arfcn == 1850 && !strcmp(dd.lte[0].bw, "20 MHz") &&
          dd.lte[1].pci == 200 && dd.lte[1].has_rsrp && dd.lte[1].rsrp == -97 && dd.nnr == 1, "NSA mapping");
    CHECK(!strcmp(T(ov_header), "5G NSA · 中国移动") && !strcmp(T(ov_rsrp), "-92 dBm") && !strcmp(T(ov_rsrp_rate), "一般") &&
          rgb_is(ov_rsrp_rate, 242, 181, 68) && !strcmp(T(ov_carrier), "B3 · PCI 100 · ARFCN 1850 · 20 MHz"), "NSA overview: %s / %s", T(ov_rsrp), T(ov_carrier));
    world("lte");
    CHECK(!strcmp(T(ov_rsrp_rate), "差") && rgb_is(ov_rsrp_rate, 240, 96, 90) && !strcmp(T(ov_sinr_rate), "差"), "LTE poor ratings");
    world("none");
    CHECK(!strcmp(T(ov_rsrp), "—") && !strcmp(T(ov_rsrp_rate), "不可用") && !strcmp(T(ov_carrier), "没有服务小区"), "no service");
    /* stale signal source */
    world("sa");
    ctl("/__radio", "{\"stale\":true}"); refresh_dash(); render_all();
    CHECK(!strcmp(T(ov_header), "数据已过期") && rgb_is(ov_rsrp, 110, 124, 140), "stale: header + grey numbers");
    /* agent errors: never keep old numbers */
    ctl("/__radio", "{\"stale\":false,\"battery\":null}"); refresh_dash(); render_all();
    CHECK(!strcmp(T(ov_card_val[0]), "—") && !strcmp(T(ov_card_title[0]), "电量"), "no battery -> —");
    ctl("/__fail", "{\"key\":\"GET /api/dashboard\",\"status\":503}"); refresh_dash(); render_all();
    CHECK(!strcmp(T(ov_header), "agent 返回异常") && !strcmp(T(ov_rsrp), "—") && !strcmp(T(ov_dl), "—") && !strcmp(T(ov_card_val[2]), "—"), "503 -> all —");
    int saved_port = agent_port;
    agent_port = 1; agent_token[0] = 0; login_retry_at = 0; refresh_dash(); render_all();
    CHECK(!strcmp(T(ov_header), "agent 登录失败") || !strcmp(T(ov_header), "agent 无响应"), "agent down: %s", T(ov_header));
    agent_port = saved_port;
    /* login failure backs off: no login storm from the 2 s poll */
    world("sa");
    ctl("/__fail", "{\"key\":\"POST /api/auth/login\",\"status\":401}");
    agent_token[0] = 0; login_retry_at = 0;
    for (int i = 0; i < 5; i++) refresh_dash();
    CHECK(count_of(mock_log(), "POST /api/auth/login") == 1 && dash_err == 2, "one login attempt then 30 s back-off (%d)", count_of(mock_log(), "POST /api/auth/login"));
    login_retry_at = 0;

    /* ---------- band page: chips, draft, confirm gating ---------- */
    world("sa");
    goto_page(PG_BAND, 0); render_all();
    int vis = 0; for (int i = 0; i < BAND_CHIPS; i++) vis += !(W(bd_chip[i])->flags & HIDDEN);
    CHECK(vis == 21 && find_chip("n78") >= 0 && find_chip("n1") == 0, "21 NR SA chips from capabilities (%d)", vis);
    CHECK(!hidden(bd_chip_dot[find_chip("n78")]) && !hidden(bd_chip_dot[find_chip("n41")]) && hidden(bd_chip_dot[find_chip("n1")]), "in-use dots on n78/n41");
    CHECK(!strcmp(T(bd_info1), "实际服务 n78 PCC · PCI 412 +n41 +n78") && !strcmp(T(bd_info2), "锁定 NR 自动 · LTE 自动"), "info: %s | %s", T(bd_info1), T(bd_info2));
    CHECK(!strcmp(T(bd_apply_cap), "应用锁频") && !(W(bd_apply)->flags & CLICKABLE) && hidden(bd_pill), "apply disabled without a draft");
    tap(31);
    vis = 0; for (int i = 0; i < BAND_CHIPS; i++) vis += !(W(bd_chip[i])->flags & HIDDEN);
    CHECK(vis == 24 && find_chip("B71") >= 0, "24 LTE chips (%d)", vis);
    tap(30);
    CHECK(tap(200 + find_chip("n41")) && tap(200 + find_chip("n78")), "chips toggle the draft");
    CHECK(!strcmp(T(bd_apply_cap), "应用锁频 (2)") && (W(bd_apply)->flags & CLICKABLE) && !strcmp(T(bd_pill), "草稿未下发") && !hidden(bd_pill),
          "apply (2) + draft pill: %s", T(bd_apply_cap));
    CHECK(W(bd_chip[find_chip("n41")])->bg[2] == 237, "draft chip blue");
    tap(200 + find_chip("n41")); tap(200 + find_chip("n41"));
    refresh_dash(); render_all();
    CHECK(!strcmp(T(bd_apply_cap), "应用锁频 (2)"), "draft survives a readback refresh");
    CHECK(posts() == 0, "no POST while editing");
    dump("c-bandlock", PG_BAND, "锁频（草稿 n41, n78）");
    /* confirm gating */
    CHECK(lock_step() == 0 && posts() == 0, "worker without a confirm: nothing sent");
    CHECK(tap(33) && lk_state == LS_CONFIRM && !hidden(dl_block) && W(dl_block)->x == PAGE_X(PG_BAND), "apply opens the dialog on this page");
    CHECK(!strcmp(T(dl_title), "锁定 NR 频段？") && strstr(T(dl_details), "只使用  n41, n78") && !strcmp(T(dl_lead), "只使用 n41, n78") && strstr(T(dl_details), "当前  自动 · 在用 n78") &&
          strstr(T(dl_cons), "断开") && strstr(T(dl_recov), "恢复自动"), "dialog text: %s", T(dl_details));
    CHECK(lock_step() == 0 && posts() == 0, "dialog open: still nothing sent");
    CHECK(!tap(32) && !tap(33) && !queue_tap(11) && !tap(200), "other actions refused while the dialog is open");
    fake_scroll_x = PAGE_X(PG_BAND) + 100; scroll_release(form);
    CHECK(current == PG_BAND && fake_scroll_x == PAGE_X(PG_BAND), "dialog pins the page");
    CHECK(tap(50) && lk_state == LS_IDLE && hidden(dl_block) && lock_step() == 0 && posts() == 0, "cancel: closed, nothing sent");
    tap(33);
    ctl("/__lockcfg", "{\"lag\":2}");
    CHECK(tap(51) && lk_state == LS_SENDING && !tap(51), "confirm arms once (second tap ignored)");
    CHECK(!strcmp(T(bd_apply_cap), "下发中…") && !hidden(rs_card) && !strcmp(T(rs_title), "正在下发…"), "sending state shown");
    CHECK(posts() == 0, "the UI thread never POSTs");
    CHECK(lock_step() == 1 && lock_step() == 0, "worker POSTs exactly once");
    const char *l = mock_log();
    CHECK(count_of(l, "POST /api/cell/band/nr") == 1 && strstr(l, "{\\\"nr5g_type\\\":\\\"SA\\\",\\\"nr5g_band\\\":\\\"41,78\\\"}"), "NR body as the agent expects");
    CHECK(lk_state == LS_VERIFY && lk_next_poll - now_ms() > 1500 && lk_next_poll - now_ms() <= 2000, "verify every 2 s");
    render_all();
    CHECK(!strcmp(T(rs_title), "已下发 · 等待设备确认") && strstr(T(rs_body), "第 0/15 次读回") && !strcmp(T(bd_apply_cap), "读回中…") &&
          !(W(bd_reset)->flags & CLICKABLE), "f1 sent/verifying: %s", T(rs_body));
    verify_step(); render_all();
    CHECK(lk_state == LS_VERIFY && lk_polls == 1 && strstr(T(rs_body), "第 1/15 次读回") && strstr(T(rs_body), "读回：NR 自动"), "poll 1: still old (%s)", T(rs_body));
    dump("f1-result-sent", PG_BAND, "已下发 · 等待设备确认");
    verify_step(); render_all();
    CHECK(lk_state == LS_OK && !strcmp(T(rs_title), "设备已确认") && strstr(T(rs_body), "NR 已锁 n41, n78") && !strcmp(T(bd_pill), "设备已确认"),
          "f2 confirmed after readback: %s", T(rs_body));
    CHECK(!drafts[0].edited && !strcmp(T(bd_apply_cap), "应用锁频") && !hidden(bd_chip_bar[find_chip("n78")]) && !strcmp(T(bd_info2), "锁定 NR 已锁 n41, n78 · LTE 自动"),
          "draft re-baselined to the readback");
    dump("f2-result-confirmed", PG_BAND, "设备已确认");
    lk_state_at -= RESULT_OK_MS + 1; render_all();
    CHECK(hidden(rs_card) && hidden(bd_pill), "confirmed result auto-dismisses after 5 s");
    CHECK(posts() == 1, "one POST in total");

    /* readback never changes -> failure after 15 polls (30 s) */
    world("sa");
    ctl("/__lockcfg", "{\"ignore\":true}");
    goto_page(PG_BAND, 0); render_all();
    tap(200 + find_chip("n78")); tap(33); tap(51); lock_step();
    finish_verify(40);
    CHECK(lk_state == LS_FAIL && lk_polls == 15 && strstr(lk_reason, "读回 30 秒后仍为「NR 自动」"), "f3 after 15 polls: %s", lk_reason);
    CHECK(!strcmp(T(rs_title), "未生效") && !hidden(rs_dismiss) && !strcmp(T(bd_apply_cap), "重试下发") && (W(bd_apply)->flags & CLICKABLE) &&
          (W(bd_reset)->flags & CLICKABLE) && !strcmp(T(bd_pill), "未生效"), "failed state: retry + reset available");
    CHECK(count_of(mock_log(), "POST /api/cell/band/nr") == 1, "no automatic retry");
    dump("f3-result-failed", PG_BAND, "未生效");
    CHECK(tap(33) && lk_state == LS_CONFIRM && lk_op.nb == 1 && lk_op.bands[0] == 78, "retry re-opens the confirm with the same snapshot");
    tap(50);
    CHECK(lk_state == LS_IDLE, "cancel retry");
    /* the dialog over the band page (preview) */
    ctl("/__lockcfg", "{\"ignore\":false}");
    world("sa"); goto_page(PG_BAND, 0); render_all();
    tap(200 + find_chip("n41")); tap(200 + find_chip("n78")); tap(33);
    dump("e-confirm", PG_BAND, "确认弹窗");
    tap(50);
    /* rejected by the agent (HTTP 503 {ok:false,error}) */
    world("sa"); goto_page(PG_BAND, 0); render_all();
    ctl("/__fail", "{\"key\":\"POST /api/cell/band/nr\",\"status\":503}");
    tap(200 + find_chip("n78")); tap(33); tap(51); lock_step(); render_all();
    CHECK(lk_state == LS_FAIL && strstr(lk_reason, "HTTP 503") && strstr(lk_reason, "injected") && !strcmp(T(rs_title), "下发失败") && posts() == 1,
          "agent error shown, no retry: %s", lk_reason);
    tap(52);
    CHECK(lk_state == LS_IDLE && hidden(rs_card), "知道了 dismisses");
    /* agent hangs: POST times out once, outcome unknown -> readback decides */
    world("sa"); goto_page(PG_BAND, 0); render_all();
    ctl("/__fail", "{\"key\":\"POST /api/cell/band/nr\",\"status\":\"hang\"}");
    lock_timeout_ms = 1200;
    tap(200 + find_chip("n78")); tap(33); tap(51);
    int64_t t0 = now_ms(); lock_step(); int64_t took = now_ms() - t0;
    CHECK(lk_state == LS_VERIFY && strstr(lk_reason, "没有应答") && took >= 1100 && took < 3000 && posts() == 1, "hang -> one POST, verify (%lld ms)", (long long)took);
    lock_timeout_ms = T_LOCK;
    /* agent unreachable: nothing sent */
    world("sa"); goto_page(PG_BAND, 0); render_all();
    tap(200 + find_chip("n78")); tap(33); tap(51);
    agent_port = 1; lock_step(); agent_port = saved_port;
    CHECK(lk_state == LS_FAIL && strstr(lk_reason, "未下发") && posts() == 0, "unreachable: %s", lk_reason);
    /* expired token: 401, not retried */
    world("sa"); goto_page(PG_BAND, 0); render_all();
    tap(200 + find_chip("n78")); tap(33); tap(51);
    ctl("/__expire", NULL); lock_step();
    CHECK(lk_state == LS_FAIL && strstr(lk_reason, "登录已过期") && posts() == 1 && !agent_token[0], "401 not retried: %s", lk_reason);
    /* LTE lock: u128 decimal mask accepted by the agent's validator, read back as the firmware's hex */
    world("lte"); goto_page(PG_BAND, 0); tap(31);
    tap(200 + find_chip("B3")); tap(200 + find_chip("B71")); tap(33);
    CHECK(strstr(T(dl_details), "只使用  B3, B71") && !strcmp(T(dl_title), "锁定 LTE 频段？"), "LTE dialog: %s", T(dl_details));
    tap(51); lock_step(); finish_verify(3);
    l = mock_log();
    CHECK(strstr(l, "\\\"lte_band_mask\\\":\\\"1180591620717411303428\\\"") && lk_state == LS_OK, "LTE B3+B71 mask + confirmed (%d)", lk_state);
    /* no intersection with the serving band -> extra warning */
    world("lte"); goto_page(PG_BAND, 0); tap(31);
    tap(200 + find_chip("B1")); tap(33);
    CHECK(strstr(T(dl_cons), "当前在用的 B3 不在所选范围内") != NULL, "no-overlap warning: %s", T(dl_cons));
    tap(50);
    /* band reset */
    world("sa");
    ctl("/__radio", "{\"set\":{\"nr5g_sa_band_lock\":\"78\",\"lte_band_lock\":\"0x4\"}}"); refresh_dash();
    goto_page(PG_BAND, 0); render_all();
    CHECK(!strcmp(T(bd_info2), "锁定 NR 已锁 n78 · LTE 已锁 B3"), "locked info: %s", T(bd_info2));
    tap(32);
    CHECK(!strcmp(T(dl_title), "恢复自动频段？") && strstr(T(dl_details), "当前  NR 已锁 n78 · LTE 已锁 B3"), "reset dialog: %s", T(dl_details));
    tap(51); lock_step(); finish_verify(3);
    l = mock_log();
    CHECK(count_of(l, "POST /api/cell/band/reset") == 1 && lk_state == LS_OK && !strcmp(T(bd_info2), "锁定 NR 自动（全部频段） · LTE 自动（全部频段）"), "band reset confirmed (device-style all-bands readback): %s", T(bd_info2));
    /* ---- regression: device incident 2026-11-13. Auto is reported as "all bands"; one tap on n78 must
     * mean "only n78", never "everything except n78". ---- */
    {
        static const char all_nr[] = "1,2,3,5,7,8,18,20,26,28,29,38,40,41,48,66,71,75,77,78,79";
        char body[256];
        world("sa");
        snprintf(body, sizeof body, "{\"set\":{\"nr5g_sa_band_lock\":\"%s\",\"lte_band_lock\":\"0x87e29a0e00df\"}}", all_nr);
        ctl("/__radio", body); refresh_dash();
        goto_page(PG_BAND, 0); render_all();
        int bars = 0; for (int i = 0; i < BAND_CHIPS; i++) bars += !hidden(bd_chip_bar[i]);
        CHECK(!strcmp(T(bd_legend), "点选要使用的频段（当前自动）") && bars == 0 && W(bd_chip[find_chip("n78")])->bg[2] != 237,
              "auto (all bands): empty draft, hint, no lock bars (%s, %d bars)", T(bd_legend), bars);
        CHECK(!strcmp(T(bd_info2), "锁定 NR 自动（全部频段） · LTE 自动（全部频段）"), "auto described: %s", T(bd_info2));
        CHECK(!strcmp(T(bd_apply_cap), "应用锁频") && !(W(bd_apply)->flags & CLICKABLE) && !tap(33) && lk_state == LS_IDLE,
              "empty draft: apply disabled, tap refused");
        tap(200 + find_chip("n78"));
        int nd = drafts[0].n;
        CHECK(drafts[0].edited && nd == 1 && drafts[0].bands[0] == 78 && W(bd_chip[find_chip("n78")])->bg[2] == 237 &&
              W(bd_chip[find_chip("n41")])->bg[2] != 237 && !strcmp(T(bd_apply_cap), "应用锁频 (1)") && !strcmp(T(bd_legend), "只用蓝底频段，其余全部排除"),
              "tap n78 selects ONLY n78 (draft %d, %s)", nd, T(bd_apply_cap));
        CHECK(tap(33) && lk_state == LS_CONFIRM && lk_op.nb == 1 && lk_op.bands[0] == 78, "confirm snapshot = {78}");
        CHECK(!strcmp(T(dl_lead), "只使用 n78") && strstr(T(dl_details), "只使用  n78\n") && strstr(T(dl_details), "排除  n41, n1, n2 等 20 个") &&
              strstr(T(dl_details), "当前  自动（全部频段）") && !strcmp(T(dl_warn), "！网络会中断"),
              "dialog says only n78: %s | %s | %s", T(dl_lead), T(dl_details), T(dl_warn));
        dump("e2-confirm-only-n78", PG_BAND, "只使用 n78");
        tap(51); lock_step();
        const char *lg = mock_log();
        CHECK(count_of(lg, "POST /api/cell/band/nr") == 1 && strstr(lg, "\\\"nr5g_band\\\":\\\"78\\\"}"), "POST body nr5g_band=\"78\"");
        finish_verify(3);
        CHECK(lk_state == LS_OK && !strcmp(T(bd_info2), "锁定 NR 已锁 n78 · LTE 自动（全部频段）") && !hidden(bd_chip_bar[find_chip("n78")]),
              "readback n78 only: %s", T(bd_info2));
        /* a band never seen serving -> red warning in the dialog */
        world("sa");
        ctl("/__radio", body); refresh_dash(); goto_page(PG_BAND, 0); render_all();
        tap(200 + find_chip("n79")); tap(33);
        CHECK(!strcmp(T(dl_warn), "！本机未见过该频段信号，可能断网") && rgb_is(dl_warn, 240, 96, 90) && strstr(T(dl_cons), "当前在用的 n78 不在所选范围内"),
              "unseen-band warning: %s", T(dl_warn));
        tap(50);
        /* seen earlier (served before) -> no red warning even if not serving now */
        seen_band[0][79] = 1; tap(33);
        CHECK(!strcmp(T(dl_warn), "！网络会中断") && rgb_is(dl_warn, 242, 181, 68), "seen band: default warning");
        tap(50);
        CHECK(posts() == 0, "nothing sent without confirm");
        /* LTE tab: same auto semantics with the device's 64-bit mask (all supported bands up to B48) */
        tap(31);
        bars = 0; for (int i = 0; i < BAND_CHIPS; i++) bars += !hidden(bd_chip_bar[i]);
        CHECK(!strcmp(T(bd_legend), "点选要使用的频段（当前自动）") && bars == 0 && !(W(bd_apply)->flags & CLICKABLE), "LTE auto (0x87e29a0e00df)");
        tap(200 + find_chip("B3"));
        CHECK(drafts[1].n == 1 && drafts[1].bands[0] == 3 && !strcmp(T(bd_apply_cap), "应用锁频 (1)"), "LTE tap B3 = only B3");
        tap(33);
        CHECK(!strcmp(T(dl_lead), "只使用 B3") && strstr(T(dl_details), "排除  B1, B2, B4 等 23 个"), "LTE dialog: %s", T(dl_details));
        tap(51); lock_step();
        CHECK(strstr(mock_log(), "\\\"lte_band_mask\\\":\\\"4\\\""), "LTE body mask = 4 (B3 only)");
        finish_verify(3);
        CHECK(lk_state == LS_OK, "LTE B3 confirmed");
        /* real subset: the draft starts as that subset, locked bands marked; deselecting all is refused */
        world("sa");
        ctl("/__radio", "{\"set\":{\"nr5g_sa_band_lock\":\"41,78\",\"lte_band_lock\":\"0x4\"}}"); refresh_dash();
        goto_page(PG_BAND, 0); render_all();
        CHECK(!strcmp(T(bd_legend), "绿点 在用 · 黄线 已锁 · 蓝底 草稿") && !hidden(bd_chip_bar[find_chip("n41")]) && !hidden(bd_chip_bar[find_chip("n78")]) &&
              hidden(bd_chip_bar[find_chip("n1")]) && W(bd_chip[find_chip("n41")])->bg[2] == 237, "subset: draft = lock, bars on n41/n78");
        tap(200 + find_chip("n41")); tap(200 + find_chip("n78"));
        CHECK(drafts[0].edited && drafts[0].n == 0 && !strcmp(T(bd_apply_cap), "至少选 1 个频段") && !(W(bd_apply)->flags & CLICKABLE) && !tap(33) &&
              lk_state == LS_IDLE && posts() == 0, "empty draft never sent (%s)", T(bd_apply_cap));
        tap(200 + find_chip("n78"));
        tap(33);
        CHECK(!strcmp(T(dl_lead), "只使用 n78") && strstr(T(dl_details), "当前  已锁 n41, n78"), "subset -> only n78: %s", T(dl_details));
        tap(50);
        tap(31);
        CHECK(!strcmp(T(bd_legend), "绿点 在用 · 黄线 已锁 · 蓝底 草稿") && !hidden(bd_chip_bar[find_chip("B3")]) && drafts[1].n == 0 &&
              W(bd_chip[find_chip("B3")])->bg[2] == 237, "LTE subset B3 kept as-is");
        tap(30);
    }
    /* NSA note on the NR tab */
    world("nsa"); goto_page(PG_BAND, 0); render_all();
    CHECK(strstr(T(bd_legend), "NSA") != NULL, "NSA note: %s", T(bd_legend));

    /* ---------- cell page ---------- */
    world("sa"); goto_page(PG_CELL, 0); render_all();
    int rows = 0; for (int i = 0; i < CELL_ROWS; i++) rows += !(W(cl_row[i])->flags & HIDDEN);
    CHECK(rows == 3 && !strcmp(T(cl_row_main[0]), "PCI 412  PCC") && !strcmp(T(cl_row_sub[1]), "n41 · ARFCN 504990") && !strcmp(T(cl_row_rsrp[1]), "-95 dBm") &&
          !strcmp(T(cl_row_rsrp[2]), "—"), "serving-cell candidates (%d): %s / %s", rows, T(cl_row_main[0]), T(cl_row_sub[1]));
    CHECK(!strcmp(T(cl_pcc), "PCI 412 · 627264 · n78") && !strcmp(T(cl_tag), "NR") && strstr(T(cl_caption), "服务载波") &&
          strstr(T(cl_scan_cap), "扫描"), "pcc: %s / %s", T(cl_pcc), T(cl_caption));
    CHECK(!strcmp(T(cl_lock_cap), "先选择一个小区") && !(W(cl_lock)->flags & CLICKABLE), "lock disabled until a row is picked");
    CHECK(tap(301) && !strcmp(T(cl_lock_cap), "锁定 PCI 118") && (W(cl_lock)->flags & CLICKABLE) && W(cl_row[1])->bg[2] == 237, "row picked");
    CHECK(posts() == 0, "picking a row sends nothing");
    dump("d-celllock", PG_CELL, "锁小区（选中 PCI 118）");
    tap(41);
    CHECK(lk_state == LS_CONFIRM && !strcmp(T(dl_title), "锁定到这个小区？") && strstr(T(dl_details), "PCI  118") && strstr(T(dl_details), "NR-ARFCN  504990") &&
          strstr(T(dl_details), "频段  n41") && W(dl_block)->x == PAGE_X(PG_CELL), "cell dialog: %s", T(dl_details));
    tap(51); lock_step();
    l = mock_log();
    CHECK(strstr(l, "{\\\"lock_nr_pci\\\":\\\"118\\\",\\\"lock_nr_earfcn\\\":\\\"504990\\\",\\\"lock_nr_cell_band\\\":\\\"41\\\"}") != NULL, "NR cell body");
    finish_verify(3);
    CHECK(lk_state == LS_OK && !strcmp(T(rs_title), "主小区已切到目标") && strstr(T(rs_body), "PCI 118 · 504990") && strstr(T(cl_lockline), "NR PCI 118 · 504990"),
          "cell lock read back as PCC: %s", T(rs_body));
    /* locking the cell that already serves: say we cannot tell */
    world("sa"); goto_page(PG_CELL, 0); render_all();
    tap(300); tap(41); tap(51); lock_step(); finish_verify(2);
    CHECK(lk_state == LS_OK && strstr(T(rs_body), "无法区分") != NULL, "preset target caveat");
    /* cell reset: accepted, no readback exists */
    world("sa"); goto_page(PG_CELL, 0); render_all();
    tap(40);
    CHECK(!strcmp(T(dl_title), "解除小区锁定？"), "cell reset dialog");
    tap(51); lock_step(); render_all();
    CHECK(lk_state == LS_SENT && !strcmp(T(rs_title), "已下发（无法读回）") && !hidden(rs_dismiss) && count_of(mock_log(), "POST /api/cell/lock/reset") == 1, "cell reset sent");
    tap(52);
    /* LTE cell lock body */
    world("nsa"); goto_page(PG_CELL, 0); render_all();
    int lte_row = -1; for (int i = 0; i < CELL_ROWS; i++) if (!(W(cl_row[i])->flags & HIDDEN) && strstr(T(cl_row_main[i]), "PCI 200")) lte_row = i;
    tap(300 + lte_row); tap(41); tap(51); lock_step();
    CHECK(strstr(mock_log(), "{\\\"lock_lte_pci\\\":\\\"200\\\",\\\"lock_lte_earfcn\\\":\\\"100\\\"}") != NULL, "LTE cell body");
    finish_verify(3);
    CHECK(lk_state == LS_OK, "LTE cell confirmed");
    /* a selected cell that stopped serving cannot be locked */
    world("sa"); goto_page(PG_CELL, 0); render_all();
    tap(301);
    ctl("/__radio", "{\"set\":{\"nrca\":\"\"}}"); refresh_dash(); render_all();
    CHECK(!tap(41) && lk_state == LS_IDLE && !strcmp(T(cl_lock_cap), "先选择一个小区"), "vanished selection -> no dialog");

    /* ---------- neighbour scan (ubus test double; never a real ubus) ---------- */
    {
        /* cancel confirm -> zero scans */
        world("sa"); goto_page(PG_CELL, 0); render_all();
        test_scan_hook_reset();
        CHECK(tap(42) && lk_state == LS_CONFIRM && !strcmp(T(dl_title), "扫描附近小区？") &&
              strstr(T(dl_warn), "5G SA") && strstr(T(dl_details), "不用单独扫频段") &&
              !strcmp(T(dl_ok_cap), "确认扫描") && W(dl_block)->x == PAGE_X(PG_CELL), "scan dialog: %s", T(dl_lead));
        dump("d2-scan-confirm", PG_CELL, "锁小区 · 扫描确认");
        CHECK(tap(50) && lk_state == LS_IDLE && hidden(dl_block) && test_scan_n == 0 && sc_calls == 0 && sc_state == SC_IDLE,
              "cancel scan confirm -> zero scans (%d)", test_scan_n);
        CHECK(posts() == 0, "cancel scan sends no lock POST");

        /* confirm -> exactly one scan, results render with band/PCI/RSRP, serving marked */
        world("sa"); goto_page(PG_CELL, 0); render_all();
        test_scan_hook_reset(); ctl("/__clearlog", NULL);
        CHECK(sc_go() && sc_state == SC_DONE && test_scan_n == 1 && test_get_lte_n >= 1 && test_get_nr_n >= 1 && sc_calls == 1,
              "confirm runs exactly one scan (scan=%d lte=%d nr=%d calls=%d state=%d)", test_scan_n, test_get_lte_n, test_get_nr_n, sc_calls, sc_state);
        /* serving (3) then scan results sorted stronger first: NR 412 -86, LTE 253 -75, LTE 252 -85 */
        int vis = 0, s253 = -1, s412 = -1, s252 = -1;
        for (int i = 0; i < CELL_ROWS; i++) if (!(W(cl_row[i])->flags & HIDDEN)) {
            vis++;
            if (strstr(T(cl_row_main[i]), "PCI 253")) s253 = i;
            if (strstr(T(cl_row_main[i]), "PCI 412") && strstr(T(cl_row_main[i]), "NR")) s412 = i;
            if (strstr(T(cl_row_main[i]), "PCI 252")) s252 = i;
        }
        CHECK(vis >= 6 && s253 > 2 && s412 > 2 && s252 > s253,
              "scan rows after serving (%d) 253=%d 412=%d 252=%d", vis, s253, s412, s252);
        CHECK(s253 >= 0 && strstr(T(cl_row_main[s253]), "LTE") && strstr(T(cl_row_sub[s253]), "B8") &&
              strstr(T(cl_row_sub[s253]), "3500") && !strcmp(T(cl_row_rsrp[s253]), "-75 dBm") &&
              strstr(T(cl_row_sub[s253]), "RSRQ") && strstr(T(cl_row_sub[s253]), "SINR"),
              "LTE B8 PCI 253 rsrp: %s / %s", T(cl_row_main[s253]), T(cl_row_sub[s253]));
        CHECK(s412 >= 0 && strstr(T(cl_row_main[s412]), "在用") && strstr(T(cl_row_sub[s412]), "n78"),
              "serving cell marked in scan list: %s", T(cl_row_main[s412]));
        CHECK(strstr(T(cl_caption), "扫描结果"), "caption after scan: %s", T(cl_caption));
        /* sort: -75 before -85 (253 before 252). 412 is -86, after 252 */
        CHECK(s253 < s252 && s252 < s412, "stronger first: 253(%d) 252(%d) 412(%d)", s253, s252, s412);
        dump("d4-scan-results", PG_CELL, "锁小区 · 扫描结果");

        /* select a scanned LTE cell and lock POST body */
        CHECK(tap(300 + s253) && strstr(T(cl_lock_cap), "锁定 PCI 253"), "pick scanned 253: %s", T(cl_lock_cap));
        CHECK(tap(41) && lk_state == LS_CONFIRM && strstr(T(dl_details), "PCI  253") && strstr(T(dl_details), "EARFCN  3500"),
              "lock dialog from scan row: %s", T(dl_details));
        tap(51); lock_step();
        CHECK(strstr(mock_log(), "{\\\"lock_lte_pci\\\":\\\"253\\\",\\\"lock_lte_earfcn\\\":\\\"3500\\\"}") != NULL, "scan-row LTE lock body");
        /* do not wait for verify here; dismiss by resetting world below */

        /* empty result */
        world("sa"); goto_page(PG_CELL, 0); render_all();
        test_scan_hook_reset(); test_empty_both = 1;
        CHECK(sc_go() && sc_state == SC_EMPTY && test_scan_n == 1 && strstr(T(cl_empty), "无附近小区") && !hidden(cl_empty),
              "empty scan: %s state=%d", T(cl_empty), sc_state);
        dump("d5-scan-empty", PG_CELL, "锁小区 · 无附近小区");

        /* error */
        world("sa"); goto_page(PG_CELL, 0); render_all();
        test_scan_hook_reset(); test_fail_scan = 1;
        CHECK(sc_go() && sc_state == SC_FAIL && test_scan_n == 1 && test_get_lte_n == 0 && test_get_nr_n == 0 &&
              strstr(T(cl_empty), "失败") && sc_calls == 1, "scan error: %s (gets %d/%d)", T(cl_empty), test_get_lte_n, test_get_nr_n);

        /* mutual exclusion vs lock and speed test */
        world("sa"); goto_page(PG_CELL, 0); render_all();
        test_scan_hook_reset();
        CHECK(tap(42) && lk_state == LS_CONFIRM, "open scan confirm for exclusion");
        goto_page(PG_OVERVIEW, 0); render_all();
        CHECK(!tap(21) && st_state != SP_RUN, "测速 refused while scan confirm is open");
        goto_page(PG_CELL, 0); render_all();
        /* confirm starts SC_RUN; do not call scan_run yet */
        CHECK(tap(51) && sc_state == SC_RUN && sc_req == 1 && hidden(dl_block), "scan running (not yet collected)");
        render_all();
        dump("d3-scan-running", PG_CELL, "锁小区 · 扫描中");
        CHECK(strstr(T(cl_scan_cap), "扫描中") && !(W(cl_scan)->flags & CLICKABLE), "scan button disabled while running");
        CHECK(!tap(42) && lk_state == LS_IDLE && test_scan_n == 0, "second scan refused while running (no ubus yet)");
        CHECK(!tap(40) && !tap(41), "lock/reset refused while scanning");
        goto_page(PG_OVERVIEW, 0); render_all();
        CHECK(!tap(21) && st_state != SP_RUN && strstr(T(ov_st_btn_cap), "扫描中"), "测速 refused while scanning: %s", T(ov_st_btn_cap));
        goto_page(PG_BAND, 0); render_all();
        /* apply may be disabled / refused */
        int apply_refused = !tap(33) || lk_state != LS_CONFIRM;
        CHECK(apply_refused && lk_state != LS_CONFIRM, "band apply refused while scanning");
        /* finish the pending scan once */
        pthread_mutex_lock(&lock); sc_req = 0; pthread_mutex_unlock(&lock);
        scan_run(); render_all();
        CHECK(sc_state == SC_DONE && test_scan_n == 1, "pending scan completes once");

        /* speed test running blocks a new scan */
        world("sa"); goto_page(PG_OVERVIEW, 0); render_all();
        test_scan_hook_reset();
        pthread_mutex_lock(&lock); st_cool_until = 0; pthread_mutex_unlock(&lock);
        CHECK(tap(21) && st_state == SP_RUN, "start 测速 to block scan");
        goto_page(PG_CELL, 0); render_all();
        CHECK(!tap(42) && lk_state == LS_IDLE && test_scan_n == 0 && sc_state == SC_IDLE, "scan refused during 测速");
        pthread_mutex_lock(&lock); st_req = 0; st_state = SP_IDLE; st_cool_until = 0; pthread_mutex_unlock(&lock);
        render_all();

        /* opening the cell page does not start a scan */
        world("sa"); test_scan_hook_reset();
        goto_page(PG_CELL, 0); render_all();
        CHECK(test_scan_n == 0 && sc_state == SC_IDLE && sc_calls == 0, "opening the page does not scan");

        ubus_hook = NULL; scan_wait_hook = NULL;
    }

    /* ---------- render cost: an unchanged view makes no LVGL calls ---------- */
    world("sa"); goto_page(PG_OVERVIEW, 0); render_all();
    long before = fake_setter_calls; render_all();
    CHECK(fake_setter_calls == before, "idle re-render is free (%ld calls)", fake_setter_calls - before);

    /* ---------- destroy + re-create keeps the pool exact ---------- */
    int total = nalloc;
    destroy(form);
    CHECK(nalloc == 0 && mainform == NULL, "destroy frees the pool");
    create(form);
    CHECK(nalloc == total, "re-create: same object count (%d)", nalloc);
    render_all();

    /* ---------- worker: polling cadence, modal pause, verify pacing ---------- */
    world("sa");
    pthread_t th; pthread_create(&th, NULL, worker, NULL);
    goto_page(PG_PROXY, 0);
    usleep(300000); ctl("/__clearlog", NULL);
    usleep(3000000);
    l = mock_log();
    int dash_n = count_of(l, "GET /api/dashboard"), cfg_n = count_of(l, "GET /configs");
    CHECK(dash_n <= 1 && cfg_n >= 1, "proxy page: dashboard every 10 s (%d in 3 s), proxy every 2 s (%d)", dash_n, cfg_n);
    goto_page(PG_OVERVIEW, 0);
    ctl("/__clearlog", NULL); usleep(4500000);
    dash_n = count_of(mock_log(), "GET /api/dashboard");
    CHECK(dash_n >= 2 && dash_n <= 4, "overview page: dashboard every 2 s (%d in 4.5 s)", dash_n);
    fake_modal = 1; usleep(300000); ctl("/__clearlog", NULL); usleep(2500000);
    dash_n = count_of(mock_log(), "GET /api/");
    CHECK(dash_n == 0, "modal/screen off: polling paused (%d)", dash_n);
    fake_modal = 0;
    pthread_mutex_lock(&lock); verify_every_ms = 150; pthread_mutex_unlock(&lock);
    ctl("/__lockcfg", "{\"ignore\":true}");
    goto_page(PG_BAND, 0); render_all();
    tap(200 + find_chip("n41")); tap(33);
    ctl("/__clearlog", NULL);
    int64_t start = wall_ms();
    tap(51);
    for (int i = 0; i < 100; i++) { usleep(50000); pthread_mutex_lock(&lock); int s = lk_state; pthread_mutex_unlock(&lock); if (s == LS_FAIL) break; }
    l = mock_log();
    int64_t post_at = 0;
    { const char *k = strstr(l, "POST /api/cell/band/nr"); const char *t = k; while (t && t > l && strncmp(t, "\"t\":", 4)) t--; if (k) post_at = (int64_t)(strtod(t + 4, NULL) * 1000); }
    CHECK(lk_state == LS_FAIL && count_of(l, "POST /api/cell/band/nr") == 1 && count_of(l, "GET /api/dashboard") == 15,
          "worker: 1 POST, exactly 15 readbacks (%d), then failure", count_of(l, "GET /api/dashboard"));
    CHECK(post_at - start < 200, "confirm -> POST %lld ms", (long long)(post_at - start));

    printf("%d passed, %d failed\n", passes, fails);
    return fails != 0;
}
