#!/usr/bin/env python3
# HD2 mod dashboard backend.
# Hosts the mod engine (mod_engine.py) and serves the dashboard page. The page's Mods tab drives
# the engine; its Server tab is the original fake-server monitor (gateway log view).
# Run: python server.py   ->   http://127.0.0.1:8777
import json, os, re, subprocess, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import mod_engine

ENGINE = mod_engine.Engine()

LOG   = r"C:\HD2VR\phase1\gateway\gateway.log"
ERRLG = r"C:\HD2VR\phase1\gateway\gw-err.log"
PORT  = 8777
TAIL  = 500_000  # bytes of log tail to parse each poll

# component -> (what it does, in plain english)
ABOUT = {
    "Arrowhead": "Login, galactic war state, progression, friends, stats (api.live.prod) - faked from captured real data",
    "PlayFab":   "Steam login -> PlayFab session, profiles, title config (974a9.playfabapi.com) - faked",
    "PubSub":    "Lobby / SignalR realtime channel (Azure SignalR) - faked handshake + subscription",
    "PSN":       "Sony / PlayStation services (pss-cloud.net) - PROXIED to the real Sony servers",
    "Other":     "Uncategorized traffic",
}

TS = re.compile(r"^(\d\d:\d\d:\d\d\.\d\d\d)\s(.*)$", re.S)
REQ = re.compile(r"^>\s+(\S+)\s+(\S+)\s+host=(\S+)")
PROX = re.compile(r"->\s+(\d{3})")

def classify(host, path):
    h = (host or "").lower(); p = (path or "").lower()
    if "signalr" in h: return "PubSub"
    if "pss-cloud" in h: return "PSN"
    if "playfab" in h: return "PlayFab"
    if "api.live.prod" in h or "thehelldiversgame" in h: return "Arrowhead"
    if p.startswith("/client/") or p.startswith("/profile/") or "/gs_accounts" in p or "/gs_config" in p or p.startswith("/pubsub/"):
        return "PlayFab"
    if p.startswith("/api/"): return "Arrowhead"
    if p.startswith("/"): return "Other"
    return "Other"

def subsystem(path):
    p = (path or "").lower()
    if "/account/login" in p: return "login"
    if "/warseason" in p or "/newsfeed" in p or "/episode" in p or "/operation" in p: return "war"
    if "/progression" in p or "/seasonpass" in p or "/mission" in p or "itempackage" in p: return "progression"
    if "/friendsv2" in p: return "social"
    if "/stats" in p: return "stats"
    if "/configuration" in p or "/gs_config" in p: return "config"
    if "loginwithsteam" in p: return "login"
    if "/profile" in p: return "profile"
    if "/pubsub" in p or "negotiate" in p: return "pubsub"
    return "other"

def read_tail(path, n):
    try:
        sz = os.path.getsize(path)
        with open(path, "rb") as f:
            if sz > n: f.seek(sz - n)
            data = f.read()
        txt = data.decode("utf-8", "ignore")
        lines = txt.split("\n")
        if sz > n and lines: lines = lines[1:]  # drop partial first line
        return lines
    except Exception:
        return []

def is_error(content):
    c = content.lower()
    if c.startswith("conn end"): return False          # routine socket close
    if c.startswith("~ pipe"): return False             # routine passthrough byte counts
    if "handshake complete" in c or "subscribed" in c: return False
    if "error" in c or "fail" in c or "no ip for" in c: return True
    m = PROX.search(content)
    if m and int(m.group(1)) >= 400: return True
    return False

def proc_up(name):
    try:
        out = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {name}", "/NH"],
                             capture_output=True, text=True, timeout=5).stdout
        return name.lower() in out.lower()
    except Exception:
        return False

def build_status():
    lines = read_tail(LOG, TAIL)
    comps = {k: {"about": ABOUT[k], "requests": 0, "real": 0, "empty": 0,
                 "errors": 0, "last": "", "subs": {}, "recent": []} for k in ABOUT}
    feed, errors = [], []
    cur = None  # current request event awaiting an outcome
    for raw in lines:
        raw = raw.rstrip("\r")
        m = TS.match(raw)
        if not m: continue
        t, content = m.group(1), m.group(2)
        if content.startswith(">"):
            rm = REQ.match(content)
            if not rm:
                continue
            method, path, host = rm.group(1), rm.group(2), rm.group(3)
            comp = classify(host, path)
            cur = {"t": t, "method": method, "path": path, "host": host,
                   "comp": comp, "sub": subsystem(path), "outcome": "...", "cls": "pending"}
            c = comps[comp]
            c["requests"] += 1; c["last"] = t
            c["subs"][cur["sub"]] = c["subs"].get(cur["sub"], 0) + 1
            c["recent"].append(cur)
            c["recent"] = c["recent"][-12:]
            feed.append(cur); feed[:] = feed[-120:]
        elif content.startswith("<"):
            body = content[1:].strip()
            empty = body.startswith("DEFAULT") or body.startswith("GetTitleNews")
            if cur is not None:
                cur["outcome"] = body[:60]
                cur["cls"] = "empty" if empty else "real"
                comps[cur["comp"]]["empty" if empty else "real"] += 1
        elif content.startswith("~"):
            ev = content[1:].strip()
            comp = None
            if ev.lower().startswith("ws") or "signalr" in ev.lower() or "subscription" in ev.lower():
                comp = "PubSub"
            elif "pss-cloud" in ev.lower() or "psn" in ev.lower():
                comp = "PSN"
            elif cur is not None:
                comp = cur["comp"]
            if comp:
                comps[comp]["last"] = t
                if ev.lower().startswith("ws") or "signalr" in ev.lower() or "subscription" in ev.lower():
                    comps[comp]["recent"].append({"t": t, "method": "WS", "path": ev[:50],
                                                   "comp": comp, "sub": "pubsub",
                                                   "outcome": "", "cls": "real"})
                    comps[comp]["recent"] = comps[comp]["recent"][-12:]
            if is_error(content):
                e = {"t": t, "comp": comp or (cur["comp"] if cur else "Other"), "text": ev[:140]}
                errors.append(e); errors[:] = errors[-40:]
                if e["comp"] in comps: comps[e["comp"]]["errors"] += 1

    return {
        "generated": time.strftime("%H:%M:%S"),
        "gatewayUp": proc_up("gateway.exe"),
        "gameUp": proc_up("helldivers2.exe"),
        "components": comps,
        "feed": list(reversed(feed[-60:])),
        "errors": list(reversed(errors[-25:])),
    }

HTML = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html"), "r", encoding="utf-8").read() \
    if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), "dashboard.html")) else "<h1>dashboard.html missing</h1>"

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def _json(self, obj, code=200):
        payload = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Cache-Control", "no-store")
        self.end_headers(); self.wfile.write(payload)

    def do_GET(self):
        if self.path.startswith("/api/status"):
            self._json(build_status())
        elif self.path.startswith("/api/mods"):
            self._json(ENGINE.snapshot())
        else:
            body = HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.end_headers(); self.wfile.write(body)

    # POST /api/mods/<id>      body {"enabled": bool?, "values": {...}?}
    # POST /api/engine/rescan  re-find the game's tables
    # POST /api/engine/reapply re-apply every enabled mod (e.g. after the game rebuilt its text)
    # POST /api/engine/stop    stop the running action and clear the queue
    # POST /api/engine/stop/<id> stop just that mod (running or queued); it is undone and switched off
    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}")
        except Exception:
            return self._json({"error": "bad JSON"}, 400)
        parts = self.path.strip("/").split("/")
        try:
            if parts[:2] == ["api", "mods"] and len(parts) == 3:
                ENGINE.update(parts[2], body.get("enabled"), body.get("values"))
            elif self.path.startswith("/api/engine/rescan"):
                ENGINE.rescan()
            elif self.path.startswith("/api/engine/reapply"):
                ENGINE.reapply_all()
            elif parts[:3] == ["api", "engine", "stop"]:
                mod_id = parts[3] if len(parts) > 3 else None
                if mod_id is not None and mod_id not in mod_engine.MODS_BY_ID:
                    raise KeyError(mod_id)
                ENGINE.stop(mod_id)
            else:
                return self._json({"error": "unknown endpoint"}, 404)
        except KeyError:
            return self._json({"error": "unknown mod"}, 404)
        self._json(ENGINE.snapshot())

if __name__ == "__main__":
    ENGINE.start()
    print(f"HD2 dashboard -> http://127.0.0.1:{PORT}")
    ThreadingHTTPServer(("127.0.0.1", PORT), H).serve_forever()
