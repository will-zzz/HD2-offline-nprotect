-- First-person camera: every frame (in render()) hand the native hook the Helldiver's eye position
-- in world space (hd2vr_cam_set_eye). The hook measures it against the camera the game really
-- renders (the game moves its camera again after Lua's render()), so the view sits at the eyes.
-- The game camera keeps aiming; only the rendered position moves.
-- Tunables: hd2vr_fp.forward / hd2vr_fp.up (metres from the head bone). Stop: hd2vr_fp_stop()
local ffi = require("ffi")
local A, W, U, V = stingray.Application, stingray.World, stingray.Unit, stingray.Vector3
local M = stingray.Matrix4x4
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end

if hd2vr_fp_stop then hd2vr_fp_stop() end

pcall(ffi.cdef, "void hd2vr_cam_set_eye(float x, float y, float z, int enabled);")
-- The newest loaded hook copy wins (standalone test copies chain over the one in the mod DLL).
local hook
for _, name in ipairs({ "hd2vr_mod14", "hd2vr_mod13", "hd2vr_mod12", "hd2vr_mod11" }) do
  local ok, lib = pcall(ffi.load, name)
  if ok and pcall(function() return lib.hd2vr_cam_set_eye end) then hook = lib; hd2vr_log("fp: using " .. name) break end
end
if not hook then hd2vr_log("fp: no camera hook with hd2vr_cam_set_eye loaded") return end

hd2vr_fp = hd2vr_fp or { forward = 0.12, up = 0.06 }
local state = { frames = 0, errs = 0, player = nil, head = nil, world = A.main_world() }

local function find_camera()
  for _, u in ipairs(W.units(state.world)) do
    if handle(u) == 5 and U.num_cameras(u) > 0 then return u end
  end
end

-- The Helldiver: the closest unit to the camera with a "head" node and a full skeleton.
local function find_player(cpos)
  local best, bestd, bestnode
  for _, u in ipairs(W.units(state.world)) do
    if U.alive(u) and U.num_scene_graph_items(u) > 60 then
      local ok, has = pcall(U.has_node, u, "head")
      if ok and has then
        local n = U.node(u, "head")
        local d = V.length(U.world_position(u, n) - cpos)
        if not bestd or d < bestd then best, bestd, bestnode = u, d, n end
      end
    end
  end
  if best then hd2vr_log(string.format("fp: player unit %d, head %.2f m from camera", handle(best), bestd)) end
  return best, bestnode
end

local function step()
  if not state.cam or not U.alive(state.cam) then state.cam = find_camera() end
  if not state.cam then hook.hd2vr_cam_set_eye(0, 0, 0, 0) return end
  local pose = U.world_pose(state.cam, 1)
  local cpos = M.translation(pose)
  if not state.player or not U.alive(state.player) then
    state.player, state.head = find_player(cpos)
    if not state.player then hook.hd2vr_cam_set_eye(0, 0, 0, 0) return end
  end
  local eye = U.world_position(state.player, state.head) + M.y(pose) * hd2vr_fp.forward + M.z(pose) * hd2vr_fp.up
  hook.hd2vr_cam_set_eye(eye.x, eye.y, eye.z, 1)
end

local orig_render = render
render = function(...)
  if state then
    local ok, err = pcall(step)
    state.frames = state.frames + 1
    if not ok then
      state.errs = state.errs + 1
      if state.errs == 1 then hd2vr_log("fp error: " .. tostring(err)) end
      state.player = nil
    end
  end
  return orig_render(...)
end

function hd2vr_fp_stop()
  render = orig_render
  hd2vr_fp_stop = nil
  if hook then hook.hd2vr_cam_set_eye(0, 0, 0, 0) end
  if state then hd2vr_log(string.format("fp: stopped after %d frames, %d errors", state.frames, state.errs)) end
  state = nil
end
hd2vr_log("fp: first-person camera on")
