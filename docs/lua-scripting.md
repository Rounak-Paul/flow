# Lua behavior assets

Lua source is an ordinary project asset. Import a `.lua` file or create one in Scripts, then attach its asset to an entity as a Script component. Multiple instances can share one asset and keep independent parameters and clocks. Each component has a stable instance ID. Scripts opens the editable project asset file under `assets/scripts/`; saved external edits are validated before the editor publishes a replacement document.

## Module contract

```lua
return {
    parameters = {
        distance = { type = "number", default = 40, min = 0, max = 1000 },
    },
    outputs = {self = {x = "replace"}},
    sample = function(ctx)
        ctx.entity.transform.x = ctx.entity.transform.x
            + math.sin(ctx.time.seconds * 2) * ctx.params.distance
    end,
}
```

An asset returns one module table with optional `build(ctx)`, `initialize(ctx)`, `step(ctx)` and `sample(ctx)` capabilities. Every callback gets a fresh module environment. Only explicit `ctx.state` survives simulation steps; globals and captured locals reset. A simple motion effect only needs `sample`.

`initialize` creates explicit state at local time zero. `step` advances it at a fixed local interval: `step_hz` defaults to 120 and must be an integer from 1 to 240 that divides 120000. `sample` receives a copy of the state at the preceding fixed step and emits the frame. Changing that copy does not advance or alter committed simulation state. Use `ctx.time.alpha` for optional interpolation.

See [spring.lua](../examples/scripts/spring.lua) for a reusable simulation. See [the runtime contract](LUA_RUNTIME.md) for structural build, asset imports, Flow prototypes, writer layers, generated-child editing, simulation events and native baking.

Syntax, attached module declarations and parameter values are validated before a GUI/MCP changeset or watched file candidate publishes. A failure preserves the previous accepted source and document. Source and matching parameter edits can be grouped in one atomic changeset. Runtime failures inside callbacks remain evaluated diagnostics.

Evaluation starts with authored values and keyframes. Enabled Script instances publish atomic target patches through declared writer layers. Same-layer inputs are coherent; ambiguous writers and dependency cycles fail validation. A failure preserves the affected layer inputs and blocks affected downstream writers. Scripts do not rewrite authored components or keyframes.

## Context

- `ctx.entity.transform`: `x`, `y`, `width`, `height`, `rotation` (radians), `scale_x`, `scale_y`.
- `ctx.entity.appearance`: `fill`, `stroke` (RGBA arrays, 0–255), `stroke_width`, `font_size`.
- `ctx.entity.crop`: `x`, `y`, `width`, `height` as source fractions.
- `ctx.entity.style`: radius, blur, gradient_angle, gradient_to, shadow_blur, shadow_x/y and shadow_color.
- `ctx.entity.media.seconds`: video source seconds; -1 follows the native clip placement.
- `ctx.events`: deterministic named JSON input events for initialize/step; sample receives an empty list.
- `ctx.entity.opacity`: 0–1.
- `ctx.entity.text`: text content; sampled output uses variable-length storage within a 16 MiB aggregate frame budget. Invalid UTF-8 or embedded NUL rejects the output; text is never silently truncated.
- `ctx.time`: local `tick`, `seconds`, `duration`, `progress`, `cycle`, `phase`, and `composition_seconds`. Duration is local seconds; progress is normalized local position. `dt` is the fixed simulation interval, `step` the completed step index, and `alpha` the fraction towards the next step.
- `ctx.state`: explicit simulation values; integers, finite floats, booleans, binary strings, dense arrays and string-keyed objects. Functions, handles, sparse/mixed tables and cycles are rejected.
- `ctx.params`: declared defaults merged with this component's explicit parameter values.

Parameter declarations support `number`, `integer`, `boolean`, `string`, `vec2` and `color`, with optional `label` and `description`. Numeric declarations can constrain `min` and `max`. Unknown parameter names and invalid values produce diagnostics. Unchanged defaults remain asset-owned; editing a parameter in Inspector writes an instance override.

Inspector configures whole-composition or beat-bound intervals, stretch/fixed speed, canonical duration, rate, looping, and before/after hold. Beat references follow reordering and resizing. Splitting a beat preserves the original bound interval. A missing beat disables sampling with a diagnostic. Use `component.set` through MCP to edit the same typed clock/parameter fields found in scene JSON.

## Runtime boundaries

Bundled Lua 5.4 supplies selected base functions and math/string/table/utf8 libraries. `pairs` uses stable numeric/string/boolean key order. Callback random streams are deterministic per entity/component and fixed step/cycle. Filesystem, network, processes, wall clock, dynamic modules, debug, coroutine and protected-call APIs are unavailable.

Scripted editor previews run on isolated document views in worker jobs, publishing only complete samples. A pending indicator distinguishes catch-up from a completed frame. Starting a transform gesture requests exact values.

Each evaluator has a 16 MiB Lua allocation budget. Each module/callback invocation has a one-million-instruction budget. Explicit state is limited to 64 KiB, 4096 values and 16 nested levels per instance; checkpoint storage is bounded to 16 MiB per evaluator, with an initial checkpoint and a rolling set of one-second checkpoints. Catch-up is cancellable and limited to one million replay steps per request. Native inputs are sampled at each step’s actual composition time. Source, parameters and scene-input edits invalidate the evaluator; cycle changes reset canonical simulation state. Nonfinite or out-of-range outputs reject the sample. Errors are exposed by the Inspector and `evaluate_scene`.
