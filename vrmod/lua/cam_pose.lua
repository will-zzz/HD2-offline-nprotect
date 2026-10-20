-- Logs the gameplay camera's (world 1, handle 5) poses at full float precision, for memory scans.
local ffi = require("ffi")
local A, W, U, M = stingray.Application, stingray.World, stingray.Unit, stingray.Matrix4x4
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end

local cam
for _, u in ipairs(W.units(A.main_world())) do
  if handle(u) == 5 then cam = u break end
end
if not cam then hd2vr_log("cam_pose: gameplay camera not found") return end

local function dump(name, m)
  local e = { M.to_elements(m) }
  local s = {}
  for i = 1, #e do s[#s + 1] = string.format("%.9g", e[i]) end
  hd2vr_log("cam_pose " .. name .. " " .. table.concat(s, " "))
end
dump("world", U.world_pose(cam, 1))
dump("local", U.local_pose(cam, 1))
local okc, c = pcall(U.camera, cam, 1)
if okc and c then
  local ok, m = pcall(stingray.Camera.world_pose, c)
  if ok and m then dump("camworld", m) end
  local ok2, m2 = pcall(stingray.Camera.local_pose, c, cam)
  if ok2 and m2 then dump("camlocal", m2) end
  local okf, f = pcall(stingray.Camera.vertical_fov, c)
  if okf then hd2vr_log(string.format("cam_pose fov %.9g", f)) end
  local okn, n = pcall(stingray.Camera.near_range, c)
  local okr, r = pcall(stingray.Camera.far_range, c)
  if okn and okr then hd2vr_log(string.format("cam_pose near %.9g far %.9g", n, r)) end
end
