# nProtect GameGuard — Technical Study Notes

**Subject:** nProtect GameGuard (INCA Internet Co., Ltd.) as deployed with *Helldivers 2*
**Deployment path:** `C:\Program Files (x86)\Steam\steamapps\common\Helldivers 2\bin\GameGuard\`
**Version stamp observed:** `Tue Apr 22 09:37:40 2025`
**Document type:** Educational architecture & mechanism analysis

---

## 0. Scope — what this document is and is not

This document explains **how the anti-cheat is put together and how its detection mechanisms
work conceptually**. Everything here comes from two sources:

1. **Observations of this deployment** — file inventory, PE headers, Authenticode signatures,
   container headers, and the Steam install script. These are marked **[observed]**.
2. **Publicly documented behavior** of nProtect GameGuard and of kernel-mode anti-cheat/EDR
   systems in general — conference talks, academic papers, Microsoft documentation, and the
   broader game-security literature. These are marked **[public]**.

This is **not** an operational bypass document. It intentionally contains **no**:
binary offsets, patch points, "disable check X here" instructions, memory signatures extracted
from the live binaries, or cheat-enabling tooling. Those are excluded on purpose (see §15).

---

## 1. Summary

nProtect GameGuard is a long-lived, commercial **anti-cheat / anti-tamper** suite. It predates
most modern kernel anti-cheats (it has shipped since the early 2000s) and its stable, recognizable
design is: a **launcher** that verifies and updates itself, a separate **monitor process**, a set
of **native modules** that scan memory and instrument the game, an optional **kernel component**
that locks down access to the game's address space, and a **backend** that ships updated
detection signatures back down.

The defining architectural traits visible in this deployment:

- **Separation into a standalone monitor process** that lives outside the game process.
- **Heavy obfuscation** (multi-megabyte modules for small jobs, renamed extensions, encrypted config).
- **A/B update slots** so detection logic can be swapped without a game patch.
- **Runtime integrity + memory scanning + handle-based access control** as the core detection triad.

---

## 2. Threat model — what it is defending against

Anti-cheat exists to defeat a specific adversary: a user with **Administrator rights on their own
machine**, full access to the game's memory, and the ability to attach debuggers, inject DLLs, patch
files, and script input. The attacker is *not* remote — they are local, privileged, and persistent.

| Attack class | Example | Anti-cheat countermeasure |
|---|---|---|
| Static file modification | Editing a weapon config or patching `game.dll` | Integrity/checksum verification |
| Runtime memory patching | Writing `0x90` over an instruction | In-memory integrity checks, page monitoring |
| External process instrumentation | Attaching a debugger or a trainer | Debugger detection, process enumeration, handle control |
| DLL injection / hooking | Injected code hooking DirectX `Present` | Module enumeration, executable-page scanning, hook detection |
| Automation | Macros, auto-aim scripts | Input heuristics, behavioral analysis |
| Analysis/RE of the anti-cheat itself | Reverse-engineering the scanner | Obfuscation, virtualization, anti-debug, anti-VM |

The design problem is fundamentally **asymmetric**: the defender's checks run on a machine the
attacker fully controls, so no client-side check can ever be *provably* unbypassable. This is why
the **server** retains final authority (§12) and why the client focuses on **raising cost and
catching casual/semi-skilled attackers**.

---

## 3. Deployment inventory **[observed]**

All `.des` files are ordinary PE images with a renamed extension. Every executable/DLL carries a
**valid** Authenticode signature from `CN="INCA Internet Co., Ltd."` (Seoul, KR).

| File | PE class | Subsystem | Role |
|---|---|---|---|
| `GameGuard.des` | PE32 **exe** (x86) | GUI | Launcher / stub |
| `GameMon64.des` | PE32+ **exe** (x64) | GUI | "GameMon" monitor process (64-bit) |
| `GameMon.des` | PE32 **exe** (x86) | GUI | Monitor process (32-bit variant) |
| `npggNT64.des` | PE32+ **DLL** (x64) | GUI | GameGuard native core module |
| `npggNT.des` | PE32 **DLL** (x86) | GUI | Native core module (32-bit) |
| `npsc64.des` | PE32+ **DLL** (x64) | GUI | Scanning / security-core module |
| `ggscan.des` | PE32 **DLL** (x86) | GUI | Signature scanner |
| `ggerror.des` | PE32 **exe** (x86) | GUI | Error reporting / dialog |
| `npgmup.des` | PE32 **DLL** (x86) | GUI | Updater (drives the A/B slots) |
| `SONYGame.ini` | binary/obfuscated | — | Game-specific configuration (not plaintext) |
| `nplsm.des`, `nplsb.des` | opaque markers | — | Short header + hex blob (IDs/nonce-like) |
| `0npg*.erl`, `1npg*.erl`, `npg*.erl` | data container | — | Scan/update lists (see §11) |
| `GameGuard.ver` | text | — | Version timestamp |
| `Splash.jpg` | image | — | Branding shown at launch |

**Key observations:**

- The two `GameGuard.des` copies (`bin\` and `bin\GameGuard\`) are **byte-identical**
  (SHA-256 `85CF5ED3…1790AF`).
- All `.erl` files begin with the **same 4-byte container magic `00 FC CC 00`** — a shared
  encrypted/obfuscated container format for the scan lists.
- `SONYGame.ini` is **not** text — it is encrypted/obfuscated, with embedded ASCII fragments
  (`SONYGame.ini`) inside an otherwise binary blob.
- Sizes are disproportionate to function (`ggerror.des` ≈ 3.8 MB for an error dialog;
  `GameMon64.des` ≈ 13.8 MB) — the classic footprint of **code virtualization / obfuscation**.

### Steam install integration **[observed]**

`tools\installscript.vdf` is plaintext and shows the lifecycle hooks:

```vdf
"InstallScript"
{
    "Run Process"
    {
        "npGameGuard_install"
        {
            "Process 1"  "%INSTALLDIR%\\tools\\GGSetup.exe"
        }
    }
    "Run Process On Uninstall"
    {
        "npGameGuard_uninstall"
        {
            "Process 1"  "%INSTALLDIR%\\tools\\gguninst.exe"
            "Command 1"  "/silent"
        }
    }
    "Firewall"
    {
        "HELLDIVERS™ 2"  "%INSTALLDIR%\\bin\\helldivers2.exe"
    }
}
```

Interpretation: **`GGSetup.exe` runs at install time** and is what registers the anti-cheat's
service/driver and copies the payload into place; `gguninst.exe /silent` reverses it on uninstall.
The firewall rule is for the game, not GameGuard.

---

## 4. Architecture **[public, consistent with observed layout]**

GameGuard is layered across privilege rings, from the game down to the kernel and out to the
network:

```
                         ┌───────────────────────────────────────────────┐
                         │        INCA / GameGuard backend servers        │
                         │   (signature updates, health reports, verdict) │
                         └───────────────▲───────────────┬───────────────┘
                                         │ heartbeats     │ signature/update push
                                         │ + reports      │
        ┌────────────────────────────────┴───────────────▼────────────────────────────────┐
        │                              MONITOR PROCESS (ring 3)                            │
        │   GameMon64.des / GameMon.des                                                    │
        │   • orchestrates checks        • loads native modules                            │
        │   • scheduled scan loop        • owns the A/B updater (npgmup.des)               │
        └───────────────┬─────────────────────────────────┬───────────────────────────────┘
                        │ loads                            │ injects / attaches
        ┌───────────────▼───────────────┐   ┌──────────────▼──────────────────────────────┐
        │   NATIVE MODULES (ring 3)     │   │        GAME PROCESS (ring 3)                │
        │   npggNT64.des (core)         │   │   helldivers2.exe + data\game\game.dll      │
        │   npsc64.des   (scan core)    │   │   • GameGuard code mapped in                │
        │   ggscan.des   (signatures)   │   │   • checks game's own memory & API usage    │
        └───────────────┬───────────────┘   └─────────────────────────────────────────────┘
                        │ ioctl / syscalls
        ┌───────────────▼─────────────────────────────────────────────────────────────────┐
        │                       KERNEL COMPONENT (ring 0)                                  │
        │   Deployed/managed at runtime as a service by GGSetup.exe                        │
        │   • object callbacks → strip VM_READ/VM_WRITE from handles to the game           │
        │   • hidden-process / inline-hook / SSDT hook detection                           │
        │   • protect its own files, registry keys and process                             │
        └─────────────────────────────────────────────────────────────────────────────────┘
```

Notes on the diagram:

- `npggNT64.des` / `npsc64.des` ship here as **user-mode DLLs** (subsystem = GUI, DLL flag set,
  **[observed]**). The **kernel driver is not a loose `.sys` in this folder** — it is deployed and
  registered at runtime by `GGSetup.exe`. This is consistent with GameGuard's long-standing
  practice of staging its driver rather than leaving it open on disk. **[public]**
- The monitor is deliberately **out-of-process**: compromising the game process alone doesn't
  disable the monitors, and vice-versa.

---

## 5. Startup sequence

```
 [1] Steam launches the game
      │
      ▼
 [2] GameGuard.des (launcher)            ── verify own files (integrity)
      │                                   ── optionally fetch update
      ▼
 [3] GameMon64.des (monitor process)     ── spawns as a separate process
      │                                   ── loads npggNT64.des / npsc64.des
      ▼
 [4] Kernel component                    ── service/driver installed & started
      │                                   ── begins protecting the game process
      ▼
 [5] helldivers2.exe                     ── GameGuard code mapped into game address space
      │
      ▼
 [6] Recurring loop (while playing)      ── integrity + memory scans
                                          ── process/module/window enumeration
                                          ── debugger & instrumentation checks
                                          ── periodic heartbeat to backend
      │
      ▼
 [7] On detection                        ── report to server / terminate game
```

The important structural point: **steps 2–4 happen before/around the game**, and step 6 runs
**continuously**, not once at startup.

---

## 6. Component reference

- **`GameGuard.des` — launcher.** The entry point Steam invokes. Establishes the trust anchor:
  checks that GameGuard's own files are intact, may run the updater, then launches the monitor and
  hands off to the game.
- **`GameMon64.des` / `GameMon.des` — monitor.** The brain. A standalone process that owns the
  scan schedule, loads the native modules, attaches to the game, and talks to the backend.
- **`npggNT64.des` / `npggNT.des` — native core.** The main GameGuard logic: hook detection,
  integrity routines, and the user-mode half of the kernel interface.
- **`npsc64.des` / `npsc.des` — scan core.** The scanning engine that walks memory and matches
  against signatures.
- **`ggscan.des` — signature scanner.** The matcher component used by the scan core.
- **`npgmup.des` — updater.** Implements the A/B slot swap (§11).
- **`ggerror.des` — error reporter.** Presents failures and can feed data back.
- **`SONYGame.ini` — game profile.** Encrypted config: which files to hash, tuning, game-specific
  targets. *Per-game*, hence the "SONY"/publisher naming.
- **`nplsm.des` / `nplsb.des` — markers.** Tiny opaque files (a 4-byte header + hex payload);
  consistent with license/session identifiers or nonce material.

---

## 7. Detection & enforcement techniques

The following is the standard, publicly-documented technique set for this class of anti-cheat.

### 7.1 Integrity verification (static + dynamic)
The launcher/monitor hash the game's files and loaded modules and compare against reference
values. On disk this catches modified executables/DLLs; **in memory** it catches runtime patching
(comparing the live image against the on-disk image, or periodic re-hashing of code pages).

### 7.2 Memory scanning
The scan core walks the target process's virtual address space, looking for:
- **Known cheat signatures** — byte patterns from public trainers/injectors.
- **Anomalies** — executable pages that aren't backed by the expected module, RWX regions,
  orphaned trampolines, or unexpected code in the heap.

### 7.3 Process / module / window enumeration
Enumerates running processes, loaded modules, and window classes/titles, matching against a
blacklist of known debugging and cheating tools by name, hash, and behavior. This is the cheapest
and noisiest layer — and the one most prone to false positives.

### 7.4 Debugger & instrumentation detection
Classic techniques, all textbook:
- `IsDebuggerPresent`, `CheckRemoteDebuggerPresent`, and the **PEB `BeingDebugged`** flag.
- **Hardware breakpoints** — inspecting `Dr0–Dr7` debug registers for set breakpoints.
- **Software breakpoints** — scanning code regions for `INT3` (`0xCC`) where the original
  instruction shouldn't be.
- **Timing checks** — measuring elapsed high-resolution time across a code region; a breakpoint or
  single-step introduces measurable delay.
- **Anti-attach** — preventing or detecting remote thread creation into the game.

### 7.5 Anti-analysis / anti-RE
Anti-VM and anti-sandbox probes (CPUID hypervisor bit, timing artifacts, device fingerprints),
encrypted strings, self-checksumming code, and **control-flow obfuscation / virtualization** so
that static disassembly is expensive.

### 7.6 Kernel-enforced access control
The kernel component restricts what other processes may do to the game: stripping memory-read and
memory-write rights from handles to the game process, and detecting hidden processes and hooks.
See §8.

### 7.7 Behavioral / server-side validation
Client-side heuristics (impossible input rates, unnatural accuracy) plus server-side statistical
analysis. This is the layer an attacker cannot observe or patch — which is exactly why it exists.

---

## 8. Kernel component deep dive **[public]**

The ring-0 piece is what distinguishes a "serious" anti-cheat. Its capabilities, in general terms:

1. **Handle access control via object callbacks.** Using `ObRegisterCallbacks`, a driver can
   intercept every attempt to obtain a handle to the game process and **remove dangerous access
   bits** (`PROCESS_VM_READ`, `PROCESS_VM_WRITE`, `PROCESS_VM_OPERATION`) from the granted handle.
   Result: `OpenProcess` may "succeed" for an attacker tool, but reads/writes through the handle
   fail. This single mechanism neutralizes a large fraction of external memory editors.
2. **Hidden-object detection.** Cross-referencing kernel lists to find processes, threads, or
   drivers that are deliberately unlinked (rootkit-style hiding).
3. **Hook detection.** Inspecting system service dispatch tables and inline function prologues in
   kernel modules for tampering.
4. **Self-protection.** Guarding its own image, registry keys, services, and threads from being
   stopped or tampered with.
5. **Fidelity of game protection.** Monitoring handle creation, thread injection, and page
   protection changes targeting the game.

Because a driver runs at the highest privilege level, it is also **the largest attack surface** —
a kernel anti-cheat driver is, from a security standpoint, effectively privileged code that a
determined local administrator can attack. This is a well-known tension in the industry.

---

## 9. Illustrative code

> These snippets illustrate the **general technique classes**. They are generic and are **not**
> extracted from, or targeted at, the GameGuard binaries. They come from public documentation
> (chiefly Microsoft's) and standard security references.

### 9.1 The integrity-check idea (conceptual, Python)

```python
# Illustrative only: the shape of a module integrity check.
# Real implementations hash specific sections, not whole files, and compare
# the on-disk image against the mapped (live) image to catch runtime patches.
import hashlib, pefile

def section_hashes(path):
    pe = pefile.PE(path, fast_load=True)
    out = {}
    for s in pe.sections:
        out[s.Name.rstrip(b"\x00").decode(errors="replace")] = \
            hashlib.sha256(s.get_data()).hexdigest()
    return out

def verify(path, reference):
    for name, digest in section_hashes(path).items():
        if reference.get(name) != digest:
            return f"tampered section: {name}"
    return "ok"
```

### 9.2 The debugger-detection idea (textbook Win32, C)

```c
// Textbook anti-debug probes. Framed here as *what detectors look for*,
// which is also what you study to understand them defensively.
#include <windows.h>

int looks_debugged(void) {
    if (IsDebuggerPresent()) return 1;                 // PEB.BeingDebugged

    BOOL remote = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    if (remote) return 1;

    CONTEXT ctx = { .ContextFlags = CONTEXT_DEBUG_REGISTERS };
    if (GetThreadContext(GetCurrentThread(), &ctx)) {  // hardware breakpoints
        if (ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3) return 1;
    }
    return 0;
}
```

### 9.3 The kernel handle-stripping idea (Microsoft-documented pattern, C)

```c
// Conceptual: register an object callback that strips VM access from handles
// opened to the protected process. This is the mechanism EDR and anti-cheat
// drivers share; it is fully documented by Microsoft.
OB_PREOP_CALLBACK_STATUS
on_pre_open(PVOID reg, POB_PRE_OPERATION_INFORMATION info) {
    if (info->KernelHandle) return OB_PREOP_SUCCESS;
    if (info->ObjectType != *PsProcessType) return OB_PREOP_SUCCESS;

    ACCESS_MASK strip = PROCESS_VM_READ | PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
    info->Parameters->CreateHandleInformation.DesiredAccess &= ~strip;
    return OB_PREOP_SUCCESS;
}
// ...registered once via ObRegisterCallbacks with OB_OPERATION_HANDLE_CREATE.
```

The takeaway from 9.3: blocking *external* memory access doesn't require finding and banning each
tool — it requires controlling the **handle-grant path**, which every tool must pass through.

---

## 10. Obfuscation & anti-analysis observations **[observed + public]**

- **Extension camouflage:** `.des` instead of `.exe`/`.dll`. Cosmetic, but it breaks naive tooling
  and simple allow/deny rules keyed on extension.
- **Size/function mismatch:** the binaries are far larger than their apparent role, indicating
  embedded obfuscation engines (string encryption, control-flow flattening, and often
  virtual-machine-based protection).
- **Encrypted config:** `SONYGame.ini` is a binary blob with embedded ASCII fragments — the single
  clearest signal that GameGuard encrypts its own data at rest.
- **Uniform container header:** all `.erl` lists share the `00 FC CC 00` magic — one proprietary
  container format for all its scan/update data.
- **Self-protection:** expect self-checksumming across modules, cross-module mutual verification
  (the launcher, monitor, and driver watching each other), and anti-dump logic.

---

## 11. The update & A/B slot mechanism **[observed + public]**

GameGuard ships **two parallel copies** of its data lists:

```
 0npgm.erl   625,939      npgm.erl   584,052   (base)
 1npgm.erl   857,964      npgl.erl    94,671
 0npgg.erl    53,622      npgg.erl    49,548
 1npgg.erl    52,562      npsc64.erl  30,053
 0npgl.erl   100,773      npgmup.erl  30,495
 1npgl.erl   110,730
 0npsc64.erl  25,779
 1npsc64.erl  47,651
 0npgmup.erl  32,610
 1npgmup.erl  32,074
```

The `0…` / `1…` prefixes are **update slots**. The updater (`npgmup.des`) writes a new signature
set into the inactive slot, validates it, then flips which slot is live. This is the classic
**A/B (dual-bank) update** pattern:

```
   active = slot 0            write new set            active = slot 1
   ┌──────────┐               ┌──────────┐             ┌──────────┐
   │ slot 0 ✔ │  ───────────▶ │ slot 0 ✔ │  ──flip──▶  │ slot 0   │
   │ slot 1   │               │ slot 1 ✚ │             │ slot 1 ✔ │
   └──────────┘               └──────────┘             └──────────┘
```

**Why it matters:** detection signatures can be refreshed **out of band**, without patching the
game, so the anti-cheat can react to new cheats within days rather than waiting on the developer's
release cycle. `GameGuard.ver` records the timestamp of the installed set.

---

## 12. Network & server trust **[public]**

GameGuard maintains a channel to its backend. Two functions:

1. **Telemetry** — periodic heartbeats and, on detection events, structured reports.
2. **Signature delivery** — pushing updated scan data that feeds the A/B slots.

Architecturally, the server is the **root of trust**: any purely client-side bypass is only
durable until the server stops trusting the client's reports, or until statistical/behavioral
detection on the server flags the account. This is the industry's consensus answer to "you cannot
win a client-side arms race."

---

## 13. Class-level limitations (read defensively)

These are general properties of *any* client-side anti-cheat — useful to understand both why it
works as well as it does and where its inherent limits are:

- **Asymmetric adversary.** The defender's code runs on hardware the attacker controls, at equal
  or lower privilege. Client-side guarantees are therefore best-effort by construction.
- **TOCTOU races.** Any "check, then use" sequence can in principle be raced by a sufficiently
  privileged attacker. Anti-cheat mitigates with randomization and redundancy, not proofs.
- **Kernel driver = attack surface.** The most powerful component is also privileged code; if it
  has a bug, that bug is a kernel vulnerability.
- **False positives.** Process-name and behavioral heuristics can flag legitimate software
  (overlays, accessibility tools, debuggers used by developers).
- **Performance cost.** Continuous scanning and handle callbacks have measurable overhead.
- **Detectability of the detector.** A loud, resident monitor is itself observable — which is why
  obfuscation and multi-process/multi-ring design matter.

---

## 14. What this teaches (blue-team transferable skills)

Anti-cheat is a specialization of **endpoint security**, and the overlap with EDR is large:

| GameGuard concept | Security-engineering counterpart |
|---|---|
| Handle access control (stripping VM rights) | EDR process protection / anti-ransomware handle guards |
| Integrity + in-memory re-hashing | File-integrity monitoring, code-integrity attestation |
| Kernel object callbacks | EDR kernel telemetry |
| A/B signature slots | Fast IOC/signature update pipelines |
| Server authority + telemetry | SIEM detection + centralized verdicts |
| Anti-analysis obfuscation | Malware tradecraft (studied defensively) |

If you want to build low-level security skills in this space, the transferable core is:
**Windows internals → kernel callbacks and object manager → PE/loader mechanics → telemetry
design.** Those are the same muscles used by EDR, rootkit detection, and hypervisor-based security.

---

## 15. Ethics & legal note

Reverse-engineering this class of software to *understand* it is a recognized security discipline
and is how defenses and vulnerabilities are found. But:

- **Bypassing it to cheat** is barred by the game's/Steam's/Sony's EULA and ToS.
- **Defeating technical protection measures** can engage anti-circumvention statutes (e.g., US
  DMCA §1201) depending on jurisdiction and intent.
- In an **online multiplayer** title, a bypass harms other players — it is not a victimless act.

Accordingly, this document stops at mechanisms and does not provide bypass instructions, offsets,
signatures, or tooling. For hands-on practice, use **crackmes, CTFs, and purpose-built reversing
labs**, not a live anti-cheat protecting other people's games.

---

## 16. Glossary

| Term | Meaning |
|---|---|
| **Ring 0 / ring 3** | Privilege levels: kernel mode vs. user mode. |
| **PE / PE32 / PE32+** | Windows Portable Executable format; 32-bit / 64-bit. |
| **Authenticode** | Microsoft's code-signing scheme; proves publisher & integrity. |
| **Object callback** | Kernel hook on object (process/thread) handle operations. |
| **SSDT** | System Service Dispatch Table — maps syscall numbers to kernel functions. |
| **Inline hook** | Overwriting a function's first bytes to redirect execution. |
| **Code virtualization** | Compiling logic to a custom VM bytecode to defeat static analysis. |
| **A/B update** | Dual-bank firmware/signature update that flips active slots. |
| **TOCTOU** | Time-of-check-to-time-of-use race. |

---

## 17. Further reading (public)

- Microsoft Learn: *Object Callbacks*, *ObRegisterCallbacks*, *PE format*.
- *Game Hacking: Developing Autonomous Bots for Online Games* (Canavan) — attacker's perspective,
  the standard reference.
- Academic anti-cheat literature (e.g., *Cheat-proofing*, *Anti-cheat survey* papers).
- Conference talks on kernel anti-cheat design (DEF CON / REcon / Black Hat archives).
- Rizin/radare2 and Ghidra docs for reversing **practice targets** (crackmes).

---

*End of document.*
