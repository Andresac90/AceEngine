-- Walks the hero with WASD / arrow keys, relative to the camera, and plays the
-- walk cycle from its sprite sheet while moving.
local Hero = { speed = 3.5, facing_left = false }

function Hero:update(dt)
  local right, up = 0, 0
  if ace.key("A") or ace.key("Left") then right = right - 1 end
  if ace.key("D") or ace.key("Right") then right = right + 1 end
  if ace.key("W") or ace.key("Up") then up = up + 1 end
  if ace.key("S") or ace.key("Down") then up = up - 1 end
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
  ace.set_position(self.entity, x + dx / len * self.speed * dt, y, z + dz / len * self.speed * dt)

  if right ~= 0 then self.facing_left = right < 0 end
  ace.set_sprite(self.entity, 0, 8, self.facing_left)
end

return Hero
