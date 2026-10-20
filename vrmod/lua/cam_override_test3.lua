-- Camera override test 3: +30 deg yaw on the gameplay camera before render(), then force the
-- scene graph to recompute its world transform (World.update_unit), since the engine only does that
-- in update. Stop: hd2vr_cam_stop()
local ffi = require("ffi")
local A, W, U, Q, V = stingray.Application, stingray.World, stingray.Unit, stingray.Quaternion, stingray.Vector3
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end

if hd2vr_cam_stop then hd2vr_cam_stop() end

local world = A.main_world()
local cam
for _, u in ipairs(W.units(world)) do
  if handle(u) == 5 then cam = u break end
end
if not cam then hd2vr_log("cam test3: gameplay camera not found") return end

local test = { frames = 0, errs = 0 }
local extra = Q.axis_angle(V(0, 0, 1), math.rad(hd2vr_cam_yaw or 30))
local function apply()
  U.set_local_rotation(cam, 1, Q.multiply(extra, U.local_rotation(cam, 1)))
  W.update_unit(world, cam)
end

local orig_render = render
render = function(...)
  if test then
    local ok, err = pcall(apply)
    test.frames = test.frames + 1
    if not ok then test.errs = test.errs + 1; if test.errs == 1 then hd2vr_log("cam test3 error: " .. tostring(err)) end end
  end
  return orig_render(...)
end

function hd2vr_cam_stop()
  render = orig_render
  hd2vr_cam_stop = nil
  if test then hd2vr_log(string.format("cam test3: stopped after %d frames, %d errors", test.frames, test.errs)) end
  test = nil
end
hd2vr_log("cam test3: running (+30 deg yaw + World.update_unit before render)")
