# Flow MCP server

The running editor is an MCP server. An agent edits the project that is open in Flow and the user sees every change as it happens. Nothing is written to project files directly: every change is a typed document operation, grouped into atomic, undoable changesets.

## Connect

Open a project in Flow. The editor listens on `http://127.0.0.1:7788/mcp` (another free port if that one is taken) and writes the port and bearer token to `mcp.json` in its config directory:

| OS | File |
|---|---|
| macOS | `~/Library/Application Support/Flow/config/mcp.json` |
| Windows | `%APPDATA%\Flow\config\mcp.json` |
| Linux | `~/.config/flow/mcp.json` |

The file is readable by you only and keeps the same token across restarts. Claude Code:

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
| `evaluate_scene` | Resolved values of every entity at a time, after keyframes and Lua, plus script errors |
| `apply_ops` | Atomic batch of operations. Created objects get `"as": "$name"` and can be referenced by later operations. `expect` refuses the batch if a scene changed since it was read; `dry_run` validates without applying |
| `undo` / `redo` | Revert or repeat the newest changeset; refused (`conflict`) if anything else edited the project since |
| `render_frame` | Renders a scene at one time or up to six (`times`) offscreen on the GPU and returns PNG images at any width (default 1024, up to 8.8 megapixels). Independent of the editor window: no grid or selection, nothing on screen changes |
| `export_video` | Renders the project's scenes in order to an H.264 MP4 (BT.709, yuv420p) through ffmpeg: `path` (a file name goes to the project's `exports` folder), `width` (default 1920, height follows the aspect, both even), `fps` (default 60), `crf` (default 16), optional `scenes`. Runs in the background on the same renderer as `render_frame` |
| `get_export_status` / `cancel_export` | Progress (state, frames done/total, elapsed, output path) of the latest export; stop it |
| `get_editor_state` / `set_editor_state` | Read or set the shown scene, playhead and selection |
| `import_asset` | Copy a local image, audio, video or font file (absolute path) into the project; not part of undo |
| `save_project` | Write now; Flow also autosaves after changes |

Look properties beyond fill and stroke: `radius`, a two-colour gradient (`fill2`, `gradient_angle`), a soft drop shadow (`shadow_blur`, `shadow_x`, `shadow_y`, `shadow_color`), text `align`, a `crop` window and animatable `crop_x/y/w/h` for images and videos (zoom and pan inside a fixed frame), and scene `background`. Video entities (`kind: "video"`, asset from `import_asset`) play from `start` at source `offset` and `rate`; import extracts the frames once with ffmpeg into the project's cache. Lua scripts can replace a text entity's content through `self.text`, for example to count up a number.

Units: times in seconds, rotation in degrees, colours `#RRGGBB[AA]`, positions in scene pixels. Operations: `scene.add/remove/rename/move/set`, `entity.create/set/remove/reparent/reorder`, `key.set/remove`, `track.remove`, `beat.add/update/remove/move/split`, `clip.add/update/remove`, `script.set/remove`.

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
- History keeps up to 64 changesets. After any edit made outside the history (in the GUI, an asset import, a rename), earlier entries can no longer be undone and are dropped when the next changeset starts.
- Calls run on the editor's UI thread and time out after 30 s. Closing the project fails in-flight calls cleanly.
- Renders come from the same GPU scene renderer the editor viewport uses (`src/editor/fl_scene_renderer.c`), drawn into an offscreen image, so shapes, blending, text and images match what you see. Text is rasterized at the output resolution, so large renders are sharper than the viewport.
- Video needs ffmpeg (on PATH, in a common install folder, or named by `FLOW_FFMPEG`); export and video import fail with a clear error without it. Audio is not exported yet.
- Scripts are `on_evaluate(self, time)` only; see `docs/lua-scripting.md`.

## Internals

`src/engine/agent/`: `fl_mcp` (JSON-RPC and tool registry, hand-off to the owner thread), `fl_mcp_http` (CivetWeb transport), `fl_agent*` (document tools and the operation executor), `fl_history` (changeset undo/redo over scene snapshots, `fl_project_snapshot/restore`). `src/editor/fl_scene_renderer` is the GPU renderer shared by the viewport and `render_frame`. `src/editor/fl_mcp_service.c` wires the endpoint to the editor and implements the editor-state tools. Vendored: `vendors/civetweb` (MIT), `vendors/cjson` (MIT).
