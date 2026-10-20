-- Logs the gameplay camera's forward vector and yaw/pitch (several conventions) for aim-variable scans.
local ffi = require("ffi")
local A, W, U, M = stingray.Application, stingray.World, stingray.Unit, stingray.Matrix4x4
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end
for _, u in ipairs(W.units(A.main_world())) do
  if handle(u) == 5 then
    local f = M.y(U.world_pose(u, 1))
    hd2vr_log(string.format("camdir %.7f %.7f %.7f yaw_atan2(-x,y) %.7f pitch %.7f", f.x, f.y, f.z,
      math.atan2(-f.x, f.y), math.asin(f.z)))
    return
  end
end
hd2vr_log("camdir: camera not found")
