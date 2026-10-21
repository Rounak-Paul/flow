# Lua behavior runtime

Lua files are ordinary project assets. A repeatable Script component references one asset and owns its parameters, clock and simulation state. One returned table provides any combination of `build`, `initialize`, `step` and `sample` capabilities. Helper assets imported by another module can export ordinary functions without lifecycle callbacks.

```lua
return {
    parameters = {distance = {type = "number", default = 100}},
    outputs = {self = {x = "replace"}},
    step_hz = 120,
    initialize = function(ctx)
        ctx.state = {position = 0}
    end,
    step = function(ctx)
        ctx.state.position = ctx.state.position + ctx.params.distance * ctx.time.dt
    end,
    sample = function(ctx)
        ctx.entity.transform.x = ctx.state.position
    end,
}
```

Only explicit `ctx.state` survives fixed steps. Module tables, globals and closure state are reconstructed for each invocation. Sampling receives a state copy and cannot advance committed simulation state. Clocks expose local ticks/seconds, duration, progress, cycle, phase, fixed-step delta and interpolation alpha. Moving or retiming an attached clock changes evaluation without rewriting source.

## Asset imports

Declare dependencies with `imports = {alias = "exact Lua asset ID"}`. Asset IDs remain stable when names or paths change. Imported modules are available through `ctx.imports.alias` in callbacks and `build`. Missing resources, incompatible kinds and import cycles reject the candidate document. Dependency edits invalidate the affected executable generation through the shared document revision boundary.

```lua
-- Helper asset 123
local count = 0
return {
    next = function()
        count = count + 1
        return count
    end,
}

-- Attached behavior
return {
    imports = {first = "123", second = "123"},
    sample = function(ctx)
        ctx.entity.transform.x = ctx.imports.first.next() + ctx.imports.second.next()
    end,
}
```

Both aliases share one helper instance within this invocation, producing 3 every sample. A later callback gets fresh helper tables and closure state. Imports can have their own declared imports; their returned `imports` field is resolved before their exported functions are called. Imports cannot access arbitrary files, processes, networks or ambient time.

## Structural build

`build(ctx)` runs during document compilation, rather than on every frame. It receives effective parameters and canonical authored inputs at tick zero. Return a typed version-one Flow prototype descriptor. The engine validates the same entity/component/beat/clip schema used by `.flproto.json` assets, then expands it under the Script owner with independent scoped clocks and stable generated identities. It does not create an authored asset or rewrite the document.

```lua
return {
    parameters = {width = {type = "number", default = 200, min = 1}},
    build = function(ctx)
        return {
            version = 1, root = 1, next_id = 2, next_component_id = 3,
            duration = 120000, beats = {}, clips = {},
            entities = {{
                id = 1, parent = 0, name = "Panel", visible = true,
                components = {
                    {id = 1, type = "transform", version = 1,
                     x = 0, y = 0, width = ctx.params.width, height = 100,
                     rotation = 0, scale_x = 1, scale_y = 1},
                    {id = 2, type = "rectangle", version = 1, color = {80, 120, 255, 255}},
                },
            }},
        }
    end,
}
```

Use stable local IDs across builds. Both next-ID cursors must exceed the corresponding IDs; timeline IDs share the entity cursor. Empty native collection tables become arrays. Generated native components can include animation, scripts, nested prototypes and media. Recursion, disconnected hierarchies, duplicate identities, unsupported payloads and invalid resources reject publication. Generated resource dependencies are included in captured exports.

Generated objects are independently selectable in the viewport and the Inspector Contents selector. Inspector edits and gestures persist overrides against local identities. Their timeline shows the local scope's beats and keys; seeking maps through parent clocks using the current loop cycle. A behavior-owned channel is read-only in property/gesture controls: edit its parameters, bindings or source instead. Native keyed fields retain their authored base and write at the local playhead.

Detach converts a Flow prototype instance to an owned composition, preserving the generator identity, local timelines, behaviors and media clocks. Nested assets remain ordinary references. Bake explicitly samples the whole scene into native sampled-animation components and owned nested scopes; it does not generate another Lua script or freeze a single frame.

## Typed attachment inputs

A module declares named inputs independently of project IDs:

```lua
return {
    bindings = {
        target = {type = "entity"},
        image = {type = "asset", optional = true},
    },
    sample = function(ctx)
        ctx.entity.transform.x = ctx.bindings.target.transform.x + 20
    end,
}
```

The Script component's `bindings` object supplies typed references, for example `{"target":{"type":"entity","id":"42"}}`. Script schema version 8 stores bindings independently of parameters. Entity/component IDs are scoped automatically inside each Flow prototype instance; regions use that scope's beat IDs and assets remain project resources. Component bindings require both `entity` and `id`. Region/asset bindings require `id`. At most 64 named bindings and 16 KiB of reference JSON are allowed. Names are nonempty and at most 63 UTF-8 bytes. Required declarations need wiring; optional declarations may be omitted. Deleted targets keep their reference and produce an unresolved runtime diagnostic.

`ctx.bindings` contains independent native snapshots. Entity inputs expose the same property vocabulary as `ctx.entity`, sampled from authored components and animation at the actual callback composition tick. Fixed-step replay therefore reads its own step's inputs. Component inputs expose typed authored component JSON, bounded to 32 KiB of native payload and 512 animation keys per input. Region inputs expose `start_tick` and `end_tick`; asset inputs expose identity, name and kind. Mutating these Lua tables cannot change another entity or the document. Asset bindings participate in dependency retention and export capture, including bindings retained by prototype overrides.

These are native reads. Evaluated dependencies use `reads`, `ctx.inputs`, `outputs` and explicit attachment layers:

```lua
return {
    bindings = {target = {type = "entity"}},
    reads = {target = {x = true}},
    outputs = {self = {x = "replace"}, target = {y = "add"}},
    sample = function(ctx)
        ctx.entity.transform.x = ctx.inputs.target.transform.x + 20
        ctx.outputs.target.transform.y = 10
    end,
}
```

One layer observes a coherent input snapshot. Lower layers publish before higher layers. Reads of another behavior require a strictly lower writer layer. Ambiguous same-layer writers and dependency cycles reject publication. Numeric outputs support replace/add/multiply; text supports replacement. Every target of a callback validates before any target publishes. Failed dependencies block affected downstream outputs while unaffected peers continue.

Omitting `outputs` gives a sampling behavior replacement ownership of all self channels. Declare narrow outputs to leave other channels available for native authoring. Build/state-only modules own no sampled channels. The Inspector exposes layer and typed entity/component/beat/asset bindings; MCP evaluation reports writer provenance and runtime/local component addresses.

## Native properties and simulation events

The shared property registry includes transform, opacity, fill/stroke, font size, crop, radius, blur, gradient angle/colour, shadow blur/offset/colour and video source time. Native tracks, Lua outputs, baking and rendering use the same vocabulary. `ctx.entity.style` carries the style properties; `ctx.entity.media.seconds` overrides video source seconds (`-1` follows the native placement clock). Angles use radians in native documents/Lua and the generic evaluated properties map. Text remains a budgeted UTF-8 string.

An optional `events` component contains `{next_id, points}`. Each point has a stable component-local `id`, a native beat-aware `at` anchor, a `name` and JSON `value`. The Inspector's Simulation events controls add a named payload at the local playhead and remove points through the normal Apply/Revert draft. Files and MCP `component.set` edit the same native data. At most 256 events and 64 KiB of event JSON belong to one entity.

Initialize receives events at its source origin. Each fixed step receives `ctx.events` in `(previous, current]`, sorted by source tick then stable ID. Each event exposes `id`, `name`, `value` and source `tick`. Sampling receives an empty event list, preventing presentation callbacks from firing simulation events twice. Replay, checkpoints, loop scopes and backward seeks reproduce the same explicit state; beat split preserves resolved event times. Deleted beat anchors remain inactive until rebound or restored.

## Explicit native bake

The Inspector's collapsed Bake scene motion controls and MCP `bake_scene` share one engine job owner. Start with an explicit interval, maximum sample step and nonnegative numeric tolerance; poll completion, then apply. Capture uses immutable project documents and cancellable workers. Apply rejects a changed project and creates one shared undo entry. Scope media/audio clocks remain native.

Reduction verifies every captured interior point against native linear interpolation, holding text between keys. Tolerance applies at captured ticks; maximum step defines the unsampled interval. Endpoints hold outside the captured outer interval. Nested scopes capture their complete canonical intervals to preserve loops. Unresolved structural overrides reject bake. Capture is bounded to one million total poses, 64 MiB per scope and bounded native documents. Ready data is deliberately authored, cache-independent native data.

Future specialized physics, transient particle/path batches and new effects extend these boundaries; they are not additional Script types.

## Budgets

Callbacks use a 16 MiB Lua VM and one million Lua instructions per invocation, with cooperative cancellation. Explicit state and sampled text are bounded separately. Imports allow 128 compiled helpers and depth 32. Build document capture allows depth 32, 200,000 nodes and 16 MiB of native capture storage. Composition expansion allows 100,000 entities, 4,096 scopes, depth 32 and 128 MiB of cumulative expanded source. Budget failures produce diagnostics and preserve the last accepted document.
