# Lua scripting

Add a **Script** component to an entity and point it at a `.lua` file. The file
returns a table; the engine creates one instance per entity.

```lua
local Mover = { speed = 3 }          -- fields here are defaults shared by all instances

function Mover:start()               -- optional, once, before the first update
  self.t = 0
end

function Mover:update(dt)            -- optional, every frame; dt in seconds
  local x, y, z = ace.position(self.entity)
  ace.set_position(self.entity, x + self.speed * dt, y, z)
end

return Mover
```

`self.entity` is this entity's id. A script that raises an error is logged
(with a traceback) and stops running; other scripts continue.

## API

| Function | Notes |
| --- | --- |
| `ace.log(value)` | prints to the console |
| `ace.time()` | seconds since play started |
| `ace.key(name)` / `ace.key_pressed(name)` | held / went down this frame. Names are SDL key names: `"A"`, `"Space"`, `"Left"`, `"Return"`, `"Left Shift"` |
| `ace.find(name)` | entity id or `nil` |
| `ace.spawn(name)` | new empty entity |
| `ace.destroy(e)` | destroys the entity and its children |
| `ace.position(e)` / `ace.set_position(e, x, y, z)` | local position |
| `ace.rotation(e)` / `ace.set_rotation(e, x, y, z)` | Euler degrees |
| `ace.scale(e)` / `ace.set_scale(e, x, y, z)` | |
| `ace.set_mesh(e, mesh [, r, g, b])` | adds/updates a MeshRenderer |
| `ace.set_color(e, r, g, b)` | MeshRenderer color (sRGB 0..1) |
| `ace.set_sprite(e, frame [, fps [, flip_x]])` | SpriteRenderer sheet frame; `fps > 0` loops the sheet; `flip_x` mirrors it |

Passing a destroyed or invalid entity id raises a Lua error.

Scripts run sandboxed: only `base`, `table`, `string`, `math` and `utf8` are
available (no `io`, `os`, `require`, `load`).

To add a function: write an `l_name` C function in `engine/script/script.cpp`,
add it to the `api` table, and document it here.
