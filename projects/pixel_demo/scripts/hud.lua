-- On-screen text: the place name, centred at the top, and controls at the bottom.
-- Set the name per scene with the Script params:  title = Oakvale
local Hud = { title = "Oakvale" }

function Hud:update(dt)
  local w, h = ace.screen()
  local title_scale = 2
  local tw = ace.text_width(self.title) * title_scale
  ace.text(self.title, math.floor((w - tw) / 2), 6, 1.0, 0.92, 0.55, title_scale)
  ace.text("WASD / arrows, gamepad or touch to walk", 6, h - 12, 0.9, 0.95, 1.0)
end

return Hud
