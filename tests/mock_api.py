#!/usr/bin/env python3
"""Mock of mihomo's controller (:MIHOMO) and the U60 agent (:AGENT) for screen.so logic tests.
Mimics Go: compact JSON, raw UTF-8, '&' '<' '>' escaped as \\u0026..., chunked bodies > 2048 B.
Control endpoints (mihomo port): POST /__reset {"scenario":..}, POST /__fail {"key":"PUT /proxies/x","status":500|"hang"},
GET /__log, POST /__expire (agent token invalid)."""
import json, sys, threading, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PASSWORD = "pw\\back&<x>"
NODE_SEL = "🚀节点选择"
lock = threading.Lock()
S = {}

def reset(scenario="default"):
    nodes = ["🇯🇵AWS日本05", "🇭🇰HK 01", "A&B <node>", "剩余流量：10G", "♻️自动选择"]
    groups = {
        "GLOBAL": {"type": "Selector", "now": "DIRECT", "all": ["DIRECT", "REJECT", NODE_SEL, "🐟漏网之鱼", "🎯全球直连", "♻️自动选择"] + nodes[:4]},
        NODE_SEL: {"type": "Selector", "now": "🇯🇵AWS日本05", "all": ["♻️自动选择", "🇯🇵AWS日本05", "🇭🇰HK 01", "A&B <node>", "DIRECT"]},
        "🐟漏网之鱼": {"type": "Selector", "now": NODE_SEL, "all": [NODE_SEL, "🎯全球直连", "DIRECT"]},
        "🎯全球直连": {"type": "Selector", "now": "DIRECT", "all": ["DIRECT", NODE_SEL]},
        "♻️自动选择": {"type": "URLTest", "now": "🇭🇰HK 01", "all": ["🇯🇵AWS日本05", "🇭🇰HK 01"]},
    }
    match = "🐟漏网之鱼"
    if scenario == "no_preferred":           # different subscription: MATCH -> 🐟 -> 🔰Proxy
        groups["🔰Proxy"] = groups.pop(NODE_SEL)
        groups["🐟漏网之鱼"]["now"] = "🔰Proxy"; groups["🐟漏网之鱼"]["all"][0] = "🔰Proxy"
        groups["GLOBAL"]["all"][2] = "🔰Proxy"
    elif scenario == "no_match_selector":    # MATCH -> DIRECT, no 🚀: first Selector in GLOBAL.all
        groups["🔰Proxy"] = groups.pop(NODE_SEL)
        groups["GLOBAL"]["all"][2] = "🔰Proxy"
        match = "DIRECT"
    elif scenario == "many":                 # > 2048 B group body -> chunked, names split across chunks
        extra = [f"🇺🇸美国 节点 {i:02d} & co" for i in range(70)]
        groups[NODE_SEL]["all"] += extra
        groups[NODE_SEL]["now"] = extra[57]
    elif scenario == "split":                # what the device showed: global mode, GLOBAL and 🚀 disagree
        groups["GLOBAL"]["now"] = "🇯🇵AWS日本05"
        groups[NODE_SEL]["all"].insert(3, "🇸🇬新加坡03")
        groups[NODE_SEL]["now"] = "🇸🇬新加坡03"
    elif scenario == "no_group_api":         # old mihomo: no /group/<name>/delay
        pass
    elif scenario == "no_selectors":
        groups = {"GLOBAL": groups["GLOBAL"]}
        groups["GLOBAL"]["all"] = ["DIRECT", "🇯🇵AWS日本05", "🇭🇰HK 01"]
        for k in list(groups):
            pass
        match = "DIRECT"
    rules = [{"index": i, "type": "DomainSuffix", "payload": f"example{i}.com", "proxy": NODE_SEL if i % 2 else "🎯全球直连", "size": -1,
              "extra": {"disabled": False, "hitCount": 0}} for i in range(1500)]
    rules.append({"index": 1500, "type": "Match", "payload": "", "proxy": match, "size": -1, "extra": {"disabled": False}})
    mode0 = "global" if scenario == "split" else "rule"
    S.clear()
    S.update(speed={}, speed_hits=[], dns_mode="ok", dns_queries=0)
    delays = {"🇯🇵AWS日本05": 120, "🇭🇰HK 01": 350, "A&B <node>": None, "DIRECT": 3}
    S.update(delays=delays, group_api=(scenario != "no_group_api"), group_status=None,
             mode=mode0, tun=True, groups=groups, rules=rules, log=[], fail={}, conns=5, token="tok-1", token_valid=True,
             logins=0, scenario=scenario)
    S.update(radio=radio_preset("sa"), pending=[], lock_lag=0, lock_ignore=False, slow={}, dash_reads=0, stale=False,
             battery=dict(BATTERY), clients=4, cell_lock=None)

# ---- agent radio model: the RAW zte_nwinfo_api nwinfo_get_netinfo object (agent/src/handlers.rs read_radio) ----
LTE_BANDS = [1, 2, 3, 4, 5, 7, 8, 18, 19, 20, 26, 28, 29, 32, 34, 38, 39, 40, 41, 42, 43, 48, 66, 71]      # agent/src/cell.rs
NR_BANDS = [1, 2, 3, 5, 7, 8, 18, 20, 26, 28, 29, 38, 40, 41, 48, 66, 71, 75, 77, 78, 79]
# what the real device reports after nwinfo_rest_band_rat (observed 2026-11-13): every NR band listed,
# LTE mask = every supported band up to B48 (the 64-bit firmware mask cannot hold B66/B71)
RESET_LOCKS = {"nr5g_sa_band_lock": ",".join(str(b) for b in NR_BANDS), "nr5g_nsa_band_lock": ",".join(str(b) for b in NR_BANDS),
               "lte_band_lock": hex(sum(1 << (b - 1) for b in LTE_BANDS if b <= 64))}
BATTERY = {"status": "Charging", "capacity": 78, "voltage_uv": 4105000, "current_ua": 512000, "temperature": 305, "external_power": True}
def radio_preset(name):
    base = {"network_provider_fullname": "中国移动", "network_provider": "CMCC", "signalbar": "4", "net_select": "Only_5G",
            "lte_band_lock": "0", "nr5g_sa_band_lock": "0", "nr5g_nsa_band_lock": "0", "cell_id": "0", "nr5g_cell_id": "1A2B3C4D5",
            "lte_rsrq": "-11", "lte_rssi": "-70", "nr5g_rsrq": "-11", "nr5g_rssi": "-60"}
    if name == "sa":     # SA: LTE fields are stale ghosts (active band is an NR band)
        base.update(network_type="SA", nr5g_action_band="n78", nr5g_action_channel="627264", nr5g_pci="412", nr5g_rsrp="-86",
                    nr5g_snr="18", nr5g_bandwidth="100",
                    nrca="1,412,2,78,627264,100,0,-86,-11,18,-60;1,118,2,41,504990,100,0,-95,-12,9,-70;0,77,1,78,630000,40,0,-140,-43,-23,0",
                    wan_active_band="n78", wan_active_channel="1850", lte_pci="100", lte_rsrp="-101", lte_snr="3", lteca="", ltecasig="")
    elif name == "nsa":  # EN-DC: LTE anchor is the serving cell; numbers as JSON numbers on purpose
        base.update(network_type="ENDC", net_select="WL_AND_5G", wan_active_band="B3", wan_active_channel=1850, lte_pci=100, lte_rsrp=-92,
                    lte_snr="12", lteca="100,3,0,1850,20;200,1,0,100,20", ltecasig="-97,-12,6,-66,0,2",
                    nr5g_action_band="n78", nr5g_action_channel="627264", nr5g_pci="412", nr5g_rsrp="-99", nr5g_snr="4",
                    nr5g_bandwidth="100", nrca="")
    elif name == "lte":
        base.update(network_type="LTE", net_select="Only_LTE", wan_active_band="B3", wan_active_channel="1850", lte_pci="100", lte_rsrp="-105",
                    lte_snr="-2", lteca="", ltecasig="", nr5g_action_band="", nr5g_action_channel="0", nr5g_pci="", nr5g_rsrp="", nr5g_snr="", nrca="")
    else:                # no service
        base.update(network_type="", signalbar="0", wan_active_band="", wan_active_channel="0", lte_pci="", lte_rsrp="0", lteca="",
                    nr5g_action_band="", nr5g_action_channel="0", nr5g_pci="", nrca="")
    return base
def apply_pending_radio(on_read):
    keep = []
    for p in S["pending"]:
        p["reads"] -= 1 if on_read else 0
        if p["reads"] <= 0: S["radio"].update(p["set"])
        else: keep.append(p)
    S["pending"] = keep
def radio_change(update):
    if S["lock_ignore"]: return
    if S["lock_lag"] <= 0: S["radio"].update(update)
    else: S["pending"].append({"reads": S["lock_lag"], "set": update})
def dashboard():
    S["dash_reads"] += 1
    apply_pending_radio(True)
    now = int(time.time() * 1000)
    fresh = lambda ttl, stale=False: {"sampled_at_ms": now - 300, "age_ms": 300, "ttl_ms": ttl, "stale": stale, "error": None}
    period = lambda rx, tx: {"tx_bytes": tx, "rx_bytes": rx, "time_secs": 7200, "tx_packets": 1000, "rx_packets": 2000}
    return {"device": {"model": "U60 Pro", "firmware": "MU5250_B31"}, "battery": dict(S["battery"]) if S["battery"] else None,
            "cpu": {"usage": 12.5}, "memory": {"total_kb": 1000000, "available_kb": 600000},
            "speed": {"rx_bytes": 123456789, "tx_bytes": 2345678, "rx_speed": 22800000.0, "tx_speed": 1550000.0,
                      "max_rx_speed": 50000000.0, "max_tx_speed": 6000000.0, "elapsed_ms": 1000},
            "data_usage": {"day": period(3300000000, 211000000), "month": period(53000000000, 2000000000), "cycle": period(1, 1),
                           "since_power_on": period(1, 1), "total": period(1, 1), "reset_day": 1, "reset_enabled": 1,
                           "clear_date_record": None, "next_clear_date": None},
            "signal": dict(S["radio"]), "wan": {"up": True, "uptime": 3600}, "wan6": {"up": False},
            "thermal": {"cpuss_temp": 41, "modem_temp": 44},
            "sources": {"signal": fresh(1000, S["stale"]), "wan": fresh(1000), "wan6": fresh(1000), "thermal": fresh(10000),
                        "data_usage": fresh(30000), "speed": fresh(1000)},
            "charge_control_error": None}
def bad(msg): return 400, {"ok": False, "error": msg}
def band_nr(v):          # agent/src/cell.rs NrBandLock::validate (deny_unknown_fields)
    if set(v) != {"nr5g_type", "nr5g_band"} or not all(isinstance(x, str) for x in v.values()): return bad("invalid NR band selection")
    if v["nr5g_type"] != "SA": return bad("this firmware only supports NR band locking in SA mode")
    try: bands = [int(b.strip()) for b in v["nr5g_band"].split(",")]
    except ValueError: return bad("band list must contain comma-separated numbers")
    if not bands or len(bands) > len(NR_BANDS): return bad("select at least one band")
    if len(set(bands)) != len(bands): return bad("band list contains duplicates")
    if any(b not in NR_BANDS for b in bands): return bad("NR selection contains a band not supported by U60 Pro firmware")
    radio_change({"nr5g_sa_band_lock": ",".join(str(b) for b in bands)})
    return 200, {"ok": True, "data": {"result": "success"}}
def band_lte(v):
    if set(v) != {"is_lte_band", "lte_band_mask", "is_gw_band", "gw_band_mask"} or not all(isinstance(x, str) for x in v.values()):
        return bad("invalid LTE band selection")
    if v["is_lte_band"] != "1" or v["is_gw_band"] != "0" or v["gw_band_mask"] != "0": return bad("LTE band request does not match the firmware contract")
    if not v["lte_band_mask"].isdigit(): return bad("lte_band_mask must be a decimal bitmask")
    mask = int(v["lte_band_mask"])
    allowed = sum(1 << (b - 1) for b in LTE_BANDS)
    if mask == 0 or mask & ~allowed or mask >= 1 << 128: return bad("LTE mask is empty or contains a band not supported by U60 Pro firmware")
    radio_change({"lte_band_lock": hex(mask)})          # firmware reports a hex string
    return 200, {"ok": True, "data": {"result": "success"}}
def cell_lock(v, nr):
    if not isinstance(v, dict): return bad("expected JSON object or array")
    keys = {"lock_nr_pci", "lock_nr_earfcn", "lock_nr_cell_band"} if nr else {"lock_lte_pci", "lock_lte_earfcn"}
    if set(v) != keys or not all(isinstance(x, str) and x.isdigit() for x in v.values()): return 503, {"ok": False, "error": "ubus: invalid argument"}
    S["cell_lock"] = dict(v)
    if nr: radio_change({"nr5g_pci": v["lock_nr_pci"], "nr5g_action_channel": v["lock_nr_earfcn"], "nr5g_action_band": "n" + v["lock_nr_cell_band"],
                         "nrca": ""})
    else: radio_change({"lte_pci": v["lock_lte_pci"], "wan_active_channel": v["lock_lte_earfcn"], "lteca": "", "ltecasig": ""})
    return 200, {"ok": True, "data": {"result": "success"}}

def gojson(o):
    s = json.dumps(o, ensure_ascii=False, separators=(",", ":"))
    return s.replace("&", "\\u0026").replace("<", "\\u003c").replace(">", "\\u003e").encode()

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""
    def send(self, code, obj=None):
        data = b"" if obj is None else gojson(obj)
        self.send_response(code)
        if obj is not None: self.send_header("Content-Type", "application/json; charset=utf-8")
        chunked = len(data) > 2048
        if chunked: self.send_header("Transfer-Encoding", "chunked")
        elif code != 204: self.send_header("Content-Length", str(len(data)))
        self.send_header("Connection", "close")
        self.end_headers()
        if chunked:
            for i in range(0, len(data), 1000):   # small, odd-sized chunks to split names mid-UTF-8
                part = data[i:i + 1000]
                self.wfile.write(b"%x\r\n" % len(part) + part + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif data: self.wfile.write(data)
        self.close_connection = True
    def handle_any(self, method):
        path = urllib.parse.unquote(self.path.split("?", 1)[0])
        self.query = urllib.parse.parse_qs(self.path.split("?", 1)[1]) if "?" in self.path else {}
        raw = self.body()
        port = self.server.server_address[1]
        key = f"{method} {path}"
        with lock:
            if not path.startswith("/__"):
                S["log"].append({"t": time.time(), "port": port, "key": key, "body": raw.decode("utf-8", "replace"),
                                 "auth": self.headers.get("Authorization", ""), "raw_path": self.path})
            f = S["fail"].get(key)
        if f == "hang": time.sleep(30); return
        if f: return self.send(f, {"ok": False, "error": "injected"} if port == self.server.agent_port else {"message": "injected"})
        if port == self.server.agent_port: return self.agent(method, path, raw)
        if path.startswith("/speed/"): return self.speed(path)
        with lock: return self.mihomo(method, path, raw)
    def speed(self, path):
        """speed-test file server (the screen's 测速): rate-limited body, redirect, errors, slow headers"""
        c = dict(S["speed"])
        S["speed_hits"].append({"path": path, "host": self.headers.get("Host", "")})
        if path == "/speed/redirect":
            self.send_response(302); self.send_header("Location", f"http://127.0.0.1:{self.server.server_address[1]}/speed/blob")
            self.send_header("Content-Length", "0"); self.send_header("Connection", "close"); self.end_headers(); self.close_connection = True
            return
        if c.get("status", 200) != 200: return self.send(c["status"], {"message": "injected"})
        if c.get("hang"): time.sleep(c["hang"]); return
        size, rate = c.get("size", 200000000), c.get("rate", 4000000)      # bytes, bytes/s
        self.send_response(200); self.send_header("Content-Length", str(size)); self.send_header("Connection", "close"); self.end_headers()
        sent, t0, chunk = 0, time.time(), b"\0" * 16384
        try:
            while sent < size:
                n = min(len(chunk), size - sent)
                self.wfile.write(chunk[:n]); sent += n
                ahead = sent / rate - (time.time() - t0)
                if ahead > 0: time.sleep(ahead)
        except (BrokenPipeError, ConnectionResetError): pass
        self.close_connection = True
    def agent(self, method, path, raw):
        if method == "POST" and path == "/api/auth/login":
            with lock:
                S["logins"] += 1
                if json.loads(raw).get("password") != PASSWORD: return self.send(401, {"ok": False})
                S["token"] = f"tok-{S['logins']}"; S["token_valid"] = True
                return self.send(200, {"ok": True, "data": {"token": S["token"]}, "token": S["token"]})
        if method == "PUT" and path == "/api/proxy/settings":
            with lock:
                if self.headers.get("Authorization") != "Bearer " + S["token"] or not S["token_valid"]:
                    return self.send(401, {"ok": False, "error": "unauthorized"})
                v = json.loads(raw)
                if "mode" in v: S["mode"] = v["mode"]
                if "tun" in v: S["tun"] = v["tun"]
            time.sleep(0.3)      # hot reload
            return self.send(200, {"ok": True})
        if path.startswith("/api/"):
            with lock:
                if self.headers.get("Authorization") != "Bearer " + S["token"] or not S["token_valid"]:
                    return self.send(401, {"ok": False, "error": "unauthorized"})
                slow = S["slow"].get(f"{method} {path}", 0)
            if slow: time.sleep(slow)
            with lock:
                if method == "GET" and path == "/api/dashboard": return self.send(200, {"ok": True, "data": dashboard()})
                if method == "GET" and path == "/api/network/clients":
                    cl = [{"mac": f"AA:BB:CC:00:00:0{i}", "ip": f"192.168.0.{100 + i}", "hostname": f"host{i}", "name": None, "medium": "wifi"}
                          for i in range(S["clients"])]
                    return self.send(200, {"ok": True, "data": {"clients": cl}})
                if method == "GET" and path == "/api/modem/capabilities":
                    return self.send(200, {"ok": True, "data": {"network_modes": [{"value": "Only_5G", "label": "5G SA only"}],
                                                                "lte_bands": LTE_BANDS, "nr_sa_bands": NR_BANDS, "nr_nsa_band_lock_supported": False}})
                if method == "POST" and path.startswith("/api/cell/"):
                    try: v = json.loads(raw) if raw else None
                    except ValueError: return self.send(*bad("invalid JSON"))
                    if path == "/api/cell/band/nr": return self.send(*band_nr(v if isinstance(v, dict) else {}))
                    if path == "/api/cell/band/lte": return self.send(*band_lte(v if isinstance(v, dict) else {}))
                    if path == "/api/cell/band/reset":
                        radio_change(RESET_LOCKS); return self.send(200, {"ok": True, "data": {}})
                    if path == "/api/cell/lock/nr": return self.send(*cell_lock(v, True))
                    if path == "/api/cell/lock/lte": return self.send(*cell_lock(v, False))
                    if path == "/api/cell/lock/reset": S["cell_lock"] = None; return self.send(200, {"ok": True, "data": {}})
        return self.send(404, {"ok": False})
    def mihomo(self, method, path, raw):
        if path == "/__speed": v = json.loads(raw or b"{}"); S["speed"] = v.get("cfg", {}); S["speed_hits"] = []; S["dns_mode"] = v.get("dns", "ok"); return self.send(200, {})
        if path == "/__speedlog": return self.send(200, {"hits": S["speed_hits"], "dns": S["dns_queries"]})
        if path == "/__reset": reset(json.loads(raw or b"{}").get("scenario", "default")); return self.send(200, {})
        if path == "/__fail": v = json.loads(raw); S["fail"][v["key"]] = v["status"]; return self.send(200, {})
        if path == "/__clearlog": S["log"] = []; return self.send(200, {})
        if path == "/__expire": S["token_valid"] = False; return self.send(200, {})
        if path == "/__radio":       # {"preset": "sa|nsa|lte|none", "set": {...}, "stale": bool, "battery": null}
            v = json.loads(raw or b"{}")
            if "preset" in v: S["radio"] = radio_preset(v["preset"])
            S["radio"].update(v.get("set", {}))
            for k in ("stale", "clients"):
                if k in v: S[k] = v[k]
            if "battery" in v: S["battery"] = v["battery"]
            return self.send(200, {})
        if path == "/__lockcfg":     # {"lag": reads before a lock shows in the readback, "ignore": never apply, "slow": {key: secs}}
            v = json.loads(raw or b"{}")
            if "lag" in v: S["lock_lag"] = v["lag"]
            if "ignore" in v: S["lock_ignore"] = v["ignore"]
            if "slow" in v: S["slow"] = v["slow"]
            return self.send(200, {})
        if path == "/__state": return self.send(200, {"radio": S["radio"], "dash_reads": S["dash_reads"], "cell_lock": S["cell_lock"], "logins": S["logins"]})
        if path == "/__log": return self.send(200, {"log": S["log"], "mode": S["mode"], "tun": S["tun"], "conns": S["conns"],
                                                    "now": {k: g["now"] for k, g in S["groups"].items()}})
        if method == "GET" and path == "/configs":
            return self.send(200, {"port": 0, "mixed-port": 7890, "mode": S["mode"], "log-level": "info", "ipv6": False,
                                   "tun": {"enable": S["tun"], "device": "mihomo", "stack": "Mixed"}})
        if method == "GET" and path.startswith("/group/") and path.endswith("/delay"):
            name = path[len("/group/"):-len("/delay")]
            if not S["group_api"]: return self.send(404, {"message": "not found"})
            if self.query.get("url") != ["https://www.gstatic.com/generate_204"] or self.query.get("timeout") != ["5000"]:
                return self.send(400, {"message": "bad query"})
            g = S["groups"].get(name)
            if not g: return self.send(404, {"message": "Resource not found"})
            if S["group_status"]: return self.send(S["group_status"], {"message": "Timeout"})
            out = {}
            for m in g["all"]:
                leaf = m
                for _ in range(5):
                    if leaf in S["groups"]: leaf = S["groups"][leaf]["now"]
                d = S["delays"].get(leaf, 800)
                if d is not None: out[m] = d
            return self.send(200, out)
        if method == "GET" and path.startswith("/proxies/") and path.endswith("/delay"):
            name = path[len("/proxies/"):-len("/delay")]
            if self.query.get("url") != ["https://www.gstatic.com/generate_204"]: return self.send(400, {"message": "bad query"})
            if name not in S["delays"] and name not in S["groups"] and not name.startswith("🇺🇸") and name != "♻️自动选择":
                return self.send(404, {"message": "Resource not found"})
            leaf = name
            for _ in range(5):
                if leaf in S["groups"]: leaf = S["groups"][leaf]["now"]
            d = S["delays"].get(leaf, 800)
            if d is None: return self.send(408, {"message": "Timeout"})
            return self.send(200, {"delay": d})
        if path == "/__delays":
            v = json.loads(raw); S["delays"].update(v.get("delays", {}))
            if "group_status" in v: S["group_status"] = v["group_status"]
            return self.send(200, {})
        if method == "GET" and path == "/rules": return self.send(200, {"rules": S["rules"]})
        if method == "DELETE" and path == "/connections": S["conns"] = 0; return self.send(204)
        if path.startswith("/proxies/"):
            name = path[len("/proxies/"):]
            g = S["groups"].get(name)
            if not g: return self.send(404, {"message": "Resource not found"})
            if method == "GET":
                return self.send(200, {"alive": True, "all": g["all"], "extra": {}, "hidden": False, "history": [],
                                       "icon": "", "name": name, "now": g["now"], "testUrl": "", "type": g["type"], "udp": True})
            if method == "PUT":
                if g["type"] != "Selector": return self.send(400, {"message": "Must be a Selector"})
                n = json.loads(raw).get("name")
                if n not in g["all"]: return self.send(400, {"message": "Selector update error: proxy not exist"})
                g["now"] = n
                return self.send(204)
        return self.send(404, {"message": "not found"})
    def do_GET(self): self.handle_any("GET")
    def do_PUT(self): self.handle_any("PUT")
    def do_POST(self): self.handle_any("POST")
    def do_DELETE(self): self.handle_any("DELETE")

def dns_server(port):
    """UDP DNS for the speed test: speed.mock -> 127.0.0.1; dns mode fake -> 198.18.0.7 (mihomo fake-ip),
    fail -> SERVFAIL. Answers use name compression like real resolvers."""
    import socket, struct
    u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); u.bind(("127.0.0.1", port))
    while True:
        q, a = u.recvfrom(1500)
        with lock: S["dns_queries"] += 1; mode = S["dns_mode"]
        if len(q) < 17: continue
        i, labels = 12, []
        while q[i]: labels.append(q[i + 1:i + 1 + q[i]].decode()); i += 1 + q[i]
        qend = i + 5
        name = ".".join(labels)
        if mode == "fail" or name != "speed.mock":
            u.sendto(q[:2] + b"\x81\x82" + q[4:6] + b"\0\0\0\0\0\0" + q[12:qend], a); continue
        ips = ["198.18.0.7"] if mode == "fake" else (["198.18.0.7", "127.0.0.1"] if mode == "fake_first" else ["127.0.0.1"])
        ans = b"".join(b"\xc0\x0c" + struct.pack(">HHIH", 1, 1, 60, 4) + socket.inet_aton(ip) for ip in ips)
        cname = b""
        u.sendto(q[:2] + b"\x81\x80" + q[4:6] + struct.pack(">HHH", len(ips), 0, 0) + q[12:qend] + cname + ans, a)

if __name__ == "__main__":
    mport, aport = int(sys.argv[1]), int(sys.argv[2])
    reset()
    if len(sys.argv) > 3: threading.Thread(target=dns_server, args=(int(sys.argv[3]),), daemon=True).start()
    servers = []
    for p in (mport, aport):
        srv = ThreadingHTTPServer(("127.0.0.1", p), H); srv.agent_port = aport; srv.daemon_threads = True
        servers.append(srv); threading.Thread(target=srv.serve_forever, daemon=True).start()
    print("ready", flush=True)
    threading.Event().wait()
