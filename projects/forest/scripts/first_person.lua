-- First-person walking: put on the entity that has the Camera (and a Collider
-- around the body, below the eyes). Walks on any mesh below it (ace.ground),
-- slides along colliders (trees, walls), jumps and falls.
--   Mouse: look (click to capture the mouse, Esc to release; in the editor
--          hold the right button). Arrow keys or the right stick also look.
--   WASD / left stick: walk.  Shift / left trigger: run.  Space / A: jump.
local Walker = { speed = 4.5, run = 9, eye = 1.7, step = 0.9, jump = 5.5, gravity = 18, sensitivity = 0.12, turn = 110 }

function Walker:start()
  local _, ry = ace.rotation(self.entity)
  self.yaw, self.pitch, self.vy = ry, 0, 0
  ace.lock_mouse(true)
end

function Walker:update(dt)
  -- Look.
  if ace.key_pressed("Escape") then ace.lock_mouse(false) end
  if ace.mouse_button(1) and not ace.mouse_locked() then ace.lock_mouse(true) end
  if ace.mouse_locked() or ace.mouse_button(3) then
    local mx, my = ace.mouse_delta()
    self.yaw = self.yaw - mx * self.sensitivity
    self.pitch = self.pitch - my * self.sensitivity
  end
  local turnX, turnY = ace.gamepad_axis("rightx"), ace.gamepad_axis("righty")
  if ace.key("Left") then turnX = turnX - 1 end
  if ace.key("Right") then turnX = turnX + 1 end
  if ace.key("Up") then turnY = turnY - 1 end
  if ace.key("Down") then turnY = turnY + 1 end
  self.yaw = self.yaw - turnX * self.turn * dt
  self.pitch = math.max(-85, math.min(85, self.pitch - turnY * self.turn * 0.7 * dt))
  ace.set_look(self.entity, self.yaw, self.pitch)

  -- Walk, relative to where we look.
  local f = ace.gamepad_axis("lefty") * -1
  local s = ace.gamepad_axis("leftx")
  if ace.key("W") then f = f + 1 end
  if ace.key("S") then f = f - 1 end
  if ace.key("D") then s = s + 1 end
  if ace.key("A") then s = s - 1 end
  local len = math.sqrt(f * f + s * s)
  if len > 1 then f, s = f / len, s / len end
  local running = ace.key("Left Shift") or ace.gamepad_axis("left_trigger") > 0.5
  local speed = running and self.run or self.speed
  local a = math.rad(self.yaw)
  local fx, fz = -math.sin(a), -math.cos(a)   -- forward (the camera looks along -Z)
  local rx, rz = math.cos(a), -math.sin(a)    -- right
  ace.move(self.entity, (fx * f + rx * s) * speed * dt, 0, (fz * f + rz * s) * speed * dt)

  -- Stand on the ground: step up small ledges, fall off big ones.
  local x, y, z = ace.position(self.entity)
  local feet = y - self.eye
  local ground = ace.ground(x, z, feet + self.step)
  local grounded = ground and feet <= ground + 0.05
  if grounded and (ace.key_pressed("Space") or ace.gamepad_pressed("south")) then self.vy = self.jump end
  self.vy = self.vy - self.gravity * dt
  feet = feet + self.vy * dt
  if ground and feet <= ground then
    feet, self.vy = ground, 0
  end
  if feet < -200 then feet, self.vy = (ace.ground(x, z) or 0), 0 end -- fell out of the world
  ace.set_position(self.entity, x, feet + self.eye, z)
end

return Walker
