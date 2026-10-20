-- Camera override test: add a fixed yaw to the gameplay camera every frame, in the render callback.
-- If the game resets the camera each frame we add the offset to its fresh rotation; if it doesn't,
-- we re-use the base we saw, so the view never spins. Stop with: hd2vr_cam_stop()
local ffi = require("ffi")
local A, W, U, Q, V = stingray.Application, stingray.World, stingray.Unit, stingray.Quaternion, stingray.Vector3
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end

if hd2vr_cam_stop then hd2vr_cam_stop() end

-- the camera that turned with the player's view (world 1, handle 5)
local cam
for _, u in ipairs(W.units(A.main_world())) do
  if handle(u) == 5 then cam = u break end
end
if not cam or U.num_cameras(cam) == 0 then hd2vr_log("cam test: gameplay camera not found") return end

local test = { yaw = 30, frames = 0, resets = 0 }
local last_set, last_base
local function same(a, b)
  local ax, ay, az, aw = Q.to_elements(a)
  local bx, by, bz, bw = Q.to_elements(b)
  return math.abs(ax - bx) + math.abs(ay - by) + math.abs(az - bz) + math.abs(aw - bw) < 1e-5
end

local orig_render = render
render = function(...)
  if test then
    local ok, err = pcall(function()
      if not U.alive(cam) then error("camera unit gone") end
      local cur = U.local_rotation(cam, 1)
      local base = cur
      if last_set and same(cur, last_set) then base = last_base else test.resets = test.resets + 1 end
      local set = Q.multiply(Q.axis_angle(V(0, 0, 1), math.rad(test.yaw)), base)
      U.set_local_rotation(cam, 1, set)
      last_set, last_base = set, base
      test.frames = test.frames + 1
    end)
    if not ok then hd2vr_log("cam test stopped: " .. tostring(err)); test = nil end
  end
  return orig_render(...)
end

function hd2vr_cam_stop()
  render = orig_render
  hd2vr_cam_stop = nil
  if test then hd2vr_log(string.format("cam test: stopped after %d frames (game reset the camera %d times)", test.frames, test.resets)) end
  test = nil
end
function hd2vr_cam_report()
  if test then hd2vr_log(string.format("cam test: %d frames, game reset the camera %d times", test.frames, test.resets)) end
end
hd2vr_log("cam test: +30 deg yaw on the gameplay camera, applied in render()")
