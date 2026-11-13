/* Host-side tests of screen.c's non-LVGL logic against tests/mock_api.py.
 * Build: cc -DU60_API_HOST='"127.0.0.1"' -I.. test_logic.c -lpthread */
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

static int fails, passes;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
static char logbuf[1 << 20];
static void ctl(const char *path, const char *body) {
    char out[256];
    int st = http_req(mihomo_port, "POST", path, body ? body : "{}", NULL, 3000, out, sizeof out, 0);
    if (st != 200) { printf("mock control %s failed (%d)\n", path, st); exit(2); }
}
static const char *mock_log(void) {
    if (http_req(mihomo_port, "GET", "/__log", NULL, NULL, 3000, logbuf, sizeof logbuf, 0) != 200) { printf("no log\n"); exit(2); }
    return logbuf;
}
static int count_of(const char *hay, const char *needle) { int n = 0; for (const char *p = hay; (p = strstr(p, needle)); p++) n++; return n; }
static void reset_world(const char *scenario) {
    char b[96];
    snprintf(b, sizeof b, "{\"scenario\":\"%s\"}", scenario);
    ctl("/__reset", b);
    agent_token[0] = 0; rule_group_at = -1; rule_group[0] = 0;
    pthread_mutex_lock(&lock);
    busy_op = OP_NONE; busy_mode = busy_tun = -1; busy_name[0] = 0; note[0] = 0; note_until = 0;
    delay_count = 0; delay_at = -1;
    pthread_mutex_unlock(&lock);
    refresh_state();
    ctl("/__clearlog", NULL);
}
static int find_node(const char *name) { for (int i = 0; i < node_count; i++) if (!strcmp(nodes[i], name)) return i; return -1; }
static int64_t log_time_ms_of(const char *key) {   /* mock's t (epoch secs float) of the first entry with key */
    const char *l = mock_log(), *k = strstr(l, key);
    if (!k) return -1;
    const char *t = k; while (t > l && strncmp(t, "\"t\":", 4)) t--;
    return (int64_t)(strtod(t + 4, NULL) * 1000);
}
static int64_t wall_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

int main(int argc, char **argv) {
    mihomo_port = atoi(argv[1]); agent_port = atoi(argv[2]); test_pw_file = argv[3];
    pthread_once(&wake_once, wake_init);
    const char *SEL = RULE_GROUP_PREFERRED;

    /* --- pure helpers --- */
    char b[512];
    url_encode("🚀节点选择", b, sizeof b);
    CHECK(!strcmp(b, "%F0%9F%9A%80%E8%8A%82%E7%82%B9%E9%80%89%E6%8B%A9"), "url_encode emoji: %s", b);
    url_encode("A&B <x>/y?z#", b, sizeof b);
    CHECK(!strcmp(b, "A%26B%20%3Cx%3E%2Fy%3Fz%23"), "url_encode reserved: %s", b);
    CHECK(url_encode("abcdef", b, 4) == -1, "url_encode overflow rejected");
    json_escape("a\"b\\c\n", b, sizeof b);
    CHECK(!strcmp(b, "a\\\"b\\\\c\\u000a"), "json_escape: %s", b);
    CHECK(json_string("\"A\\u0026B \\u003cn\\u003e \\ud83d\\ude80 \\\"q\\\"\"", b, sizeof b) && !strcmp(b, "A&B <n> 🚀 \"q\""), "json_string escapes: %s", b);
    CHECK(json_string("\"unterminated", b, sizeof b) == NULL, "json_string unterminated");
    struct sink s = {b, 8, 0, 1, 0};
    sink_put(&s, "abcdef", 6); sink_put(&s, "ghij", 4); b[s.len] = 0;
    CHECK(!strcmp(b, "defghij"), "tail sink keeps last bytes: %s", b);

    /* --- rule mode: list comes from 🚀节点选择 (chunked body, '&' escaped by the mock) --- */
    reset_world("default");
    CHECK(have_state && !strcmp(mode_name, "rule") && tun_on == 1, "refresh rule/tun");
    CHECK(!strcmp(group_name, SEL), "rule group = 🚀节点选择, got %s", group_name);
    CHECK(node_count == 4 && find_node("A&B <node>") == 3 && find_node("DIRECT") < 0, "rule list (count %d)", node_count);
    CHECK(node_index == find_node("🇯🇵AWS日本05"), "now = 日本05");

    /* tap a node -> PUT /proxies/<encoded 🚀节点选择> then DELETE /connections */
    CHECK(!queue_tap(100 + node_index), "tapping the active node is ignored");
    CHECK(queue_tap(100 + find_node("🇭🇰HK 01")), "node tap accepted");
    CHECK(!queue_tap(100 + 0) && !queue_tap(11) && !queue_tap(3), "taps while busy are ignored");
    CHECK(busy_op == OP_NODE && !strcmp(busy_group, SEL) && wake_flag == 1, "busy state set + worker woken");
    apply_pending(); refresh_state();
    const char *l = mock_log();
    CHECK(strstr(l, "\"raw_path\":\"/proxies/%F0%9F%9A%80%E8%8A%82%E7%82%B9%E9%80%89%E6%8B%A9\""), "PUT used encoded group path");
    CHECK(strstr(l, "\"PUT /proxies/🚀节点选择\""), "PUT on 🚀节点选择 (not GLOBAL)");
    CHECK(strstr(l, "\"PUT /proxies/GLOBAL\"") && strstr(l, "\"PUT /proxies/🚀节点选择\"") < strstr(l, "\"PUT /proxies/GLOBAL\""), "rule mode: 🚀 first, then mirrored to GLOBAL");
    CHECK(strstr(l, "{\\\"name\\\":\\\"🇭🇰HK 01\\\"}"), "PUT body");
    CHECK(strstr(l, "\"conns\":0") && strstr(l, "DELETE /connections"), "connections closed");
    CHECK(strstr(l, "PUT /proxies") < strstr(l, "DELETE /connections"), "DELETE after PUT");
    CHECK(node_index == find_node("🇭🇰HK 01") && !note[0], "refresh shows new node, no note");

    /* node with JSON-special chars */
    ctl("/__clearlog", NULL);
    busy_op = OP_NONE; CHECK(queue_tap(100 + find_node("A&B <node>")), "tap A&B");
    apply_pending(); refresh_state();
    CHECK(node_index == find_node("A&B <node>") && !note[0], "A&B <node> selected");

    /* --- mode change via agent: login (password with quotes), PUT settings, DELETE connections --- */
    reset_world("default");
    busy_op = OP_NONE;
    CHECK(!queue_tap(10), "tapping active mode ignored");
    CHECK(queue_tap(11) && busy_mode == 1, "mode tap global");
    apply_pending(); refresh_state();
    l = mock_log();
    CHECK(count_of(l, "POST /api/auth/login") == 1 && strstr(l, "PUT /api/proxy/settings") && strstr(l, "{\\\"mode\\\":\\\"global\\\"}"), "agent login + PUT mode");
    CHECK(strstr(l, "\"auth\":\"Bearer tok-1\""), "bearer token sent");
    CHECK(strstr(l, "PUT /api/proxy/settings") < strstr(l, "DELETE /connections"), "DELETE after mode change");
    CHECK(!strcmp(mode_name, "global") && !strcmp(group_name, "GLOBAL"), "global mode reads GLOBAL (%s)", group_name);
    CHECK(find_node("🐟漏网之鱼") >= 0 && find_node("REJECT") < 0, "GLOBAL list");

    /* global mode node switch -> PUT /proxies/GLOBAL */
    ctl("/__clearlog", NULL); busy_op = OP_NONE;
    CHECK(queue_tap(100 + find_node("🇯🇵AWS日本05")), "tap in global");
    apply_pending(); refresh_state();
    l = mock_log();
    CHECK(strstr(l, "\"PUT /proxies/GLOBAL\"") && strstr(l, "DELETE /connections"), "global -> PUT GLOBAL + DELETE");

    /* expired token: 401 -> one re-login -> retry */
    ctl("/__expire", NULL); ctl("/__clearlog", NULL); busy_op = OP_NONE;
    CHECK(queue_tap(12), "tap direct");
    apply_pending(); refresh_state();
    l = mock_log();
    CHECK(count_of(l, "PUT /api/proxy/settings") == 2 && count_of(l, "POST /api/auth/login") == 1 && !strcmp(mode_name, "direct") && !note[0], "re-login on 401");
    CHECK(!strcmp(group_name, SEL), "direct mode shows the rule group (%s)", group_name);

    /* TUN toggle: no DELETE */
    ctl("/__clearlog", NULL); busy_op = OP_NONE;
    CHECK(queue_tap(3) && busy_tun == 0, "tun tap -> off");
    apply_pending(); refresh_state();
    l = mock_log();
    CHECK(strstr(l, "{\\\"tun\\\":false}") && !strstr(l, "DELETE /connections") && tun_on == 0, "tun off, no DELETE");

    /* --- failures --- */
    reset_world("default");
    ctl("/__fail", "{\"key\":\"PUT /proxies/🚀节点选择\",\"status\":500}");
    busy_op = OP_NONE; queue_tap(100 + find_node("🇭🇰HK 01"));
    apply_pending(); refresh_state();
    l = mock_log();
    CHECK(!strcmp(note, "节点切换失败") && note_until > now_ms() + 3000, "node failure note: %s", note);
    CHECK(!strstr(l, "DELETE /connections"), "no DELETE after failed PUT");
    CHECK(node_index == find_node("🇯🇵AWS日本05"), "screen reverts to real node");

    reset_world("default");
    ctl("/__fail", "{\"key\":\"PUT /proxies/🚀节点选择\",\"status\":\"hang\"}");
    busy_op = OP_NONE; queue_tap(100 + find_node("🇭🇰HK 01"));
    int64_t t0 = now_ms(); apply_pending(); int64_t dt = now_ms() - t0;
    printf("info: hang timeout %lld ms\n", (long long)dt);
    CHECK(!strcmp(note, "节点切换失败") && dt >= 2900 && dt < 4500, "hang -> timeout failure in %lld ms", (long long)dt);

    reset_world("default");
    ctl("/__fail", "{\"key\":\"PUT /api/proxy/settings\",\"status\":500}");
    busy_op = OP_NONE; queue_tap(11);
    apply_pending(); refresh_state();
    l = mock_log();
    CHECK(!strcmp(note, "模式切换失败") && count_of(l, "PUT /api/proxy/settings") == 1 && !strstr(l, "DELETE /connections") && !strcmp(mode_name, "rule"), "mode 500: fail, no retry, no DELETE, reverted");

    reset_world("default");
    ctl("/__fail", "{\"key\":\"PUT /api/proxy/settings\",\"status\":\"hang\"}");
    busy_op = OP_NONE; queue_tap(11);
    t0 = now_ms(); apply_pending(); dt = now_ms() - t0;
    l = mock_log();
    CHECK(!strcmp(note, "模式切换失败") && count_of(l, "PUT /api/proxy/settings") == 1 && dt < 9500, "mode timeout not retried (%lld ms)", (long long)dt);

    reset_world("default");
    ctl("/__fail", "{\"key\":\"DELETE /connections\",\"status\":500}");
    busy_op = OP_NONE; queue_tap(100 + find_node("🇭🇰HK 01"));
    apply_pending(); refresh_state();
    l = mock_log();
    CHECK(!strcmp(note, "已切换，旧连接未断开") && count_of(l, "DELETE /connections") == 2 && node_index == find_node("🇭🇰HK 01"), "DELETE failure: warning after 1 retry");

    busy_op = OP_NONE; queue_tap(11); apply_pending(); busy_op = OP_NONE; refresh_state();   /* now global */
    ctl("/__fail", "{\"key\":\"GET /configs\",\"status\":\"hang\"}");
    refresh_state();
    CHECK(have_state && !strcmp(group_name, "GLOBAL") && !strcmp(mode_name, "global") && tun_on == 1, "configs down: keeps last known mode/TUN + group (%s %s)", mode_name, group_name);

    /* --- chunked body with names split across chunk boundaries --- */
    reset_world("many");
    CHECK(node_count == 74 && node_index == find_node("🇺🇸美国 节点 57 & co") && find_node("🇺🇸美国 节点 69 & co") == 73, "chunked list intact (count %d idx %d)", node_count, node_index);
    { int bad = 0; for (int i = 4; i < node_count; i++) { char want[64]; snprintf(want, sizeof want, "🇺🇸美国 节点 %02d & co", i - 4); bad += strcmp(nodes[i], want) != 0; } CHECK(!bad, "all 70 chunked names exact"); }

    /* --- group resolution for other subscriptions --- */
    reset_world("no_preferred");
    CHECK(!strcmp(group_name, "🔰Proxy"), "MATCH -> 🐟漏网之鱼 -> 🔰Proxy, got %s", group_name);
    reset_world("no_match_selector");
    CHECK(!strcmp(group_name, "🔰Proxy"), "first Selector in GLOBAL.all, got %s", group_name);
    reset_world("no_selectors");
    CHECK(!strcmp(group_name, "GLOBAL") && have_state && node_count == 2, "no selectors -> GLOBAL, got %s", group_name);
    /* the cached group disappears (new subscription) */
    reset_world("default");
    ctl("/__reset", "{\"scenario\":\"no_preferred\"}");
    refresh_state();
    CHECK(have_state && !strcmp(group_name, "GLOBAL"), "vanished group -> GLOBAL this round (%s)", group_name);
    refresh_state();
    CHECK(!strcmp(group_name, "🔰Proxy"), "then re-resolved (%s)", group_name);


    /* ================= 全部测速 ================= */
    static struct view v;
    CHECK(delay_tone(-1) == 0 && delay_tone(1) == 1 && delay_tone(199) == 1 && delay_tone(200) == 2 && delay_tone(499) == 2 && delay_tone(500) == 3 && delay_tone(0) == 3, "delay_tone cut-offs");
    row_text("X", 0, b, sizeof b); CHECK(!strcmp(b, TXT_TIMEOUT "  X"), "row_text timeout: %s", b);
    row_text("X", 87, b, sizeof b); CHECK(!strcmp(b, "87ms  X"), "row_text ms: %s", b);
    row_text("X", -1, b, sizeof b); CHECK(!strcmp(b, "X"), "row_text none: %s", b);

    reset_world("default");
    build_view(&v);
    CHECK(!strcmp(v.test_text, TXT_TEST_ALL) && v.test_style == ST_NORMAL && v.row_tone[0] == 0 && !strcmp(v.row_text[1], "🇯🇵AWS日本05"), "no results before a test");
    CHECK(queue_tap(4) && busy_op == OP_TEST && !strcmp(busy_group, SEL) && test_count == 4, "test tap accepted (group %s, %d nodes)", busy_group, test_count);
    CHECK(!queue_tap(4) && !queue_tap(11) && !queue_tap(100), "re-taps ignored while testing");
    build_view(&v);
    CHECK(!strcmp(v.test_text, TXT_TESTING) && v.test_style == ST_PENDING && !strcmp(v.state_text, TXT_TESTING) && v.state_tone == 1, "测速中 shown immediately: %s / %s", v.test_text, v.state_text);
    apply_pending(); busy_op = OP_NONE; refresh_state();
    l = mock_log();
    CHECK(strstr(l, "\"raw_path\":\"/group/%F0%9F%9A%80%E8%8A%82%E7%82%B9%E9%80%89%E6%8B%A9/delay?url=https%3A%2F%2Fwww.gstatic.com%2Fgenerate_204\\u0026timeout=5000\""), "group delay URL (agent's HEALTH_URL, timeout=5000)");
    CHECK(count_of(l, "/delay\"") == 1, "one group call, no per-node calls");
    CHECK(!note[0], "no failure note");
    build_view(&v);
    int jp = find_node("🇯🇵AWS日本05"), hk = find_node("🇭🇰HK 01"), ab = find_node("A&B <node>"), au = find_node("♻️自动选择");
    CHECK(!strcmp(v.row_text[jp], "120ms  🇯🇵AWS日本05") && v.row_tone[jp] == 1 && v.row_style[jp] == ST_SELECTED, "row 日本: %s", v.row_text[jp]);
    CHECK(!strcmp(v.row_text[hk], "350ms  🇭🇰HK 01") && v.row_tone[hk] == 2, "row HK: %s", v.row_text[hk]);
    CHECK(!strcmp(v.row_text[ab], TXT_TIMEOUT "  A&B <node>") && v.row_tone[ab] == 3, "omitted node = timeout: %s", v.row_text[ab]);
    CHECK(!strcmp(v.row_text[au], "350ms  ♻️自动选择"), "nested group gets its delay: %s", v.row_text[au]);
    CHECK(!strcmp(v.node_text, "🇯🇵AWS日本05  120ms"), "current node line shows delay: %s", v.node_text);
    CHECK(!strcmp(v.test_text, TXT_TEST_ALL) && v.state_tone == 0, "button back to 全部测速");

    /* >=500 is red; results replaced by the next test */
    ctl("/__delays", "{\"delays\":{\"🇭🇰HK 01\":650}}");
    queue_tap(4); apply_pending(); busy_op = OP_NONE;
    build_view(&v);
    CHECK(!strcmp(v.row_text[hk], "650ms  🇭🇰HK 01") && v.row_tone[hk] == 3, "650ms red: %s", v.row_text[hk]);
    /* results survive refreshes and a node switch, but expire after 5 min */
    queue_tap(100 + hk); apply_pending(); busy_op = OP_NONE; refresh_state();
    build_view(&v);
    CHECK(v.row_style[hk] == ST_SELECTED && !strcmp(v.node_text, "🇭🇰HK 01  650ms"), "kept across node switch: %s", v.node_text);
    delay_at = now_ms() - DELAY_KEEP_MS - 1;
    build_view(&v);
    CHECK(!strcmp(v.row_text[jp], "🇯🇵AWS日本05") && v.row_tone[jp] == 0 && !strcmp(v.node_text, "🇭🇰HK 01"), "expired after 5 min");

    /* global mode tests GLOBAL */
    reset_world("default");
    queue_tap(11); apply_pending(); busy_op = OP_NONE; refresh_state(); ctl("/__clearlog", NULL);
    CHECK(queue_tap(4) && !strcmp(busy_group, "GLOBAL"), "global -> test GLOBAL");
    apply_pending(); busy_op = OP_NONE;
    CHECK(strstr(mock_log(), "\"GET /group/GLOBAL/delay\""), "GET /group/GLOBAL/delay");

    /* 504 from mihomo = everything timed out */
    reset_world("default");
    ctl("/__delays", "{\"group_status\":504}");
    queue_tap(4); apply_pending(); busy_op = OP_NONE;
    build_view(&v);
    CHECK(!note[0] && !strcmp(v.row_text[0], TXT_TIMEOUT "  ♻️自动选择") && v.row_tone[3] == 3, "504 -> all 超时: %s", v.row_text[0]);

    /* 500 -> 测速失败, nothing claimed */
    reset_world("default");
    ctl("/__delays", "{\"group_status\":500}");
    queue_tap(4); apply_pending(); busy_op = OP_NONE;
    build_view(&v);
    CHECK(!strcmp(note, TXT_TEST_FAIL) && v.state_tone == 2 && !strcmp(v.state_text, TXT_TEST_FAIL) && v.row_tone[1] == 0, "500 -> 测速失败 (%s)", v.state_text);

    /* hang -> overall ~8s timeout -> 测速失败 */
    reset_world("default");
    ctl("/__fail", "{\"key\":\"GET /group/🚀节点选择/delay\",\"status\":\"hang\"}");
    queue_tap(4);
    t0 = now_ms(); apply_pending(); dt = now_ms() - t0; busy_op = OP_NONE;
    printf("info: group delay hang -> %lld ms\n", (long long)dt);
    CHECK(!strcmp(note, TXT_TEST_FAIL) && dt >= 7900 && dt < 9500 && delay_count == 0, "group timeout ~8s (%lld ms)", (long long)dt);

    /* old mihomo without /group: per-node fallback */
    reset_world("no_group_api");
    queue_tap(4); apply_pending(); busy_op = OP_NONE;
    l = mock_log();
    CHECK(count_of(l, "\"GET /group/") == 1 && count_of(l, "/delay\"") == 5, "404 -> 4 per-node calls (%d)", count_of(l, "/delay\""));
    CHECK(strstr(l, "\"raw_path\":\"/proxies/%F0%9F%87%AF%F0%9F%87%B5AWS%E6%97%A5%E6%9C%AC05/delay?url=https%3A%2F%2Fwww.gstatic.com%2Fgenerate_204\\u0026timeout=5000\""), "per-node URL encoded");
    build_view(&v);
    CHECK(!note[0] && !strcmp(v.row_text[jp], "120ms  🇯🇵AWS日本05") && !strcmp(v.row_text[ab], TXT_TIMEOUT "  A&B <node>") && !strcmp(v.row_text[au], "350ms  ♻️自动选择"), "fallback results: %s | %s", v.row_text[jp], v.row_text[ab]);

    /* fallback when mihomo stops answering mid-way: rows tested so far keep results */
    reset_world("no_group_api");
    ctl("/__fail", "{\"key\":\"GET /proxies/🇭🇰HK 01/delay\",\"status\":\"hang\"}");
    queue_tap(4);
    t0 = now_ms(); apply_pending(); dt = now_ms() - t0; busy_op = OP_NONE;
    build_view(&v);
    CHECK(!note[0] && !strcmp(v.row_text[hk], TXT_TIMEOUT "  🇭🇰HK 01") && !strcmp(v.row_text[jp], "120ms  🇯🇵AWS日本05") && dt < 8000, "per-node hang = that node 超时 (%lld ms)", (long long)dt);

    /* no list yet -> tap ignored */
    pthread_mutex_lock(&lock); int saved = node_count; node_count = 0; pthread_mutex_unlock(&lock);
    CHECK(!queue_tap(4), "test tap ignored with an empty list");
    node_count = saved;

    /* pending node row format */
    reset_world("default");
    queue_tap(100 + hk); build_view(&v);
    CHECK(!strcmp(v.row_text[hk], "切换中... 🇭🇰HK 01") && v.row_style[hk] == ST_PENDING && v.mode_style[0] == ST_SELECTED, "pending row: %s", v.row_text[hk]);
    busy_op = OP_NONE;


    /* ================= node pick synced to both modes' groups ================= */
    /* device situation: global mode, GLOBAL=日本05, 🚀=新加坡03 */
    reset_world("split");
    CHECK(!strcmp(mode_name, "global") && !strcmp(group_name, "GLOBAL") && node_index == find_node("🇯🇵AWS日本05"), "split world: global shows GLOBAL");
    CHECK(queue_tap(100 + find_node("🇭🇰HK 01")), "tap HK in global");
    apply_pending(); busy_op = OP_NONE; refresh_state();
    l = mock_log();
    {
        const char *pg = strstr(l, "\"PUT /proxies/GLOBAL\""), *pr = strstr(l, "\"PUT /proxies/🚀节点选择\""), *dl = strstr(l, "DELETE /connections");
        CHECK(pg && pr && dl && pg < pr && pr < dl, "global tap: PUT GLOBAL, then 🚀, then DELETE");
    }
    CHECK(strstr(l, "\"GLOBAL\":\"🇭🇰HK 01\"") && strstr(l, "\"🚀节点选择\":\"🇭🇰HK 01\""), "both groups now HK");
    CHECK(!note[0] && node_index == find_node("🇭🇰HK 01"), "no notice, list shows HK");
    /* switching to rule now keeps HK (the old bug: rule traffic stayed on 新加坡03) */
    ctl("/__clearlog", NULL);
    queue_tap(10); apply_pending(); busy_op = OP_NONE; refresh_state();
    CHECK(!strcmp(mode_name, "rule") && !strcmp(group_name, SEL) && node_index == find_node("🇭🇰HK 01"), "after mode->rule: 🚀 list, current HK (%s)", group_name);

    /* tap in rule mode mirrors into GLOBAL */
    reset_world("default");
    queue_tap(100 + find_node("A&B <node>")); apply_pending(); busy_op = OP_NONE;
    l = mock_log();
    CHECK(strstr(l, "\"PUT /proxies/🚀节点选择\"") < strstr(l, "\"PUT /proxies/GLOBAL\"") && strstr(l, "\"GLOBAL\":\"A\\u0026B \\u003cnode\\u003e\""), "rule tap mirrored to GLOBAL");

    /* node only in 🚀 (新加坡03 is not in GLOBAL.all): no PUT on GLOBAL, no notice */
    reset_world("split");
    queue_tap(10); apply_pending(); busy_op = OP_NONE; refresh_state();   /* to rule */
    ctl("/__clearlog", NULL);
    queue_tap(100 + find_node("🇯🇵AWS日本05")); apply_pending(); busy_op = OP_NONE; refresh_state();
    ctl("/__clearlog", NULL);
    CHECK(queue_tap(100 + find_node("🇸🇬新加坡03")), "tap 新加坡03 (only in 🚀)");
    apply_pending(); busy_op = OP_NONE;
    l = mock_log();
    CHECK(strstr(l, "\"PUT /proxies/🚀节点选择\"") && !strstr(l, "\"PUT /proxies/GLOBAL\"") && strstr(l, "\"GET /proxies/GLOBAL\"") && !note[0], "secondary lacks node -> skipped silently");
    /* global mode: a group name in GLOBAL (🐟漏网之鱼) is not a member of 🚀 -> only GLOBAL */
    reset_world("split");
    queue_tap(100 + find_node("🐟漏网之鱼")); apply_pending(); busy_op = OP_NONE;
    l = mock_log();
    CHECK(strstr(l, "\"PUT /proxies/GLOBAL\"") && !strstr(l, "\"PUT /proxies/🚀节点选择\"") && !note[0], "group entry only switches GLOBAL");

    /* secondary PUT fails -> soft notice, not a failure; DELETE still done */
    reset_world("split");
    ctl("/__fail", "{\"key\":\"PUT /proxies/🚀节点选择\",\"status\":500}");
    queue_tap(100 + find_node("🇭🇰HK 01")); apply_pending(); busy_op = OP_NONE; refresh_state();
    l = mock_log();
    build_view(&v);
    CHECK(!strcmp(note, "已切换（另一模式未同步）") && v.state_tone == 3 && !strcmp(v.state_text, "已切换（另一模式未同步）") && strstr(l, "DELETE /connections") && node_index == find_node("🇭🇰HK 01"), "secondary failure -> soft notice (%s, tone %d)", note, v.state_tone);
    /* secondary membership GET fails -> soft notice too */
    reset_world("default");
    ctl("/__fail", "{\"key\":\"GET /proxies/GLOBAL\",\"status\":500}");
    queue_tap(100 + find_node("🇭🇰HK 01")); apply_pending(); busy_op = OP_NONE;
    CHECK(!strcmp(note, "已切换（另一模式未同步）") && note_soft == 1, "secondary GET failure -> soft notice");
    /* active PUT fails -> red failure, secondary untouched */
    reset_world("split");
    ctl("/__fail", "{\"key\":\"PUT /proxies/GLOBAL\",\"status\":500}");
    queue_tap(100 + find_node("🇭🇰HK 01")); apply_pending(); busy_op = OP_NONE;
    l = mock_log();
    build_view(&v);
    CHECK(!strcmp(note, "节点切换失败") && v.state_tone == 2 && !strstr(l, "\"PUT /proxies/🚀节点选择\"") && !strstr(l, "DELETE /connections"), "active failure -> secondary not touched");

    /* mode change: the list/current node follow the new mode right away, even with a stale cached group */
    reset_world("split");                                     /* global, GLOBAL list */
    snprintf(rule_group, sizeof rule_group, "🎯全球直连");      /* stale cache that would still be "fresh" */
    rule_group_at = now_ms();
    queue_tap(10); apply_pending(); busy_op = OP_NONE; refresh_state();
    CHECK(!strcmp(group_name, SEL) && node_index == find_node("🇸🇬新加坡03"), "global->rule: 🚀 list + 新加坡03 immediately (%s)", group_name);
    queue_tap(11); apply_pending(); busy_op = OP_NONE; refresh_state();
    CHECK(!strcmp(group_name, "GLOBAL") && node_index == find_node("🇯🇵AWS日本05"), "rule->global: GLOBAL list + 日本05 immediately (%s)", group_name);
    queue_tap(12); apply_pending(); busy_op = OP_NONE; refresh_state();
    CHECK(!strcmp(mode_name, "direct") && !strcmp(group_name, SEL), "->direct: rule group list (%s)", group_name);

    /* --- latency: the worker reacts to a tap at once instead of after sleep(2) --- */
    reset_world("default");
    pthread_t th; pthread_create(&th, NULL, worker, NULL);
    usleep(700000);                         /* worker is now parked in its 2s wait */
    ctl("/__clearlog", NULL);
    int64_t tap = wall_ms();
    CHECK(queue_tap(100 + find_node("🇭🇰HK 01")), "tap with live worker");
    int64_t done = -1;
    for (int i = 0; i < 100; i++) {
        usleep(20000);
        pthread_mutex_lock(&lock); int b2 = busy_op; pthread_mutex_unlock(&lock);
        if (b2 == OP_NONE) { done = wall_ms(); break; }
    }
    int64_t put_at = log_time_ms_of("PUT /proxies/");
    printf("info: tap->PUT %lld ms, tap->refreshed %lld ms\n", (long long)(put_at - tap), (long long)(done - tap));
    CHECK(put_at > 0 && put_at - tap < 150, "PUT reached mock %lld ms after tap", (long long)(put_at - tap));
    CHECK(done > 0 && done - tap < 1000, "busy cleared %lld ms after tap", (long long)(done - tap));
    pthread_mutex_lock(&lock);
    CHECK(node_index == find_node("🇭🇰HK 01"), "worker applied + refreshed");
    pthread_mutex_unlock(&lock);

    printf("%d passed, %d failed\n", passes, fails);
    return fails != 0;
}
