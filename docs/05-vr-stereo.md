# 05 — Real VR: OpenXR, the render-camera hook, stereo, first-person

This is the heart of the project: turning a flat, third-person, non-VR game into a **stereo,
head-tracked, first-person VR** experience on a Meta Quest 2 — all on the offline build, with
aiming still on the mouse.

Source: [`../vrmod/mod/vr_session.cpp`](../vrmod/mod/vr_session.cpp) (the OpenXR module),
[`../vrmod/mod/vr_host.cpp`](../vrmod/mod/vr_host.cpp) (hot-reload host),
[`../vrmod/mod/cam_hook.cpp`](../vrmod/mod/cam_hook.cpp) (the render-camera hook).

## Milestone 1 — the game on a screen in the headset (the easy win)

Before any real stereo, the first milestone was just **seeing the game in the headset**:

- Create an **OpenXR** session (`XR_KHR_D3D12_enable`) **on the game's own D3D12 device and
  queue** (captured by the D3D12 hook in chapter 04). Static OpenXR loader, so the mod DLL needs no
  extra DLL alongside it.
- Each frame, copy the game's back buffer into an OpenXR swapchain and submit it as a **quad
  layer** — a big virtual screen floating in front of you, either fixed in the room or locked to
  your head.
- The copy is a **fullscreen-triangle blit** (not `CopyResource`), so it works regardless of the
  game's and the runtime's pixel formats. The game's back buffer holds **sRGB-encoded** values; the
  blit shader **decodes to linear** because OpenXR runtimes treat colour as linear (an sRGB
  swapchain re-encodes on write).

Runtime: Meta Quest Link (the Oculus OpenXR runtime). The OpenXR session sits in IDLE until the
headset is actually worn — a surprising amount of debugging time was "the session is stuck" when
the real cause was the headset being off the face.

### Hot reload (iterating without relaunching the game)

Relaunching the game meant the whole GameGuard dance again, so the VR module is **hot-reloadable**.
[`vr_host.cpp`](../vrmod/mod/vr_host.cpp) is a tiny host compiled into the mod DLL; it watches the
built `hd2vr_vr.dll`, copies it to a temp path, loads that copy, and reloads it whenever the build
output changes. Edit → build → the running game picks up the new VR module in ~1 s, no relaunch.

## Head tracking v1 — head → mouse (cheap, works everywhere)

The first head tracking didn't touch the renderer at all: take the **VIEW-space head yaw/pitch
deltas** from OpenXR and feed them as **relative mouse movement** (`SendInput`), only while the game
window is foreground. Turning your head turns the in-game camera because the game thinks the mouse
moved. Simple, robust, but it's "a screen you look around on," not real VR — the world isn't
stereo and doesn't have depth.

## Real VR — the native render-camera hook

For actual stereo you have to move the **game's rendered view** per eye. The breakthrough was
finding the function that builds the render camera.

### Finding it

- **Lua can't do it.** Via the game's LuaJIT, you can reach the Stingray camera objects, but the
  game's `game.dll` camera system **rewrites the camera unit's local transform late in update**, and
  forcing a scene-graph update inside `render()` just blacks out the frame. Lua writes never reach
  the renderer.
- **Hardware watchpoints did it.** An injected tool (`hwbp` — debug-register data breakpoints, one
  of our utilities) was pointed at a copy of the camera matrix in memory to catch **which
  instruction writes it**. That led straight to the function.

### The function

```
helldivers2.exe + 0x4CFE40
  UpdateRenderCamera(RenderCamera* cam, float width, float height, float aspect)
```

- Called **per camera per frame**, on the game thread and again on the **render thread** (which
  also presents — important for stereo eye-tagging below).
- It builds `world = local(cam+0x238, a 4×4) × unit-node-world`, and writes the **world matrix at
  `+0xB8`**, the **view at `+0xF8`**, and the **projection at `+0x138`** — the matrices the culling
  and drawing threads read.
- Other fields on the struct: `+0x20` near, `+0x24` far, `+0x28` = 1 (perspective),
  `+0x2C` vertical FOV (radians), `+0x278` aspect, `+0x27C/+0x280` width/height.
- Prologue bytes (for a version-safe signature): `48 8B C4 55 53 48 8D A8 F8 FE FF FF`.

### The hook

[`cam_hook.cpp`](../vrmod/mod/cam_hook.cpp) hooks that function. For the frame's view camera it
**pre-multiplies a head pose into the camera-local matrix** (`+0x238`), optionally overrides the
FOV/aspect, lets the original run, then **restores** the bytes. Because it restores, the game's own
state never sees the change — **aim, audio, and gameplay logic are untouched**; only the rendered
view moves. Only cameras rendering at the **window's size** are touched, so shadow-cascade and probe
cameras keep their own matrices.

Verified on the desktop first (no headset needed): injecting a yaw, pitch, or a 1 m positional
offset rendered cleanly. A text command file drove tests like `yaw 30 pitch 10 x 0.03`, `off`,
`stats`, `unhook`.

Critically, the hook **waits for the GameGuard kill flag** before patching anything — patching game
code while GameGuard was alive produced the `0xC0000409` fail-fast (chapter 03).

## Stereo — alternate-eye rendering

A flat game renders **one** view per frame; VR needs **two** (one per eye). Rendering the whole
scene twice per frame in an engine you don't control is impractical, so we used **alternate-eye
rendering**:

- Each game frame renders **one eye**. Before the frame, the VR module hands the hook that eye's
  pose (relative to a recentred reference) and a projection for that eye.
- When the frame is **presented**, the module figures out **which eye** it was (by tagging each
  frame with the pose sequence number the render thread used — and because the render thread is the
  one that presents, the tag is **exact**, `eye_lag = 0`), copies that frame into that eye's
  swapchain, and submits **both eyes as an OpenXR projection layer**, each with the pose it was
  actually rendered with.
- The runtime's **reprojection** covers the eye that is a frame old, so motion stays smooth at the
  headset's refresh rate even though each eye updates at half the game's frame rate.

`End` recenters (defines "forward" in the room as the game camera's forward). Config lives in a
`vr_config.txt` the module re-reads live.

### The double-vision bug (and the fix) — the hardest VR debugging

In the headset the image came out **cross-eyed / doubled**, even though single-eye frames were
clean and the eye separation (~63 mm) and FOV measured correct. The debugging was systematic — a
series of diagnostic modes (turn one eye backwards + measure brightness; mono; a direction grid; raw
frame dumps for offline disparity measurement; a synthetic ray-traced wall drawn into both eyes):

- Eye **tagging** was verified exact (each presented frame was the eye we thought).
- Eye **separation and FOV** were correct.
- The synthetic wall was **also** doubled — so it wasn't the game's rendering, it was the
  **submission geometry**.

**Root cause:** Meta Quest Link **misplaces a projection-layer view whose FOV is not that eye's own
asymmetric FOV.** We had been rendering a **symmetric "cover" frustum** (the game's projection is
symmetric), which pushed each eye's image ~8° outward. **Fix:** keep letting the game render its
symmetric frustum, but **resample each eye into its native asymmetric FOV** in the blit before
submitting — declare to the runtime exactly the FOV it expects. After that, proper single-image 3D.

The diagnostic machinery is still in the source behind a `vr_config` `eye_test` switch (1 tilt one
eye, 4/9 mono, 6 direction grid, 7/8 raw dumps, 10/11 synthetic wall) — useful if anyone ports this
idea to another runtime.

## First-person camera (at the Helldiver's eyes)

Helldivers 2 is third-person (over-the-shoulder). To sit the view at the character's eyes:

- The Lua probe ([`lua/fp_camera.lua`](../vrmod/lua/fp_camera.lua)) finds the **player unit** (the
  closest full skeleton with a `head` node) and, every frame, reads the **head bone's world
  position** and hands the camera hook the desired **eye position in world space**
  (`hd2vr_cam_set_eye`).
- The hook turns that into an **offset in the frame of the camera the game actually renders**. The
  subtlety: the game moves its camera **again** after Lua's `render()` runs, so you can't compute
  the offset in Lua and expect it to hold — the hook measures the eye against the **real** render
  camera on the render thread, in the same frame. Tunables: how far forward/up from the head bone
  the eye sits.

The result: you look out of the Helldiver's head, the body is yours, and the mouse still aims
normally. VR head-look stacks on top of this.

## Head turns the body (so you run where you look)

With a first-person VR camera, a new problem appears: your **head** turns freely, but your
**body** (the game character) keeps facing the old way, so running "forward" sends you off at an
angle — disorienting. The fix steers the character to follow your head:

- The hook reports the game camera's **own yaw** each frame. A "**yaw lock**" renders the view at
  your head's yaw regardless of the game yaw, while the module **steers the game camera** (and thus
  the character) toward your head yaw using **in-process mouse input** (the game ignores mouse
  input injected from *other* processes, so this has to come from inside — one more reason the mod
  is injected).
- Your **real mouse** still turns you: a low-level mouse hook distinguishes real (non-injected)
  movement from the module's own injected movement, and moves the reference instead of fighting it.
- A **safety**: if the module keeps steering but the reported game yaw doesn't respond, it stops
  (rather than spinning you) — this caught a bug where placeholder "scratch" cameras reported a
  fixed 0° yaw and the loop never converged ("ran in circles").

Tunables (gain, deadzone, mouse-counts-per-degree) live in the config and on the dashboard.

> The open design question we left: how to handle **backpedalling**. In normal Helldivers 2, moving
> backward turns the body around (unless aiming). In first-person VR that fights the "body follows
> head" model. We left this for motion controllers to resolve.

## What was and wasn't finished

- ✅ Stereo 3D head-look in the Quest 2, mouse still aims.
- ✅ The Quest Link FOV double-vision fix.
- ✅ First-person at the head bone.
- ✅ Head-turns-body steering (with a convergence safety).
- ⏳ **Motion controllers (6DOF)** — the plan was: find the weapon/aim transform in the engine, map
  the right controller to aim and the left stick/buttons to locomotion/reload/grenade/stratagem
  (via `SendInput` or a virtual gamepad), and derive throw gestures from controller velocity. Not
  built. This is where the EAC transition ended the project.
- ⏳ TAA ghosting check with alternating eyes; per-eye resolution tuning.

## Honesty about difficulty

Phases "inject + overlay" were high-confidence (user-mode AC, standard D3D12 hooking). The VR
camera work was **genuine uncertainty** — a packed engine makes finding the camera/weapon matrices
hard, and per-eye stereo in a non-VR game is a large effort. The fallback for stereo, had the
native camera hook proved intractable, was **depth-buffer-based reprojection** (vorpX-style "fake
3D"). The native hook worked, so that fallback was never needed.
