-- New-unit monitor. Wraps the game's Lua update(dt) so it runs every frame; four times a second it
-- diffs the main world's units and logs each new one (resource hash + position). Built only on
-- calls proven safe on every unit: alive, id, resource_name, num_scene_graph_items, and
-- world_position(u, 1) when the unit has at least one node.
-- Stop with: hd2vr_monitor_stop()
if hd2vr_monitor_on then hd2vr_log("monitor: already running") return end

local A, W, U = stingray.Application, stingray.World, stingray.Unit
local orig_update = update
local INTERVAL = 0.25          -- seconds between diffs
local MAX_PER_TICK = 40        -- new units logged per diff (level loads spawn thousands)
local MAX_LINES = 20000        -- total lines before the monitor goes quiet

-- Neither Unit.id nor tostring(unit) is unique per unit (both follow the resource name). The unit
-- itself is a small handle, unique per live unit and stable across calls, so key by its value via
-- the FFI. (tostring() on cdata prints "[cdata (deleted)]" in this build, hence tonumber.)
local function make_key_fn()
  local ffi = require("ffi")
  local function raw(u) return tonumber(ffi.cast("uintptr_t", ffi.cast("void *", u))) end
  local units = W.units(A.main_world())
  local ids, n = {}, 0
  for i = 1, #units do
    local k = raw(units[i])
    if not ids[k] then ids[k] = true; n = n + 1 end
  end
  hd2vr_log("monitor: " .. #units .. " units, " .. n .. " distinct handle keys")
  return raw
end

local key = make_key_fn()
local known, acc, lines, ticks = nil, 0, 0, 0

local function describe(u)
  local name = tostring(U.resource_name(u))
  local pos = "no-nodes"
  if U.num_scene_graph_items(u) > 0 then
    local ok, p = pcall(U.world_position, u, 1)
    pos = ok and tostring(p) or "?"
  end
  return name .. "  " .. pos
end

local function diff()
  local units = W.units(A.main_world())
  local seen, fresh = {}, {}
  for i = 1, #units do
    local u = units[i]
    if U.alive(u) then
      local k = key(u)
      seen[k] = true
      if known and not known[k] then fresh[#fresh + 1] = u end
    end
  end
  if known and #fresh > 0 and lines < MAX_LINES then
    local t = string.format("%.2f", A.time_since_launch())
    hd2vr_log("monitor t=" .. t .. ": +" .. #fresh .. " units (total " .. #units .. ")")
    for i = 1, math.min(#fresh, MAX_PER_TICK) do
      hd2vr_log("  new " .. describe(fresh[i]))
      lines = lines + 1
    end
    lines = lines + 1
  end
  known = seen
end

update = function(dt)
  local r = orig_update(dt)
  if hd2vr_monitor_on then
    acc = acc + (dt or 0)
    ticks = ticks + 1
    if acc >= INTERVAL then
      acc = 0
      local ok, err = pcall(diff)
      if not ok then
        hd2vr_monitor_on = false
        hd2vr_log("monitor: disabled after error: " .. tostring(err))
      end
    end
  end
  return r
end

function hd2vr_monitor_stop()
  hd2vr_monitor_on = false
  update = orig_update
  hd2vr_log("monitor: stopped after " .. ticks .. " frames")
end

hd2vr_monitor_on = true
hd2vr_log("monitor: installed")
