-- Crowd showcase: a scripted hockey night, so every animation shows up.
-- Put on the entity with the Crowd. Not a game: goals and the wave happen on
-- a fixed timeline that repeats every `cycle` seconds.
local Show = { cycle = 60 }

local skaters = {}

function Show:start()
  for k = 1, 10 do skaters[k] = ace.find("Skater " .. k) end
  self.home, self.away, self.last = 0, 0, -1
  self.caption, self.captionUntil = "", 0
end

-- Skaters glide on smooth loops around the ice and face where they go.
local function skate(t)
  for k, e in ipairs(skaters) do
    if e then
      local p = t * (0.22 + 0.03 * k) + k * 1.7
      local x = 21 * math.sin(p) + 4 * math.sin(p * 2.3 + k)
      local z = 8.5 * math.sin(p * 1.6 + k * 0.9)
      local dx = 21 * math.cos(p) + 9.2 * math.cos(p * 2.3 + k)
      local dz = 13.6 * math.cos(p * 1.6 + k * 0.9)
      ace.set_position(e, x, 0, z)
      ace.set_look(e, math.deg(math.atan(-dx, -dz)), 0)
    end
  end
end

local function say(self, text, seconds)
  self.caption, self.captionUntil = text, ace.time() + seconds
end

function Show:update(dt)
  local t = ace.time()
  skate(t)
  local beat = math.floor(t % self.cycle)
  if beat ~= self.last then
    self.last = beat
    if beat == 5 then                          -- home goal: home fans jump, visitors slump
      self.home = self.home + 1
      ace.crowd_event(0, "celebrate", 7); ace.crowd_event(1, "dejected", 7); ace.crowd_event(2, "cheer", 6)
      ace.play_sound("assets/horn.wav", 0.8)
      say(self, "GOAL!", 4)
    elseif beat == 20 then                     -- the wave, once round the bowl
      ace.crowd_wave(1)
      say(self, "THE WAVE", 4)
    elseif beat == 46 then                     -- visitors score
      self.away = self.away + 1
      ace.crowd_event(1, "celebrate", 6); ace.crowd_event(0, "dejected", 5)
      say(self, "VISITORS SCORE", 4)
    elseif beat == 53 then
      ace.crowd_event(0, "flail", 5)           -- the home crowd gets loud again
    end
  end
  local w, h = ace.screen()
  ace.text(string.format("ACES %d  -  %d VISITORS", self.home, self.away), 6, 6, 1, 0.95, 0.85)
  ace.text("18125 FANS  PATH TRACED", 6, h - 12, 0.8, 0.85, 0.95)
  if t < self.captionUntil then
    local tw = ace.text_width(self.caption) * 2
    ace.text(self.caption, math.floor((w - tw) / 2), 26, 1, 0.85, 0.3, 2)
  end
end

return Show
