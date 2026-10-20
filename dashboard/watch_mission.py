import time, re, os, sys
LOG = r"C:\HD2VR\phase1\gateway\gateway.log"
dur = int(sys.argv[1]) if len(sys.argv) > 1 else 150
seen = os.path.getsize(LOG)
end = time.time() + dur
KEY = re.compile(r"operation|/api/mission|assignment|lobby|matchmak|reward|quickplay|ws recv|ws ->|dive|planet|campaign|extract|stratagem|proxy error|capture proxy failed|rawpipe|-> [45]\d\d|no ip for|accept fail|dns fail", re.I)
ERR = re.compile(r"proxy error|capture proxy failed|rawpipe.*(fail|error)|pipethread error|ws complete error|-> [45]\d\d|no ip for|accept fail|dns fail", re.I)
SKIP = re.compile(r"conn end|~ pipe|handshake complete|subscribed|/telemetry|x-reporterror|client_api_error_occurred|reporterrorassuccess", re.I)
errs = 0; acts = 0
print(f"[watch] start, {dur}s window, from offset {seen}", flush=True)
while time.time() < end:
    sz = os.path.getsize(LOG)
    if sz > seen:
        with open(LOG, "rb") as f:
            f.seek(seen); data = f.read(); seen = sz
        for ln in data.decode("utf-8", "ignore").splitlines():
            ln = ln.strip()
            if not ln or SKIP.search(ln): continue
            if KEY.search(ln):
                if ERR.search(ln):
                    errs += 1; print("ERR  " + ln, flush=True)
                else:
                    acts += 1; print("     " + ln, flush=True)
    time.sleep(3)
print(f"=== window ended: {acts} notable events, {errs} errors ===", flush=True)
