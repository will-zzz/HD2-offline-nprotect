"""HD2 mod engine: finds the game's data tables in live memory and applies or reverts mods.

server.py hosts it; the dashboard page is only an interface to it. Everything here edits the
memory of the running offline game (helldivers2.exe). Nothing touches any game file, and every
change disappears when the game exits.

Flow
  - A watchdog thread follows helldivers2.exe. On a new game process it discovers the tables,
    then applies every enabled mod.
  - Each write saves the original bytes first, so turning a mod off restores the game's values.
  - Settings persist in mod_settings.json next to this file. Every mod is off by default.
  - Long work (memory scans) runs on one job thread; the UI shows its progress and a log.

Adding a mod: append a Mod(...) to MODS with its params and an apply function. The UI renders it
from the schema; revert is automatic.
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import queue
import string
import struct
import subprocess
import threading
import time

# --------------------------------------------------------------------------------------------
# Win32 process memory access
# --------------------------------------------------------------------------------------------
k32 = ctypes.WinDLL("kernel32", use_last_error=True)

PROCESS_VM_OPERATION, PROCESS_VM_READ, PROCESS_VM_WRITE, PROCESS_QUERY_INFORMATION = 0x8, 0x10, 0x20, 0x400
MEM_COMMIT = 0x1000
PAGE_NOACCESS, PAGE_GUARD = 0x01, 0x100
PAGE_WRITABLE = 0x04 | 0x08 | 0x40 | 0x80   # READWRITE | WRITECOPY | EXECUTE_READWRITE | EXECUTE_WRITECOPY


class MEMORY_BASIC_INFORMATION(ctypes.Structure):
    _fields_ = [("BaseAddress", ctypes.c_void_p), ("AllocationBase", ctypes.c_void_p),
                ("AllocationProtect", wt.DWORD), ("PartitionId", wt.WORD), ("RegionSize", ctypes.c_size_t),
                ("State", wt.DWORD), ("Protect", wt.DWORD), ("Type", wt.DWORD)]


k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
k32.OpenProcess.restype = wt.HANDLE
k32.CloseHandle.argtypes = [wt.HANDLE]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MEMORY_BASIC_INFORMATION), ctypes.c_size_t]
k32.VirtualQueryEx.restype = ctypes.c_size_t


class Cancelled(Exception):
    """The user pressed Stop on the dashboard."""


def find_game_pid():
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq helldivers2.exe", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=5).stdout
        for line in out.splitlines():
            parts = line.split('","')
            if len(parts) > 1 and parts[0].strip('"').lower() == "helldivers2.exe":
                return int(parts[1].strip('"'))
    except Exception:
        pass
    return None


class Game:
    """An opened helldivers2.exe process."""

    CHUNK = 16 << 20

    def __init__(self, pid):
        self.pid = pid
        self.h = k32.OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_QUERY_INFORMATION, False, pid)
        if not self.h:
            raise OSError(f"OpenProcess({pid}) failed: {ctypes.get_last_error()}")

    def close(self):
        if self.h:
            k32.CloseHandle(self.h)
            self.h = None

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) and got.value == n:
            return buf.raw
        return None

    def write(self, addr, data):
        got = ctypes.c_size_t()
        return bool(k32.WriteProcessMemory(self.h, ctypes.c_void_p(addr), data, len(data), ctypes.byref(got))) and got.value == len(data)

    def writable_regions(self):
        """Yields (base, size, allocation base) for every committed, writable region."""
        mbi = MEMORY_BASIC_INFORMATION()
        addr = 0
        while addr < 0x7FFFFFFF0000:
            if not k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)):
                break
            base, size = mbi.BaseAddress or 0, mbi.RegionSize
            if mbi.State == MEM_COMMIT and not (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) and (mbi.Protect & PAGE_WRITABLE):
                yield base, size, mbi.AllocationBase or 0
            nxt = base + size
            if nxt <= addr:
                break
            addr = nxt

    def allocation_end(self, alloc):
        """End address of the allocation that starts at alloc (sum of its regions)."""
        mbi = MEMORY_BASIC_INFORMATION()
        addr = alloc
        while k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(mbi), ctypes.sizeof(mbi)) and (mbi.AllocationBase or 0) == alloc:
            addr = (mbi.BaseAddress or 0) + mbi.RegionSize
        return addr

    def _read_chunk(self, addr, n, buf):
        got = ctypes.c_size_t()
        if k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got)) and got.value:
            return buf.raw[:got.value]
        # A failed page fails the whole read: fall back to pages, zero-filling unreadable ones.
        out = bytearray(n)
        page = ctypes.create_string_buffer(4096)
        for off in range(0, n, 4096):
            m = min(4096, n - off)
            if k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr + off), page, m, ctypes.byref(got)):
                out[off:off + got.value] = page.raw[:got.value]
        return bytes(out)

    def scan(self, needles, progress=None, limit=50000, should_stop=None):
        """Find every occurrence of each byte string in writable memory. Returns {needle: [addr]}.
        Raises Cancelled between chunks once should_stop() returns True."""
        needles = [n for n in dict.fromkeys(needles) if n]
        hits = {n: [] for n in needles}
        if not needles:
            return hits
        overlap = max(len(n) for n in needles)
        regions = list(self.writable_regions())
        total = sum(size for _, size, _ in regions) or 1
        done = 0
        buf = ctypes.create_string_buffer(self.CHUNK + overlap)
        for base, size, _ in regions:
            for off in range(0, size, self.CHUNK):
                if should_stop and should_stop():
                    raise Cancelled()
                want = min(self.CHUNK + overlap, size - off)
                step = min(self.CHUNK, size - off)
                data = self._read_chunk(base + off, want, buf)
                for n in needles:
                    lst = hits[n]
                    i = data.find(n)
                    while i != -1 and i < step and len(lst) < limit:
                        lst.append(base + off + i)
                        i = data.find(n, i + 1)
                done += step
                if progress:
                    progress(done / total)
        return hits


u32 = lambda b, o: struct.unpack_from("<I", b, o)[0]
f32 = lambda b, o: struct.unpack_from("<f", b, o)[0]
p_u32 = lambda v: struct.pack("<I", int(v))
p_f32 = lambda v: struct.pack("<f", float(v))

# --------------------------------------------------------------------------------------------
# Game knowledge (see vrmod/STRATAGEM_NOTES.md)
# --------------------------------------------------------------------------------------------
ATTACK_ROW = 0x110                      # attack table row size
BOMB_MODEL_HASH = 0x71E52F800636B336    # Eagle bomb unit; sits at row +0xAC on Eagle rows
GRENADE_MODEL_HASH = 0xA6CD8A90185B6567 # grenade-launcher projectile model (GP-31 and others)
ATTACK_EAGLE_AIRSTRIKE = 170            # row +0x2C id (ids repeat across rows, so pair with the model)
ATTACK_EAGLE_500KG = 239
ATTACK_GP31 = 263                       # GP-31 Grenade Pistol (found by firing it at test explosions)
ATTACK_COYOTE = 138                     # AR-2 Coyote bullets (row has no model and no body)
BODY_500KG = 193                        # +0xBC payload body that carries an explosion in +0xC8
TRIGGER = 0.8                           # +0xC4: must be non-zero or the +0xC8 explosion never fires

# Explosion IDs (projectile/explosion table). 111 confirmed Hellbomb in game, 277 confirmed 500kg.
EXPLOSIONS = [
    {"value": 111, "label": "Hellbomb (111)"},
    {"value": 277, "label": "500kg bomb (277)"},
    {"value": 102, "label": "500kg-class (102)"},
    {"value": 169, "label": "500kg-class (169)"},
    {"value": 293, "label": "Largest known, untested (293)"},
]

# --------------------------------------------------------------------------------------------
# Mods
# --------------------------------------------------------------------------------------------
class ModError(Exception):
    pass


class Mod:
    def __init__(self, id, name, group, desc, params, apply, slow=False, needs_game=True, revert=None):
        self.id, self.name, self.group, self.desc = id, name, group, desc
        self.params = params          # [{key,label,type,default,options?,max?}]
        self.apply = apply            # apply(engine, values) -> message; raises ModError
        self.slow = slow              # needs a memory scan (text renames)
        self.needs_game = needs_game  # False: doesn't touch game memory, applies any time
        self.revert = revert          # revert(engine) for undo beyond memory patches

    def defaults(self):
        return {p["key"]: p.get("default") for p in self.params}


def _explosion_param(default=111):
    return [
        {"key": "explosion", "label": "Explosion", "type": "select", "default": default,
         "options": EXPLOSIONS + [{"value": "custom", "label": "Custom ID..."}]},
        {"key": "custom", "label": "Custom explosion ID", "type": "int", "default": 111, "showIf": ["explosion", "custom"]},
    ]


def _explosion_id(values):
    v = values.get("explosion")
    if v == "custom":
        v = values.get("custom")
    try:
        v = int(v)
    except (TypeError, ValueError):
        raise ModError("explosion ID must be a number")
    if not 0 < v < 4096:
        raise ModError("explosion ID out of range")
    return v


def apply_airstrike_bombs(engine, values):
    row = engine.attack_row(ATTACK_EAGLE_AIRSTRIKE, BOMB_MODEL_HASH)
    exp = _explosion_id(values)
    engine.patch("airstrike_bombs", row + 0xBC, p_u32(BODY_500KG))
    engine.patch("airstrike_bombs", row + 0xC4, p_f32(TRIGGER))
    engine.patch("airstrike_bombs", row + 0xC8, p_u32(exp))
    return f"Eagle Airstrike bombs now explode as {exp}"


def apply_500kg_explosion(engine, values):
    row = engine.attack_row(ATTACK_EAGLE_500KG, BOMB_MODEL_HASH)
    exp = _explosion_id(values)
    engine.patch("kg500_explosion", row + 0xC8, p_u32(exp))
    return f"500kg now explodes as {exp}"


def apply_gp31_explosion(engine, values):
    row = engine.attack_row(ATTACK_GP31, GRENADE_MODEL_HASH)
    exp = _explosion_id(values)
    engine.patch("gp31_explosion", row + 0xC4, p_f32(TRIGGER))
    engine.patch("gp31_explosion", row + 0xC8, p_u32(exp))
    return f"GP-31 grenades now explode as {exp}"


def apply_eagle_delay(engine, values):
    try:
        mult = float(values.get("multiplier"))
    except (TypeError, ValueError):
        raise ModError("multiplier must be a number")
    if not 0.05 <= mult <= 20:
        raise ModError("multiplier must be between 0.05 and 20")
    for ident in (ATTACK_EAGLE_AIRSTRIKE, ATTACK_EAGLE_500KG):
        addr = engine.attack_row(ident) + 0x44
        orig = f32(engine.original(addr, 4), 0)
        engine.patch("eagle_delay", addr, p_f32(orig * mult))
    return f"Eagle arrival delay x{mult:g}"


def apply_coyote_explosion(engine, values):
    row = engine.attack_row(ATTACK_COYOTE, 0)
    exp = _explosion_id(values)
    engine.patch("coyote_explosion", row + 0xC4, p_f32(TRIGGER))
    engine.patch("coyote_explosion", row + 0xC8, p_u32(exp))
    return f"Coyote rounds now explode as {exp}"


def apply_explosion_overrides(engine, values):
    lines = [l.strip() for l in str(values.get("rules", "")).splitlines() if l.strip() and not l.strip().startswith("#")]
    done = []
    for line in lines:
        parts = line.replace("=", " ").split()
        try:
            a, b = int(parts[0]), int(parts[1])
            trigger = float(parts[2]) if len(parts) > 2 else TRIGGER
        except (ValueError, IndexError):
            raise ModError(f"bad rule '{line}' (use: attackId = explosionId [trigger])")
        row = engine.attack_row(a)
        engine.patch("explosion_overrides", row + 0xC4, p_f32(trigger))
        engine.patch("explosion_overrides", row + 0xC8, p_u32(b))
        done.append(f"{a}->{b}")
    return "overrides: " + (", ".join(done) or "none")


def apply_text(mod_id, pairs_fn):
    def run(engine, values):
        pairs = pairs_fn(values)
        n = engine.rename_strings(mod_id, pairs)
        return f"{n} strings renamed"
    return run


def _check_pair(frm, to):
    if not frm:
        raise ModError("empty source text")
    if len(to) > len(frm):
        raise ModError(f"'{to}' is longer than '{frm}' ({len(to)} > {len(frm)} chars); text can't grow in memory")
    return frm, to


DESC_ORIGINAL = "This planet is considered enemy territory. The Helldivers must reclaim it from its oppressive occupants."

# The VR module (hd2vr_vr.dll, in-game) re-reads this file whenever it changes.
VR_CONFIG_PATH = r"C:\HD2VR\vrmod\vr_config.txt"
VR_OFF = {"stereo": 0, "head_mouse": 0, "head_locked": 0, "distance": 2.5, "width": 3.2}


def _write_vr_config(cfg):
    with open(VR_CONFIG_PATH, "w", encoding="ascii") as f:
        for k, v in cfg.items():
            f.write(f"{k}={int(v) if isinstance(v, bool) else v}\n")


def apply_vr_settings(engine, values):
    def num(key, lo, hi):
        try:
            v = float(values.get(key))
        except (TypeError, ValueError):
            raise ModError(f"{key} must be a number")
        if not lo <= v <= hi:
            raise ModError(f"{key} must be between {lo} and {hi}")
        return v
    cfg = {
        "stereo": values.get("mode", "stereo") == "stereo",
        "world_scale": num("world_scale", 0.1, 10),
        "fov_override": bool(values.get("fov_override")),
        "body_follow": bool(values.get("body_follow")),
        "head_mouse": bool(values.get("head_mouse")),
        "sens_x": num("sens_x", 0.1, 500), "sens_y": num("sens_y", 0.1, 500),
        "invert_y": bool(values.get("invert_y")),
        "head_locked": bool(values.get("head_locked")),
        "distance": num("distance", 0.5, 20), "width": num("width", 0.5, 30),
    }
    _write_vr_config(cfg)
    if cfg["stereo"]:
        return f"real VR: stereo head look, world scale {cfg['world_scale']:g}, " \
               f"{'headset' if cfg['fov_override'] else 'game'} field of view"
    return f"virtual screen: head tracking {'on' if cfg['head_mouse'] else 'off'}, sens {cfg['sens_x']:g}/{cfg['sens_y']:g}, " \
           f"{'head-locked' if cfg['head_locked'] else 'room'} screen {cfg['width']:g} m at {cfg['distance']:g} m"


def revert_vr_settings(engine):
    _write_vr_config(VR_OFF)


# First person runs as Lua in the game (vrmod\fp_camera.lua through the Lua probe, hd2vr_lua2.dll)
# and moves the rendered view with the native camera hook.
LUA_RUN_PATH = r"C:\HD2VR\vrmod\lua_run.lua"
FP_SCRIPT_PATH = r"C:\HD2VR\vrmod\fp_camera.lua"


def apply_first_person(engine, values):
    def num(key, lo, hi):
        try:
            v = float(values.get(key))
        except (TypeError, ValueError):
            raise ModError(f"{key} must be a number")
        if not lo <= v <= hi:
            raise ModError(f"{key} must be between {lo} and {hi}")
        return v
    fwd, up = num("forward", -0.5, 0.5), num("up", -0.5, 0.5)
    with open(FP_SCRIPT_PATH, encoding="utf-8") as f:
        script = f.read()
    with open(LUA_RUN_PATH, "w", encoding="utf-8") as f:
        f.write(f"hd2vr_fp = {{ forward = {fwd}, up = {up} }}\n" + script)
    return f"first person on (eyes {fwd:g} m forward, {up:g} m up from the head); needs the Lua probe injected"


def revert_first_person(engine):
    with open(LUA_RUN_PATH, "w", encoding="utf-8") as f:
        f.write("if hd2vr_fp_stop then hd2vr_fp_stop() end\n")

MODS = [
    Mod("vr_settings", "VR mode & head tracking", "VR",
        "Real VR renders the game in stereo from your head: look around freely while the mouse still aims "
        "and turns your Helldiver (needs the camera hook, hd2vr_cam*.dll). End recenters. "
        "Virtual screen shows the game on a big screen; its head tracking moves the mouse (Home toggles). "
        "Off = virtual screen fixed in the room, no head tracking. Applies live, no restart.",
        [{"key": "mode", "label": "Mode", "type": "select", "default": "stereo",
          "options": [{"value": "stereo", "label": "Real VR (stereo head look)"},
                      {"value": "screen", "label": "Virtual screen"}]},
         {"key": "world_scale", "label": "Real VR: world scale (1 = life size)", "type": "float", "default": 1.0},
         {"key": "fov_override", "label": "Real VR: use the headset's field of view", "type": "bool", "default": True},
         {"key": "body_follow", "label": "Real VR: turning your head turns your Helldiver", "type": "bool", "default": True},
         {"key": "head_mouse", "label": "Screen: head tracking (head -> mouse)", "type": "bool", "default": True},
         {"key": "sens_x", "label": "Horizontal sensitivity (mouse counts per degree)", "type": "float", "default": 12.0},
         {"key": "sens_y", "label": "Vertical sensitivity (mouse counts per degree)", "type": "float", "default": 12.0},
         {"key": "invert_y", "label": "Invert vertical", "type": "bool", "default": False},
         {"key": "head_locked", "label": "Screen: follows your head", "type": "bool", "default": True},
         {"key": "distance", "label": "Screen: distance (m)", "type": "float", "default": 2.5},
         {"key": "width", "label": "Screen: width (m)", "type": "float", "default": 3.2}],
        apply_vr_settings, needs_game=False, revert=revert_vr_settings),
    Mod("first_person", "First-person camera", "VR",
        "Puts the view at your Helldiver's eyes instead of over the shoulder; aiming and movement are "
        "unchanged. Works with or without the headset (VR head look stacks on top). Needs the Lua probe "
        "(hd2vr_lua2.dll) injected. Off = normal third-person camera.",
        [{"key": "forward", "label": "Eyes forward of the head bone (m)", "type": "float", "default": 0.12},
         {"key": "up", "label": "Eyes above the head bone (m)", "type": "float", "default": 0.06}],
        apply_first_person, revert=revert_first_person),
    Mod("airstrike_bombs", "Hellbomb Strafing Run", "Stratagems",
        "Eagle Airstrike drops a run of big bombs. Each bomb gets the explosion you pick (Hellbomb by default).",
        _explosion_param(111), apply_airstrike_bombs),
    Mod("kg500_explosion", "500kg explosion", "Stratagems",
        "Replace the Eagle 500kg's blast with another explosion.",
        _explosion_param(111), apply_500kg_explosion),
    Mod("gp31_explosion", "GP-31 Grenade Pistol explosion", "Weapons",
        "GP-31 grenades explode with the explosion you pick (Hellbomb by default). The blast and damage "
        "change; the Hellbomb's own sound doesn't come with it yet.",
        _explosion_param(111), apply_gp31_explosion),
    Mod("coyote_explosion", "AR-2 Coyote explosion", "Weapons",
        "Every Coyote round explodes with the explosion you pick (500kg by default). "
        "No explosion sound comes with it yet.",
        _explosion_param(277), apply_coyote_explosion),
    Mod("eagle_delay", "Eagle arrival delay", "Stratagems",
        "Scales how long Eagle Airstrike and 500kg take to arrive. Change it only when no Eagle is inbound "
        "(a strike already in flight can get lost).",
        [{"key": "multiplier", "label": "Delay multiplier", "type": "float", "default": 1.0}], apply_eagle_delay),
    Mod("name_democratic_nuke", "Rename Eagle Airstrike", "Text & names",
        "Renames the Eagle Airstrike everywhere it appears (HUD, loadout, messages). Plural 'Eagle Airstrikes' "
        "(ship module) is left alone.",
        [{"key": "name", "label": "New name (max 15 chars)", "type": "text", "default": "Democratic Nuke", "max": 15}],
        apply_text("name_democratic_nuke", lambda v: [_check_pair("Eagle Airstrike", str(v.get("name", "")))]), slow=True),
    Mod("cheb_sector", "NOVA Sector", "Text & names",
        "Renames the Rictus sector and its planets on the galaxy map.",
        [{"key": "sector", "label": "Rictus ->", "type": "text", "default": "NOVA", "max": 6},
         {"key": "genesis", "label": "Genesis Prime ->", "type": "text", "default": "NOVA PRIME", "max": 13},
         {"key": "grafmere", "label": "Grafmere ->", "type": "text", "default": "NOVA 426", "max": 8},
         {"key": "valmox", "label": "Valmox ->", "type": "text", "default": "NOVA 7", "max": 6}],
        apply_text("cheb_sector", lambda v: [_check_pair("Rictus", str(v.get("sector", ""))),
                                             _check_pair("Genesis Prime", str(v.get("genesis", ""))),
                                             _check_pair("Grafmere", str(v.get("grafmere", ""))),
                                             _check_pair("Valmox", str(v.get("valmox", "")))]), slow=True),
    Mod("planet_description", "Enemy planet description", "Text & names",
        "Replaces the description shown on enemy-held planets (one shared string, so every such planet).",
        [{"key": "text", "label": f"Description (max {len(DESC_ORIGINAL)} chars)", "type": "textarea",
          "default": "This planet has few Helldivers on it. A perfect place to work on mods!", "max": len(DESC_ORIGINAL)}],
        apply_text("planet_description", lambda v: [_check_pair(DESC_ORIGINAL, str(v.get("text", "")))]), slow=True),
    Mod("custom_renames", "Custom renames", "Text & names",
        "One rename per line: Original text => New text. The new text can't be longer than the original.",
        [{"key": "rules", "label": "Renames", "type": "textarea", "default": "Malevelon Creek => Nova Creek"}],
        apply_text("custom_renames", lambda v: [_check_pair(*[s.strip() for s in l.split("=>", 1)])
                                                  for l in str(v.get("rules", "")).splitlines() if "=>" in l]), slow=True),
    Mod("explosion_overrides", "Explosion overrides (advanced)", "Advanced",
        "Set any attack-table row's explosion. One rule per line: attackId = explosionId [trigger]. "
        "The trigger (+0xC4) defaults to 0.8; at 0 the explosion never fires. "
        "Example: 263 = 277 makes the GP-31 fire 500kg blasts.",
        [{"key": "rules", "label": "Rules", "type": "textarea", "default": "# 170 = 111"}], apply_explosion_overrides),
]
MODS_BY_ID = {m.id: m for m in MODS}

# --------------------------------------------------------------------------------------------
# Engine
# --------------------------------------------------------------------------------------------
SETTINGS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mod_settings.json")
# True original bytes for the current game process, so a restarted server doesn't mistake
# already-modded memory for the game's own values.
ORIGINALS_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mod_originals.json")


class Engine:
    def __init__(self):
        self.lock = threading.RLock()
        self.game = None
        self.tables_ready = False
        self.attack_rows = {}            # attack id -> row address
        self.originals = {}              # (addr, len) -> original bytes
        self.touched = {}                # mod id -> set of (addr, len)
        self.status = {}                 # mod id -> {"state": ..., "message": ...}
        self.jobs = queue.Queue()
        self.job = {"name": "", "progress": 0.0}
        self.current = None              # (kind, arg) of the running job
        self.cancel = threading.Event()  # set by stop(); scans check it between chunks
        self.discovery_stopped = False   # user stopped discovery: no automatic retries until Rescan
        self.log_lines = []
        self.settings = self._load_settings()

    # ---- settings -------------------------------------------------------------------------
    def _load_settings(self):
        try:
            with open(SETTINGS_PATH, "r", encoding="utf-8") as f:
                saved = json.load(f)
        except Exception:
            saved = {}
        out = {}
        for m in MODS:
            s = saved.get(m.id, {})
            values = m.defaults()
            values.update({k: v for k, v in s.get("values", {}).items() if k in values})
            out[m.id] = {"enabled": bool(s.get("enabled", False)), "values": values}
        return out

    def _save_settings(self):
        with open(SETTINGS_PATH, "w", encoding="utf-8") as f:
            json.dump(self.settings, f, indent=2)

    # ---- logging ----------------------------------------------------------------------------
    def log(self, msg):
        line = time.strftime("%H:%M:%S ") + msg
        with self.lock:
            self.log_lines.append(line)
            self.log_lines = self.log_lines[-200:]

    # ---- memory edits with automatic revert -----------------------------------------------
    def original(self, addr, n):
        key = (addr, n)
        if key not in self.originals:
            data = self.game.read(addr, n)
            if data is None:
                raise ModError(f"can't read {addr:#x}")
            self.originals[key] = data
            self._save_originals()
        return self.originals[key]

    def _save_originals(self):
        if not self.game:
            return
        with open(ORIGINALS_PATH, "w", encoding="utf-8") as f:
            json.dump({"pid": self.game.pid,
                       "items": {f"{a}:{n}": d.hex() for (a, n), d in self.originals.items()}}, f)

    def _load_originals(self, pid):
        """Originals saved by an earlier server run for this same game process."""
        try:
            with open(ORIGINALS_PATH, "r", encoding="utf-8") as f:
                saved = json.load(f)
        except Exception:
            return {}
        if saved.get("pid") != pid:
            return {}
        out = {}
        for key, hexdata in saved.get("items", {}).items():
            a, n = key.split(":")
            out[(int(a), int(n))] = bytes.fromhex(hexdata)
        return out

    def patch(self, mod_id, addr, data):
        self.original(addr, len(data))
        self.touched.setdefault(mod_id, set()).add((addr, len(data)))
        if not self.game.write(addr, data):
            raise ModError(f"write failed at {addr:#x}")

    def restore(self, mod_id):
        count = 0
        for key in self.touched.pop(mod_id, set()):
            if self.game and self.game.write(key[0], self.originals[key]):
                count += 1
        return count

    def attack_row(self, ident, model=None):
        """Address of the attack row with this id (ids can repeat; pass the model hash to pick one)."""
        if not self.tables_ready:
            raise ModError("tables not found yet")
        rows = self.attack_rows.get(ident, [])
        if model is not None:
            rows = [r for r in rows if r[1] == model]
        if not rows:
            raise ModError(f"attack id {ident} not in the table")
        return rows[0][0]

    # ---- discovery ------------------------------------------------------------------------
    def _find_table_fast(self):
        """The attack table starts exactly at the start of its own 128 KB allocation, with the Eagle
        Airstrike as row 2. So read 8 bytes per allocation start instead of scanning ~11 GB."""
        bomb = struct.pack("<Q", BOMB_MODEL_HASH)
        probe = 2 * ATTACK_ROW + 0xAC
        for base, size, alloc in self.game.writable_regions():
            if base == alloc and size > probe + 8 and self.game.read(base + probe, 8) == bomb:
                row = self.game.read(base + 2 * ATTACK_ROW, ATTACK_ROW)
                if row and u32(row, 0x2C) == ATTACK_EAGLE_AIRSTRIKE:
                    return base
        return None

    def _find_table_slow(self):
        """Fallback: full scan for the bomb hash, then the longest run of rows 0x110 apart."""
        bomb = struct.pack("<Q", BOMB_MODEL_HASH)
        hits = self.game.scan([bomb], progress=lambda p: self.job.update(progress=p), should_stop=self.cancel.is_set)[bomb]
        hitset = set(hits)
        best, best_run = None, 0
        for h in hits:
            run = 1
            while h + run * ATTACK_ROW in hitset:
                run += 1
            if run > best_run:
                best, best_run = h, run
        if best is None or best_run < 3:
            raise ModError(f"attack table not found ({len(hits)} bomb-hash hits)")
        # walk back from the first matching row to the table's first row
        start = best - 0xAC
        while True:
            b = self.game.read(start - ATTACK_ROW, ATTACK_ROW)
            if not (b and u32(b, 0x04) <= 16 and u32(b, 0x2C) < 4096 and 0 <= f32(b, 0x44) < 1e6):
                return start
            start -= ATTACK_ROW

    def discover(self):
        self.job["name"] = "Finding stratagem tables"
        t0 = time.time()
        start = self._find_table_fast()
        how = "fast"
        if start is None:
            self.log("fast table lookup missed; falling back to a full memory scan")
            start, how = self._find_table_slow(), "full scan"
        end = self.game.allocation_end(start)
        rows, count = {}, 0   # id -> [(addr, model hash)]
        addr, misses = start, 0
        while addr + ATTACK_ROW <= end and misses < 3:
            b = self.game.read(addr, ATTACK_ROW)
            if b is not None and u32(b, 0x04) <= 16 and u32(b, 0x2C) < 4096 and 0 <= f32(b, 0x44) < 1e6:
                misses = 0
                rows.setdefault(u32(b, 0x2C), []).append((addr, struct.unpack_from("<Q", b, 0xAC)[0]))
                count += 1
            else:
                misses += 1
            addr += ATTACK_ROW
        self.log(f"table lookup ({how}) took {time.time() - t0:.1f}s")
        self.attack_rows, self.tables_ready = rows, True
        try:
            airstrike = self.attack_row(ATTACK_EAGLE_AIRSTRIKE, BOMB_MODEL_HASH)
        except ModError:
            self.tables_ready = False
            raise ModError("found a table, but no Eagle Airstrike row in it")
        self.log(f"attack table: {count} rows ({len(rows)} ids), airstrike row {airstrike:#x}")

    # ---- text renames -----------------------------------------------------------------------
    def rename_strings(self, mod_id, pairs):
        """Rename null-terminated ASCII strings in writable memory. Shorter replacements pull the
        rest of the string up (\"RICTUS SECTOR\" -> \"NOVA SECTOR\") and zero the freed tail."""
        variants = {}   # needle bytes -> replacement bytes
        for frm, to in pairs:
            options = [(frm, to)]
            if "." not in frm:   # names: also match UPPER and Title Case copies
                options += [(frm.upper(), to.upper()), (string.capwords(frm), string.capwords(to))]
            for a, b in options:
                variants.setdefault(a.encode("ascii"), b.encode("ascii"))
        self.job["name"] = f"Renaming text ({len(pairs)} names)"
        hits = self.game.scan(list(variants), progress=lambda p: self.job.update(progress=p), should_stop=self.cancel.is_set)
        done = 0
        for needle, addrs in hits.items():
            repl = variants[needle]
            for addr in addrs:
                before = self.game.read(addr - 1, 1)
                tail = self.game.read(addr, 1024) or self.game.read(addr, 256)
                if not before or not tail or chr(before[0]).isalpha():
                    continue                                 # part of a longer word
                after = tail[len(needle):len(needle) + 1]
                if after and chr(after[0]).isalpha():
                    continue                                 # plural or longer word
                end = tail.find(b"\0")
                if end < 0:
                    if len(repl) != len(needle):
                        continue
                    new, span = repl, len(needle)
                else:
                    new = repl + tail[len(needle):end]
                    span = end
                    new = new + b"\0" * (span - len(new))
                try:
                    self.patch(mod_id, addr, new[:span])
                    done += 1
                except ModError:
                    pass
        return done

    # ---- jobs ---------------------------------------------------------------------------------
    def _set_status(self, mod_id, state, message=""):
        with self.lock:
            self.status[mod_id] = {"state": state, "message": message}

    def _apply_mod(self, mod_id):
        mod = MODS_BY_ID[mod_id]
        self.restore(mod_id)
        if not self.settings[mod_id]["enabled"]:
            if mod.revert:
                mod.revert(self)
            self._set_status(mod_id, "off")
            return
        self._set_status(mod_id, "working", "applying...")
        try:
            msg = mod.apply(self, self.settings[mod_id]["values"])
            self._set_status(mod_id, "on", msg)
            self.log(f"{mod.name}: {msg}")
        except Cancelled:
            self._disable(mod_id, "stopped")
        except ModError as e:
            self.restore(mod_id)
            self._set_status(mod_id, "error", str(e))
            self.log(f"{mod.name}: ERROR {e}")

    def _disable(self, mod_id, message):
        """Undo whatever the mod wrote and switch it off (used when the user stops it)."""
        self.restore(mod_id)
        if MODS_BY_ID[mod_id].revert:
            MODS_BY_ID[mod_id].revert(self)
        with self.lock:
            self.settings[mod_id]["enabled"] = False
            self._save_settings()
        self._set_status(mod_id, "off", message)
        self.log(f"{MODS_BY_ID[mod_id].name}: {message}, switched off")

    def stop(self, mod_id=None):
        """Stop the running job (all jobs, or just this mod's) and drop matching queued jobs."""
        kept, dropped = [], []
        while True:
            try:
                job = self.jobs.get_nowait()
            except queue.Empty:
                break
            (dropped if mod_id is None or job == ("apply", mod_id) else kept).append(job)
        for job in kept:
            self.jobs.put(job)
        for kind, arg in dropped:
            if kind == "apply":
                self._disable(arg, "stopped before it ran")
            elif kind == "discover":
                self.discovery_stopped = True
                self.log("table discovery stopped")
        cur = self.current
        if cur and (mod_id is None or cur == ("apply", mod_id)):
            self.cancel.set()

    def _worker(self):
        while True:
            kind, arg = self.jobs.get()
            self.cancel.clear()
            self.current = (kind, arg)
            try:
                if not self.game and (kind == "discover" or MODS_BY_ID[arg].needs_game):
                    continue
                if kind == "discover":
                    self.discover()
                    for m in MODS:
                        if self.settings[m.id]["enabled"]:
                            self.jobs.put(("apply", m.id))
                elif kind == "apply":
                    if self.tables_ready or MODS_BY_ID[arg].slow or not MODS_BY_ID[arg].needs_game:
                        self._apply_mod(arg)
                    else:   # discovery applies every enabled mod when it finishes
                        self._set_status(arg, "working", "waiting for tables")
            except Cancelled:
                if kind == "discover":
                    self.discovery_stopped = True
                    self.log("table discovery stopped (press Rescan to try again)")
            except Exception as e:  # keep the worker alive whatever happens
                self.log(f"job {kind} {arg or ''} failed: {e}")
                if kind == "apply":
                    self._set_status(arg, "error", str(e))
                elif kind == "discover":
                    self._retry_discover()
            finally:
                self.job = {"name": "", "progress": 0.0}
                self.current = None

    def _retry_discover(self, delay=20):
        """A freshly launched game hasn't loaded its data yet; try again until the tables appear."""
        pid = self.game.pid if self.game else None

        def again():
            if self.game and self.game.pid == pid and not self.tables_ready and not self.discovery_stopped:
                self.jobs.put(("discover", None))
        self.log(f"retrying table discovery in {delay}s")
        threading.Timer(delay, again).start()

    def _watchdog(self):
        while True:
            pid = find_game_pid()
            if pid != (self.game.pid if self.game else None):
                with self.lock:
                    if self.game:
                        self.game.close()
                    self.game, self.tables_ready, self.attack_rows = None, False, {}
                    self.discovery_stopped = False
                    self.originals, self.touched, self.status = {}, {}, {}
                    if pid:
                        try:
                            self.game = Game(pid)
                            self.originals = self._load_originals(pid)
                            self.log(f"attached to helldivers2.exe pid {pid}"
                                     + (f" ({len(self.originals)} saved originals)" if self.originals else ""))
                            self.jobs.put(("discover", None))
                        except OSError as e:
                            self.log(str(e))
                    else:
                        self.log("game not running")
            time.sleep(2)

    def start(self):
        threading.Thread(target=self._worker, daemon=True).start()
        threading.Thread(target=self._watchdog, daemon=True).start()

    # ---- API ------------------------------------------------------------------------------------
    def update(self, mod_id, enabled=None, values=None):
        if mod_id not in MODS_BY_ID:
            raise KeyError(mod_id)
        mod = MODS_BY_ID[mod_id]
        with self.lock:
            s = self.settings[mod_id]
            if enabled is not None:
                s["enabled"] = bool(enabled)
            if values:
                for p in mod.params:
                    if p["key"] in values:
                        s["values"][p["key"]] = values[p["key"]]
            self._save_settings()
        if self.game or not mod.needs_game:
            self._set_status(mod_id, "working", "queued")
            self.jobs.put(("apply", mod_id))

    def reapply_all(self):
        for m in MODS:
            if self.settings[m.id]["enabled"]:
                self.jobs.put(("apply", m.id))

    def rescan(self):
        if self.game:
            self.tables_ready = False
            self.discovery_stopped = False
            self.jobs.put(("discover", None))

    def snapshot(self):
        with self.lock:
            return {
                "engine": {
                    "gameUp": self.game is not None,
                    "pid": self.game.pid if self.game else None,
                    "tablesReady": self.tables_ready,
                    "attackRows": sum(len(v) for v in self.attack_rows.values()),
                    "job": dict(self.job),
                    "current": list(self.current) if self.current else None,
                    "queued": self.jobs.qsize(),
                    "log": list(reversed(self.log_lines[-60:])),
                },
                "mods": [{
                    "id": m.id, "name": m.name, "group": m.group, "desc": m.desc, "slow": m.slow,
                    "params": m.params,
                    "enabled": self.settings[m.id]["enabled"],
                    "values": self.settings[m.id]["values"],
                    "status": self.status.get(m.id, {"state": "pending" if self.settings[m.id]["enabled"] else "off", "message": ""}),
                } for m in MODS],
            }
