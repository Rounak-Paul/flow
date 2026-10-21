# Lua behavior runtime

Lua files are ordinary project assets. A repeatable Script component references one asset and owns its parameters, clock and simulation state. One returned table provides any combination of `build`, `initialize`, `step` and `sample` capabilities. Helper assets imported by another module can export ordinary functions without lifecycle callbacks.

```lua
return {
    parameters = {distance = {type = "number", default = 100}},
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

The native viewport currently selects the persistent generator owner when a generated child is picked. Generated-child override controls, explicit detach/bake and evaluated-output writer scheduling are separate unfinished refactor work; do not infer their availability from structural rendering.

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

The Script component's `bindings` object supplies typed references, for example `{"target":{"type":"entity","id":"42"}}`. Script schema version 6 stores bindings independently of parameters. Entity/component IDs are scoped automatically inside each Flow prototype instance; regions use that scope's beat IDs and assets remain project resources. Component bindings require both `entity` and `id`. Region/asset bindings require `id`. At most 64 named bindings and 16 KiB of reference JSON are allowed. Names are nonempty and at most 63 UTF-8 bytes. Required declarations need wiring; optional declarations may be omitted. Deleted targets keep their reference and produce an unresolved runtime diagnostic.

`ctx.bindings` contains independent native snapshots. Entity inputs expose the same property vocabulary as `ctx.entity`, sampled from authored components and animation at the actual callback composition tick. Fixed-step replay therefore reads its own step's inputs. Component inputs expose typed authored component JSON, bounded to 32 KiB of native payload and 512 animation keys per input. Region inputs expose `start_tick` and `end_tick`; asset inputs expose identity, name and kind. Mutating these Lua tables cannot change another entity or the document. Asset bindings participate in dependency retention and export capture, including bindings retained by prototype overrides.

These are explicitly native reads. Reading another behavior's evaluated output requires the unfinished declared writer/dependency plan; no insertion-order dependency is implied.

## Budgets

Callbacks use a 16 MiB Lua VM and one million Lua instructions per invocation, with cooperative cancellation. Explicit state and sampled text are bounded separately. Imports allow 128 compiled helpers and depth 32. Build document capture allows depth 32, 200,000 nodes and 16 MiB of native capture storage. Composition expansion allows 100,000 entities, 4,096 scopes, depth 32 and 128 MiB of cumulative expanded source. Budget failures produce diagnostics and preserve the last accepted document.
