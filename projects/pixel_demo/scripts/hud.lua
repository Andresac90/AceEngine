-- On-screen text: the place name, centred at the top, and controls at the bottom.
local Hud = { title = "Oakvale" }

function Hud:update(dt)
  local w, h = ace.screen()
  local title_scale = 2
  local tw = ace.text_width(self.title) * title_scale
  ace.text(self.title, math.floor((w - tw) / 2), 6, 1.0, 0.92, 0.55, title_scale)
  ace.text("WASD / arrows or touch to walk", 6, h - 12, 0.9, 0.95, 1.0)
end

return Hud
