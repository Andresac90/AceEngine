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

Scripts reload automatically when you save the file while the game is running
(the instance restarts: `start` runs again, entity transforms are kept).

`self.entity` is this entity's id.

### Keeping state between scenes

All scripts share one Lua state that lives for the whole game, so a global
table survives `ace.load_scene`:

```lua
Game = Game or { coins = 0 }   -- created once, kept across scene changes
```

For progress that must survive quitting, use `ace.save_data` / `ace.load_data`.
The file lives in the player's per-user app data folder
(`~/Library/Application Support/AceEngine/<project>/save.json` on macOS).

### Per-entity parameters

The Script component's `params` field sets fields on the instance before
`start` runs, so one script can serve many entities:

```
target = scenes/house.scene.json; speed = 2.5; locked = false
```

Numbers become numbers, `true`/`false` become booleans, anything else is text. A script that raises an error is logged
(with a traceback) and stops running; other scripts continue.

## API

| Function | Notes |
| --- | --- |
| `ace.log(value)` | prints to the console |
| `ace.time()` | seconds since play started |
| `ace.key(name)` / `ace.key_pressed(name)` | held / went down this frame. Names are SDL key names: `"A"`, `"Space"`, `"Left"`, `"Return"`, `"Left Shift"` |
| `ace.pointer()` | returns `down, x, y`: left mouse button or a finger on a touch screen; `x`, `y` run 0..1 across the window |
| `ace.play_sound(path [, volume [, loop]])` | plays a project `.wav`; returns an id (0 if it could not play). Sounds stop when play mode stops |
| `ace.stop_sound(id)` | |
| `ace.text(text, x, y [, r, g, b [, scale]])` | draws text for this frame with the built-in pixel font; `x`, `y` in game pixels from the top-left; `\n` starts a new line; lowercase prints as uppercase |
| `ace.screen()` | returns `width, height` of the screen in game pixels, for laying out text |
| `ace.text_width(text)` | width in game pixels at scale 1 (for centring) |
| `ace.load_scene(path)` | switches to another scene (e.g. `"scenes/cave.scene.json"`) after this frame; every script restarts. In the editor the switch is undone when play stops |
| `ace.overlaps(e)` | array of entities whose **Collider** overlaps `e`'s. Untick `solid` on a Collider to make a trigger the player can walk into |
| `ace.raycast(ox, oy, oz, dx, dy, dz [, max [, ignore]])` | first solid Collider along the ray: returns `entity, distance, x, y, z`, or `nil` |
| `ace.gamepad_axis(name)` | `"leftx"`, `"lefty"`, `"rightx"`, `"righty"` (-1..1, dead zone applied), `"left_trigger"`, `"right_trigger"` (0..1) |
| `ace.gamepad_button(name)` / `ace.gamepad_pressed(name)` | held / went down this frame: `"south"` (A / cross), `"east"`, `"west"`, `"north"`, `"start"`, `"back"`, `"dpup"`, `"dpdown"`, `"dpleft"`, `"dpright"`, `"leftshoulder"`, `"rightshoulder"` |
| `ace.save_data(table)` | writes the table (numbers, strings, booleans, nested tables) as the game's save file; returns `true` on success |
| `ace.load_data()` | the saved table, or `nil` if there is no save yet |
| `ace.find(name)` | entity id or `nil` |
| `ace.spawn(name)` | new empty entity |
| `ace.destroy(e)` | destroys the entity and its children |
| `ace.position(e)` / `ace.set_position(e, x, y, z)` | local position |
| `ace.rotation(e)` / `ace.set_rotation(e, x, y, z)` | Euler degrees |
| `ace.scale(e)` / `ace.set_scale(e, x, y, z)` | |
| `ace.move(e, dx, dy, dz)` | moves by a world-space delta, stopping at solid **Collider** boxes and sliding along them; returns the movement applied. Without a Collider on `e` it moves freely |
| `ace.set_mesh(e, mesh [, r, g, b])` | adds/updates a MeshRenderer |
| `ace.set_color(e, r, g, b)` | MeshRenderer color (sRGB 0..1) |
| `ace.set_sprite(e, frame [, fps [, flip_x]])` | SpriteRenderer sheet frame; `fps > 0` loops the sheet; `flip_x` mirrors it |

Passing a destroyed or invalid entity id raises a Lua error.

Scripts run sandboxed: only `base`, `table`, `string`, `math` and `utf8` are
available (no `io`, `os`, `require`, `load`).

To add a function: write an `l_name` C function in `engine/script/script.cpp`,
add it to the `api` table, and document it here.
