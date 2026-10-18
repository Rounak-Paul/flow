# Lua entity scripts

Attach a `.lua` file from **Inspector → Lua Script → Attach Lua script**, then press **Apply**. The source is embedded in the project; moving the original file does not break it. Replace source imports a revised file. Enabled and Remove script control the component. Transform is required and appears first in each entity's hierarchy; Animation (keyframes) and Lua Script are optional.

## Evaluation

```lua
function on_evaluate(self, time)
    self.transform.x = self.transform.x + math.sin(time * 2) * 40
end
```

`on_evaluate(self, time)` is the only entry point; `time` is document seconds. Every sample runs in a fresh environment, so the result depends only on the document and the time. Nothing persists between samples, and seeking, playback, presenting and export all produce the same values. Global variables, `self` fields and the random stream reset on each call.

Evaluation order per entity: authored base values → keyframe tracks → script. `self` starts from the keyframed values, so a script can offset an animated entity or replace a channel outright. Scripts never rewrite keyframes or authored components. Editing the scene rebuilds the evaluator. Save stores source and enabled state.

## Script environment

- `self.transform`: x, y, width, height, rotation (radians), scale_x, scale_y.
- `self.appearance`: fill/stroke (four-element RGBA arrays, 0–255), stroke_width, font_size.
- `self.opacity`: 0–1.
- `self.text`: on text entities, the content. Assign a new string (up to 255 bytes) to change what is drawn, for example a counting number; the stored text is never modified.
- `self.tick`: integer document time, 120000 ticks/second; `self.duration`: scene length in seconds.
- `self.entity`: stable hexadecimal entity ID.

Nonfinite or out-of-range values reject the evaluation without partially changing the frame, and the entity keeps its keyframed values. Errors appear in the Inspector and Console.

## Runtime boundaries

Lua 5.4.7 is bundled. Each script has an isolated environment. Selected base functions and math/string/table/utf8 libraries are available. `pairs` uses stable numeric/string/boolean key order. Randomness is seeded per entity; randomseed requires an explicit seed. Filesystem, network, processes, wall clock, dynamic modules, debug, coroutine and protected-call APIs are not exposed.

Each scene evaluator has a 16 MiB Lua allocation budget and each evaluation has a one-million-instruction budget.

Cross-entity queries and typed component parameters belong to the planned semantic component packs, not to entity scripts.
