-- Showcase camera: cycles through broadcast-style shots of the arena.
--   Keys 1-6 pick a shot, 0 goes back to cycling. Param `shot=N` fixes one
--   and `hold=T` freezes it at T seconds into the shot (screenshots, timings).
local Cam = { shot = 0, every = 9, hold = -1 }

local function look(e, px, py, pz, tx, ty, tz)
  ace.set_position(e, px, py, pz)
  local dx, dy, dz = tx - px, ty - py, tz - pz
  local flat = math.sqrt(dx * dx + dz * dz)
  -- The camera looks down -Z: yaw turns it about +Y, pitch tilts it up.
  ace.set_look(e, math.deg(math.atan(-dx, -dz)), math.deg(math.atan(dy, flat)))
end

local shots = {
  function(e, t) local x = 14 * math.sin(t * 0.18); look(e, x, 12.8, -32, x * 0.8, 0, 0) end,           -- 1 broadcast (press box)
  function(e, t) look(e, -47 + t * 0.4, 21, -25, 0, 4, 3) end,                                          -- 2 high corner
  function(e, t) look(e, -24 + t * 0.6, 0.9, -9.5, 2 + t * 0.6, 3.5, 9) end,                            -- 3 ice level (reflections)
  function(e, t) look(e, 8 + t * 0.2, 1.9, -14.6, 20, 5.5, -22) end,                                     -- 4 the crowd, from the walkway
  function(e, t) local a = t * 0.12; look(e, 34 * math.cos(a), 21, 22 * math.sin(a), 0, 10, 0) end,     -- 5 orbit
  function(e, t) look(e, 0, 13.5, -10, 0, 18, 0) end,                                                   -- 6 jumbotron
}

function Cam:update(dt)
  for k = 0, #shots do
    if ace.key_pressed(tostring(k)) then self.shot = k end
  end
  local t = ace.time()
  local s, local_t = self.shot, t
  if s == 0 then
    s = math.floor(t / self.every) % #shots + 1
    local_t = t % self.every
  end
  if self.hold >= 0 then local_t = self.hold end
  shots[s](self.entity, local_t)
end

return Cam
