-- First-person recon: find units near the gameplay camera that have a head-like node.
local ffi = require("ffi")
local A, W, U, M = stingray.Application, stingray.World, stingray.Unit, stingray.Matrix4x4
local V = stingray.Vector3
local function handle(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end

local world = A.main_world()
local units = W.units(world)
local cam
for _, u in ipairs(units) do if handle(u) == 5 then cam = u break end end
if not cam then hd2vr_log("fp: camera unit 5 not found") return end
local cpos = U.world_position(cam, 1)
hd2vr_log("fp: camera at " .. tostring(cpos) .. ", " .. #units .. " units")

local names = { "j_head", "head", "Head", "j_neck", "neck", "c_head", "j_spine2", "root_point", "j_hips", "camera_attach", "j_eye_l", "fp_camera" }
local found = {}
for _, u in ipairs(units) do
  if U.alive(u) and U.num_scene_graph_items(u) > 1 then
    for _, n in ipairs(names) do
      local ok, has = pcall(U.has_node, u, n)
      if ok and has then
        local idx = U.node(u, n)
        local okp, p = pcall(U.world_position, u, idx)
        if okp then
          found[#found + 1] = { d = V.length(p - cpos), h = handle(u), n = n, p = p, items = U.num_scene_graph_items(u) }
        end
      end
    end
  end
end
table.sort(found, function(a, b) return a.d < b.d end)
for i = 1, math.min(#found, 25) do
  local f = found[i]
  hd2vr_log(string.format("fp: unit %d (%d nodes) node %-12s at %s  dist %.2f", f.h, f.items, f.n, tostring(f.p), f.d))
end
hd2vr_log("fp: " .. #found .. " head-like nodes total")
