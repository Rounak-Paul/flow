# Flow MCP server

The running editor is an MCP server. An agent edits the project that is open in Flow and the user sees every change as it happens. MCP edits use typed, atomic, undoable document operations. Agents can also edit readable project JSON, Lua sources and Flow prototype asset files; the editor watches and validates these changes before publication. Unseen external edits block conflicting saves.

## Connect

Open a project in Flow. The editor listens on `http://127.0.0.1:7788/mcp` (another free port if that one is taken) and writes the port and bearer token to `mcp.json` in its config directory:

| OS | File |
|---|---|
| macOS | `~/Library/Application Support/Flow/config/mcp.json` |
| Windows | `%APPDATA%\Flow\config\mcp.json` |
| Linux | `~/.config/flow/mcp.json` |

The file is readable by you only and keeps the same token across restarts. Creating or opening a project copies that same file to `<project>/.flow/mcp.json` and refreshes the `flow` server in `<project>/.cursor/mcp.json` (url plus `Authorization: Bearer <token>`), leaving any other servers in that file alone. Both copies are rewritten every time the project is opened, so an agent whose workspace is the project folder sees the live endpoint.

Claude Code:

```sh
claude mcp add --transport http flow http://127.0.0.1:7788/mcp \
  --header "Authorization: Bearer $(jq -r .token "$HOME/Library/Application Support/Flow/config/mcp.json")"
```

Any client that supports MCP Streamable HTTP with a custom `Authorization` header works the same way. The endpoint accepts only loopback `Host`/`Origin` headers and POST requests; it never serves files.

Tools need an open project. With the project launcher showing they return `no_project`.

## Tools

| Tool | What it does |
|---|---|
| `get_reference` | The manual: units, every operation and its fields, the animation model, Lua scripting. Agents should read it first. |
| `get_project` | Scenes (id, revision, size, length), assets, undo history |
| `get_scene` | One scene in full: beats, audio clips, entities in draw order with properties, keyframe tracks and script info |
| `get_asset` | Read asset metadata and accepted source bytes, including unsaved Lua or Flow prototype edits |
| `get_script` / `set_script` | Read, create or edit a project Lua asset by ID or exact name; optional attachment to entity IDs |
| `evaluate_scene` | Resolved values of every entity at a time, after keyframes and Lua, plus script errors |
| `apply_ops` | Atomic batch of operations. Created objects get `"as": "$name"` and can be referenced by later operations. `expect` refuses the batch if a scene changed since it was read; `dry_run` validates without applying |
| `undo` / `redo` | Revert or repeat the newest changeset; shared with GUI edits |
| `render_frame` | Renders a scene at one time or up to six (`times`) offscreen on the GPU and returns PNG images at any width (default 1024, up to 8.8 megapixels). Independent of the editor window: no grid or selection, nothing on screen changes |
| `export_video` | Renders the project's scenes in order to an H.264 MP4 (BT.709, yuv420p) through ffmpeg: `path` (a file name goes to the project's `exports` folder), `width` (default 1920, height follows the aspect, both even), `fps` (default 60), `crf` (default 16), optional `scenes`, optional `crossfade` seconds (scenes overlap and dissolve into each other in linear light). Audio clips of the exported scenes are mixed (gain, fades, trimmed to their scene) into AAC. Runs in the background on the same renderer as `render_frame` |
| `get_export_status` / `cancel_export` | Progress of the latest export: state, frames done/total, elapsed, preparation/render seconds, summed GPU fence wait seconds, average frames per second after startup, and output path; stop it |
| `get_editor_state` / `set_editor_state` | Read or set the shown scene, playhead and selection |
| `import_asset` | Copy a local image, audio, video, font or Lua file (absolute path) into the project through shared undo history |
| `save_project` | Write now; Flow also autosaves after changes |

Look properties beyond fill and stroke: `radius`, a two-colour gradient (`fill2`, `gradient_angle`), a soft drop shadow (`shadow_blur`, `shadow_x`, `shadow_y`, `shadow_color`), text `align`, a `crop` window and animatable `crop_x/y/w/h` for images and videos (zoom and pan inside a fixed frame), and scene `background`. Video entities (`kind: "video"`, asset from `import_asset`) play from `start` at source `offset` and `rate`; the decoder uses a bounded timestamp-based ffmpeg frame cache. Lua scripts can replace a text entity's content through `ctx.entity.text`, for example to count up a number.

Units: times in seconds, rotation in degrees, colours `#RRGGBB[AA]`, positions in scene pixels. Operations: `scene.add/remove/rename/move/set`, `entity.create/set/remove/reparent/reorder`, `key.set/remove`, `track.remove`, `beat.add/update/remove/move/split`, `clip.add/update/remove`, `script.create/update/remove/attach/detach`, `component.set/remove`.

```json
{"label": "Intro", "ops": [
  {"op": "entity.create", "scene": 1, "kind": "ellipse", "as": "$dot",
   "props": {"name": "Dot", "x": 100, "y": 300, "width": 200, "height": 200, "fill": "#ff6b35"}},
  {"op": "key.set", "entity": "$dot", "property": "x", "at": 0, "value": 100},
  {"op": "key.set", "entity": "$dot", "property": "x", "at": 4, "value": 1200, "ease": "in_out"}
]}
```

## Behaviour to know

- A failing operation applies nothing and reports `op_index`, a stable `code` and a message naming the problem. Unknown fields are errors, not ignored.
- GUI, MCP and accepted file reloads share up to 64 changesets. Component instances keep stable IDs across undo/redo.
- Calls run on the editor's UI thread and time out after 30 s. Closing the project fails in-flight calls cleanly.
- Renders come from the same GPU scene renderer the editor viewport uses (`src/engine/render/gpu/fl_scene_renderer.c`), drawn into an offscreen image, so shapes, blending, text and images match what you see. Text is rasterized at the output resolution, so large renders are sharper than the viewport.
- Video needs ffmpeg (on PATH, in a common install folder, or named by `FLOW_FFMPEG`); export and video import fail with a clear error without it.
- Lua assets return behavior modules with `sample(ctx)` and typed parameters; see `docs/lua-scripting.md`. `get_script` and `set_script` address project assets without a scene argument.

## Internals

`src/engine/agent/`: `fl_mcp` (JSON-RPC and tool registry, hand-off to the owner thread), `fl_mcp_http` (CivetWeb transport), `fl_agent*` (document tools and the operation executor), project-owned `project/fl_history` (shared changesets over document snapshots). `src/engine/render/gpu/fl_scene_renderer` is the GPU renderer shared by the viewport and `render_frame`. `src/editor/fl_mcp_service.c` wires the endpoint to the editor and implements the editor-state tools. Vendored: `vendors/civetweb` (MIT), `vendors/cjson` (MIT).

## Editable source assets

`apply_ops` accepts `asset.source {asset?, kind: "lua" | "flproto", name?, source?}`. Creating requires name and source; updating requires the matching kind and preserves omitted name/source. The returned ID supports the same batch aliases as entities. `asset.rename {asset, name}` and `asset.remove {asset}` operate on ordinary assets. Deleting a referenced asset is rejected; detach or remove its references explicitly in the same batch first. Script convenience operations use these same engine owners.

`get_asset {asset}` returns metadata, a project-relative source path and current accepted source bytes. Flow prototype sources live at `assets/prototypes/<id>.flproto.json`. They contain a rooted native component hierarchy, local identity cursors, canonical duration, beats and clips. Referenced Lua/media asset IDs are project-wide. Source edits, watcher reload, imports and history validate resource references and enabled behavior declarations before accepting the candidate. Live prototype instances and overrides remain under implementation.

## Scoped content and native bake

`evaluate_scene` includes generated origins (`owner`, root generator `component`, local `entity`, nested `path` and runtime/local component mappings), `writers` per channel and a complete numeric `properties` map in native units. Lua and native style/media output match the engine renderer.

`scope.edit` in `apply_ops` addresses `{entity: persistentOwner, component: rootGenerator, target: localEntity, path?: localGeneratorIds, value: editOrArray}`. Edits support entity.add/patch/remove, component.set/patch/remove and timeline.patch (duration/beats/clips). A component.set ID of zero allocates a reserved extension identity. Retained unresolved operations keep their resource dependencies. Missing targets remain unresolved rather than rebinding to another object.

`prototype.detach {entity}` materializes an owned native scope with its generator identity, clocks, behaviors and media intact. Lua assets remain ordinary catalog resources. Script schema8 stores `layer`, native `bindings` and structural `overrides` alongside parameters and time.

`bake_scene` takes `action: start` with scene, start/end/step in seconds and numeric tolerance. It returns a token and running/ready/failed/cancelled/applied status. Use action status/cancel/apply with that token. Application is an atomic undoable edit and rejects intervening document changes. Work never mutates the live document from a worker. See LUA_RUNTIME.md for capture accuracy and budgets.

Native `events` component1 contains events={next_id,points:[{id,at,name,value}]}; anchors use the regular document anchor schema. Fixed-step behaviors consume deterministic ctx.events. Inspector, files and MCP configure the same component.
