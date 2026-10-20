import time, re, os
LOG = r"C:\HD2VR\phase1\gateway\gateway.log"
seen = os.path.getsize(LOG)
end = time.time() + 150
HIT = re.compile(r"mission/end|/api/operation/.*end|debrief|reward|gamresult|gameresult|missionend|report|/end\b|complete", re.I)
ERR = re.compile(r"proxy error|capture proxy failed|rawpipe.*(fail|error)|-> [45]\d\d|no ip for", re.I)
found = False
print("[extract-watch] armed 150s", flush=True)
while time.time() < end:
    sz = os.path.getsize(LOG)
    if sz > seen:
        with open(LOG, "rb") as f:
            f.seek(seen); data = f.read(); seen = sz
        for ln in data.decode("utf-8", "ignore").splitlines():
            ln = ln.strip()
            if not ln: continue
            if HIT.search(ln) and "pes/getconfiguration" not in ln.lower():
                tag = "ERR  " if ERR.search(ln) else ">>>  "
                print(tag + ln, flush=True); found = True
            elif ERR.search(ln):
                print("ERR  " + ln, flush=True)
    if found and time.time() > end - 120:
        # keep listening ~30s after first hit to capture the whole debrief burst
        end = time.time() + 30;
    time.sleep(2)
print(f"=== extract-watch done (end-flow seen: {found}) ===", flush=True)
