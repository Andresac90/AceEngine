-- Spins the entity and bobs it up and down.
local Spin = {}

function Spin:start()
  local x, y, z = ace.position(self.entity)
  self.base_y = y
  self.angle = 0
end

function Spin:update(dt)
  self.angle = self.angle + 45 * dt
  ace.set_rotation(self.entity, 0, self.angle, 0)
  local x, _, z = ace.position(self.entity)
  ace.set_position(self.entity, x, self.base_y + math.sin(ace.time() * 2) * 0.25, z)
end

return Spin
