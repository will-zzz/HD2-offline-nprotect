-- Spawn test: one impact unit (3f84fd1fde7713de) at the 500kg crater from the recon run.
local W = stingray.World
local world = stingray.Application.main_world()
local id = stingray.IdString64.from_hex("3f84fd1fde7713de")
local pos = stingray.Vector3(121.23, 46.94, 0)
hd2vr_log("spawn_test: spawning " .. tostring(id) .. " at " .. tostring(pos))
local ok, unit = pcall(W.spawn_unit, world, id, pos)
hd2vr_log("spawn_test: ok=" .. tostring(ok) .. " result=" .. tostring(unit))
