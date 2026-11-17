-- Keeps the camera at its starting offset from the entity named `target`.
local Follow = { target = "Hero", smoothing = 6 }

function Follow:start()
  local target = ace.find(self.target)
  if not target then return end
  local cx, cy, cz = ace.position(self.entity)
  local tx, ty, tz = ace.position(target)
  self.offset = { cx - tx, cy - ty, cz - tz }
end

function Follow:update(dt)
  local target = ace.find(self.target)
  if not target or not self.offset then return end
  local cx, cy, cz = ace.position(self.entity)
  local tx, ty, tz = ace.position(target)
  local k = math.min(1, self.smoothing * dt)
  ace.set_position(self.entity,
    cx + (tx + self.offset[1] - cx) * k,
    cy + (ty + self.offset[2] - cy) * k,
    cz + (tz + self.offset[3] - cz) * k)
end

return Follow
