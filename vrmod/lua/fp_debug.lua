-- First-person debug: log head, game camera and target eye positions (world), plus head node axes.
local ffi = require("ffi")
local A, W, U, V, M = stingray.Application, stingray.World, stingray.Unit, stingray.Vector3, stingray.Matrix4x4
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end
local world = A.main_world()
local cam, player
for _, u in ipairs(W.units(world)) do
  local h = handle(u)
  if h == 5 then cam = u end
  if h == 8553 then player = u end
end
local pose = U.world_pose(cam, 1)
local n = U.node(player, "head")
local hp = U.world_position(player, n)
hd2vr_log("fpdbg cam " .. tostring(M.translation(pose)) .. " fwd " .. tostring(M.y(pose)) .. " up " .. tostring(M.z(pose)))
hd2vr_log("fpdbg head " .. tostring(hp) .. " root " .. tostring(U.world_position(player, 1)))
for _, nn in ipairs({ "neck", "j_spine2", "spine2", "eye_l", "j_eye_l", "jaw", "helmet", "j_head_end", "head_end" }) do
  local ok, has = pcall(U.has_node, player, nn)
  if ok and has then hd2vr_log("fpdbg node " .. nn .. " " .. tostring(U.world_position(player, U.node(player, nn)))) end
end
local hr = U.world_rotation(player, n)
hd2vr_log("fpdbg head axes x " .. tostring(stingray.Quaternion.right(hr)) .. " y " .. tostring(stingray.Quaternion.forward(hr)) .. " z " .. tostring(stingray.Quaternion.up(hr)))
