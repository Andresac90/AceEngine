-- A trigger that loads another scene when the player walks into it.
-- Put it on an entity with a non-solid Collider; set params like:
--   target = scenes/house.scene.json; who = Hero
local Door = { target = "", who = "Hero" }

function Door:update(dt)
  local player = ace.find(self.who)
  if not player or self.target == "" then return end
  for _, e in ipairs(ace.overlaps(self.entity)) do
    if e == player then
      ace.load_scene(self.target)
      return
    end
  end
end

return Door
