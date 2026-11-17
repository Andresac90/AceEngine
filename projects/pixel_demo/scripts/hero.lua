-- Walks the hero with WASD / arrow keys, relative to the camera, and plays the
-- walk cycle from its sprite sheet while moving.
local Hero = { speed = 3.5, facing_left = false, step_timer = 0 }

function Hero:update(dt)
  local right, up = 0, 0
  if ace.key("A") or ace.key("Left") then right = right - 1 end
  if ace.key("D") or ace.key("Right") then right = right + 1 end
  if ace.key("W") or ace.key("Up") then up = up + 1 end
  if ace.key("S") or ace.key("Down") then up = up - 1 end
  -- Gamepad left stick.
  local sx, sy = ace.gamepad_axis("leftx"), ace.gamepad_axis("lefty")
  if sx ~= 0 or sy ~= 0 then right, up = sx, -sy end
  -- Touch / mouse: hold anywhere to walk in that direction from the screen centre.
  local down, px, py = ace.pointer()
  if down then
    local tx, ty = px - 0.5, 0.5 - py
    if math.abs(tx) > 0.05 or math.abs(ty) > 0.05 then right, up = tx, ty end
  end
  if right == 0 and up == 0 then
    ace.set_sprite(self.entity, 0, 0, self.facing_left) -- stand still on the first frame
    return
  end

  -- "Up" means away from the camera along the ground.
  local x, y, z = ace.position(self.entity)
  local fx, fz = 0, -1
  local camera = ace.find("Camera")
  if camera then
    local cx, _, cz = ace.position(camera)
    local len = math.sqrt((x - cx) ^ 2 + (z - cz) ^ 2)
    if len > 0.001 then fx, fz = (x - cx) / len, (z - cz) / len end
  end
  local dx, dz = fx * up - fz * right, fz * up + fx * right
  local len = math.sqrt(dx * dx + dz * dz)
  ace.move(self.entity, dx / len * self.speed * dt, 0, dz / len * self.speed * dt) -- slides along walls

  if math.abs(right) > 0.2 then self.facing_left = right < 0 end
  ace.set_sprite(self.entity, 0, 8, self.facing_left)

  -- A footstep on every other frame of the 8 fps walk cycle.
  self.step_timer = self.step_timer - dt
  if self.step_timer <= 0 then
    ace.play_sound("assets/step.wav", 0.5)
    self.step_timer = 0.25
  end
end

return Hero
