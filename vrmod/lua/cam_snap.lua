-- Camera snapshot/compare. First run stores every camera unit's world pose; later runs report the
-- cameras whose pose changed since the stored snapshot (the gameplay camera turns with the view).
local ffi = require("ffi")
local A, W, U = stingray.Application, stingray.World, stingray.Unit
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end

local now = {}
for wi, world in ipairs(A.worlds()) do
  local units = W.units(world)
  for i = 1, #units do
    local u = units[i]
    local ok, cams = pcall(U.num_cameras, u)
    if ok and cams and cams > 0 and U.num_scene_graph_items(u) > 0 then
      local okp, pose = pcall(U.world_pose, u, 1)
      if okp then now[wi .. ":" .. handle(u)] = tostring(pose) end
    end
  end
end

if not hd2vr_cam_snap then
  hd2vr_cam_snap = now
  local n = 0 for _ in pairs(now) do n = n + 1 end
  hd2vr_log("cam_snap: stored " .. n .. " camera poses; run again after looking around")
else
  local changed = 0
  for k, pose in pairs(now) do
    local before = hd2vr_cam_snap[k]
    if before and before ~= pose then
      changed = changed + 1
      hd2vr_log("cam_snap: CHANGED " .. k)
      hd2vr_log("   before " .. before)
      hd2vr_log("   now    " .. pose)
    end
  end
  hd2vr_log("cam_snap: " .. changed .. " camera(s) changed")
  hd2vr_cam_snap = now
end
