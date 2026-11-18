-- Controls hint that fades out after a few seconds.
local Hud = { seconds = 8 }

function Hud:update(dt)
  local t = ace.time()
  if t > self.seconds then return end
  local w, h = ace.screen()
  local k = math.min(1, (self.seconds - t) / 2)
  ace.text("WASD walk  SHIFT run  SPACE jump  MOUSE look  ESC free cursor", 6, h - 12, 0.95 * k, 0.88 * k, 0.75 * k)
end

return Hud
