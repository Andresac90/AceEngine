-- Moves the entity on the ground plane with WASD or the arrow keys.
local Player = { speed = 4 }

function Player:update(dt)
  local dx, dz = 0, 0
  if ace.key("A") or ace.key("Left") then dx = dx - 1 end
  if ace.key("D") or ace.key("Right") then dx = dx + 1 end
  if ace.key("W") or ace.key("Up") then dz = dz - 1 end
  if ace.key("S") or ace.key("Down") then dz = dz + 1 end
  if dx ~= 0 or dz ~= 0 then
    local len = math.sqrt(dx * dx + dz * dz)
    local x, y, z = ace.position(self.entity)
    ace.set_position(self.entity, x + dx / len * self.speed * dt, y, z + dz / len * self.speed * dt)
  end
end

return Player
