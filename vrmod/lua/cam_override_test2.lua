-- Camera override test 2: apply +30 deg yaw at the end of update() AND before render(), through both
-- the camera unit's node and the Camera object. Logs which calls succeed. Stop: hd2vr_cam_stop()
local ffi = require("ffi")
local A, W, U, Q, V, C = stingray.Application, stingray.World, stingray.Unit, stingray.Quaternion, stingray.Vector3, stingray.Camera
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end

if hd2vr_cam_stop then hd2vr_cam_stop() end

local cam
for _, u in ipairs(W.units(A.main_world())) do
  if handle(u) == 5 then cam = u break end
end
if not cam or U.num_cameras(cam) == 0 then hd2vr_log("cam test2: gameplay camera not found") return end

local okc, camobj = pcall(U.camera, cam, 1)
hd2vr_log("cam test2: Unit.camera(u, 1) -> " .. tostring(okc) .. " " .. tostring(camobj))
if okc and camobj then
  local okr, r = pcall(C.local_rotation, camobj, cam)
  hd2vr_log("cam test2: Camera.local_rotation -> " .. tostring(okr) .. " " .. tostring(r))
  local okm, m = pcall(C.mode, camobj)
  hd2vr_log("cam test2: Camera.mode -> " .. tostring(okm) .. " " .. tostring(m) .. " (MONO=" .. tostring(C.MONO) .. ", STEREO=" .. tostring(C.STEREO) .. ")")
end

local test = { frames = 0, errs = 0 }
local extra = Q.axis_angle(V(0, 0, 1), math.rad(30))
local function apply()
  if not U.alive(cam) then error("camera unit gone") end
  U.set_local_rotation(cam, 1, Q.multiply(extra, U.local_rotation(cam, 1)))
  if okc and camobj then
    C.set_local_rotation(camobj, cam, Q.multiply(extra, C.local_rotation(camobj, cam)))
  end
end

local orig_update, orig_render = update, render
update = function(...)
  local r = orig_update(...)
  if test then
    local ok, err = pcall(apply)
    test.frames = test.frames + 1
    if not ok then test.errs = test.errs + 1; if test.errs == 1 then hd2vr_log("cam test2 error: " .. tostring(err)) end end
  end
  return r
end
render = function(...)
  if test then pcall(apply) end
  return orig_render(...)
end

function hd2vr_cam_stop()
  update, render = orig_update, orig_render
  hd2vr_cam_stop = nil
  if test then hd2vr_log(string.format("cam test2: stopped after %d frames, %d errors", test.frames, test.errs)) end
  test = nil
end
hd2vr_log("cam test2: running (+30 deg yaw at end of update and before render)")
