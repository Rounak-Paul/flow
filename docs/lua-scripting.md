# Lua entity behaviors and animation

Attach a `.lua` file from **Inspector → Lua Script → Attach Lua script**, then press **Apply**. The source is embedded in the project; moving the original file does not break it. Replace source imports a revised file. Enabled and Remove script control the component. Transform is required and appears first in each entity's hierarchy; Animation and Lua Script are optional.

## Behavior lifecycle

```lua
function on_init(self)
    self.speed = 120
    self.animation:play("Idle")
end

function on_update(self, dt, time)
    self.transform.x = self.transform.x + self.speed * dt
    self.animation:parameter("speed", self.speed)
end

function on_destroy(self)
end
```

`on_init(self)` initializes private per-entity state. Press Play to advance `on_update(self, dt, time)` at fixed 60 Hz; both arguments use seconds. Custom fields persist. Pause retains state. Backward seeks recreate and replay behavior from time zero; restarting resets it. Simulation values hold between steps. Editing the scene rebuilds evaluated runtimes at the playhead. Save stores source and configuration; runtime state reconstructs on reopen.

Optional `on_destroy` runs when an initialized behavior runtime is released, including scene revisions. Internal rewind resets do not invoke it. Animation controls are unavailable during teardown because document definitions may already have changed.

A script may instead define `on_evaluate(self, time)` for exact-time procedural motion. That environment resets for each sample, optionally running on_init first. Define either on_update or on_evaluate, not both. Stateful graph control belongs in on_update; direct evaluation avoids replay for motion that has a closed-form expression.

## Animation component

Animation owns keyframed states and a transition graph. Scripts control its player rather than modifying keyframes while playing.

- The built-in **Timeline** state retains scene/beat-anchored animation.
- Named states contain their own property tracks, local duration and loop flag. Their keys use absolute local ticks.
- The graph has an entry state, number/bool/trigger parameters and ordered transition edges.
- Edges support always, greater/less/equal comparisons or a fired trigger; optional exit-time gating and crossfade duration.
- The first matching edge is taken per simulation step. A taken trigger is consumed. Entry defaults and graph transitions also work without a script.

| Lua call | Behavior |
|---|---|
| `self.animation:play("Run", 0.2)` | Select Run with a 0.2-second blend; selecting the current state does not restart it |
| `self.animation:parameter("speed", 2)` | Set a number parameter; bool parameters accept true/false |
| `self.animation:parameter("speed")` | Read the current parameter |
| `self.animation:trigger("jump")` | Fire a trigger until a transition consumes it |
| `self.animation:state()` | Read the active state name |
| `self.animation:pause()` / `:resume()` | Control animation independently of the entity behavior |

Behavior calculates the procedural base. The sampled animation pose composes on top relative to the authored bind values: positional/rotation/size deltas add; scale and opacity multiply relative to their nonzero bind values; color/style deltas apply with property bounds. This lets a behavior move an entity while its animation plays local motion. Keyframes and scripts may affect the same channel through these separate layers; there is no output mask or competing-driver rejection. Inspector fields edit authored base values when a script is enabled.

Graph definitions and local keys are editable through the typed project operations: `FL_OP_ANIMATION_STATE_SET/REMOVE`, `FL_OP_ANIMATION_ENTRY`, `FL_OP_ANIMATION_PARAMETER_SET/REMOVE`, `FL_OP_ANIMATION_TRANSITION_SET/REMOVE`, and key/track operations with `animation_state`. They validate and commit atomically. State removal prunes its edges; parameter removal prunes dependent edges. The Inspector summarizes states/entry/transitions. A dedicated graph editor and named-state timeline UI are not implemented yet.

## Script state

- `self.transform`: x, y, width, height, rotation (radians), scale_x, scale_y.
- `self.appearance`: fill/stroke (four-element RGBA arrays, 0–255), stroke_width, font_size.
- `self.opacity`: 0–1.
- `self.tick`: integer document time, 120000 ticks/second; `self.duration`: scene length in seconds.
- `self.entity`: stable hexadecimal entity ID.
- `self.animation`: the owning entity's player controls; requires an Animation component.

Runtime values do not rewrite authored components or keyframes. Nonfinite/invalid visual values reject evaluation without partially changing the frame. Errors appear in the Inspector and Console.

## Runtime boundaries

Lua 5.4.7 is bundled. Each script has an isolated environment. Selected base functions and math/string/table/utf8 libraries are available. `pairs` uses stable numeric/string/boolean key order. Randomness is seeded per entity; randomseed requires an explicit seed. Filesystem, network, processes, wall clock, dynamic modules, debug, coroutine and protected-call APIs are not exposed.

Each scene evaluator has a 16 MiB Lua allocation budget; each script evaluation/replay and teardown has a one-million-instruction budget. Behavior failures stop that instance until the scene revision changes. Very long/expensive backward seeks can exceed replay budgets; use direct evaluation for motion that does not need simulation. Graph-only replay also has a bounded step count.

Input, collision/physics, cross-entity queries, spawning and general engine event bindings remain separate future systems. This implementation provides entity behavior and animation-state control, not an entire 2D engine.
