# Flow — Design Document

> **Codename:** Flow (working name only; not cleared for use)
> **Publisher:** V4Studio (proposed)
> **Status:** Design v0.1 — pre-implementation
> **First dogfood:** the *vkcompute* chapter video series

---

## 0. TL;DR

Flow is a **local-first, AI-native visual explanation engine**, shipped as a single portable native executable (C11 on causality + Vulkan) for macOS, Windows and Linux. You make one project and use it three ways: present it live, publish it as an interactive web page, or render it to video, all with frame-accurate output.

The whole product rests on one rule: **the document is data, and every change is an operation.** The visual editor, AI agents, the CLI and scripted components all read and write the same scene graph through the same typed operation log. Nobody rewrites files. Nobody regenerates the scene from scratch.

Five ideas together make Flow different from slide tools, code-driven animation libraries and video generators:

1. **Point-and-Ask:** select anything (an entity, a sub-part of a component, a time range, a word in the script, an empty region of the canvas) and give the AI an instruction scoped to it. **The selection is both the agent's context and its permission boundary.**
2. **Provenance-aware editing:** every property knows who last authored it (user, agent or theme). Agents cannot silently overwrite human edits. Their changes arrive as reviewable **changesets** with ghost previews, partial accept and automatic rebase.
3. **Semantic entities:** a `Buffer` is not a set of rectangles. It is a typed entity with addressable parts (`buffer.cells[3]`), so both people and agents can talk about and animate *meaning* instead of coordinates.
4. **Script-anchored beats:** narration is the timeline's backbone. Animations anchor to beats and to individual spoken words. Retime the script and the motion follows. The same beats drive click-to-advance presenting and auto-timed video export.
5. **Agent verify loop:** agents can *see* their work. They render frames and receive lint reports (overlap, overflow, contrast, timing collisions) before a human ever reviews the change.

One line: **Point at anything, say what you want, keep every edit you made.**

---

## 1. Problem

| Tool class | Good at | Breaks down when |
|---|---|---|
| PowerPoint / Keynote / Canva / Gamma | Accessible layout, presenting | Real motion, programmatic visuals, technical diagrams |
| Remotion / Motion Canvas / Manim | Expressive, agent-friendly code | Visual tweaking; non-programmers can't edit; small changes need code |
| After Effects / Rive / Jitter | Direct-manipulation motion | Agents can't drive them; no programmable logic; no presenting |
| AI video generators | Speed from a prompt | Output is flat pixels; nothing can be revised at the object level |
| "Ask an LLM for an HTML page" | Free, fast | Every revision regenerates and destroys manual edits; no timeline, no export |

The common failure: **the moment a person and an AI both touch the same artifact, one of them loses.** Code-first tools lock out visual edits. Visual tools lock out agents. Generators lock out both.

Flow's bet: a shared, semantic, operation-based document where both parties edit at the level of *entities and intent*, with explicit ownership of each value.

---

## 2. Product Principles

1. **Document is data.** The scene graph is plain, versioned, schema-validated JSON. Code exists only inside sealed components with typed parameters.
2. **Operations, not rewrites.** Every mutation is a typed op against stable IDs. Ops are logged, undoable, attributable and rebased.
3. **Human edits are sovereign.** A user-authored value is never changed by an agent without explicit scope or approval.
4. **Selection is scope.** What the user points at defines what the agent sees and what it may change.
5. **Meaning over geometry.** Entities carry semantics; animations target semantic paths.
6. **Time is narrative.** Beats and script words are the primary time anchors. Seconds are derived.
7. **Deterministic everywhere.** `frame = render(document, t)` is a pure function, identical in preview, presenter and export.
8. **Agent-agnostic.** Integrations go through MCP, a CLI and a documented format. Claude Code, Codex or any future agent plugs in the same way.
9. **Beautiful by default.** Design tokens, layout constraints and a lint pass make good output the path of least resistance.

---

## 3. The Unique Product: Feature Pillars

### 3.1 Point-and-Ask (selection-scoped AI)

The user can point at any **selection target**:

| Target | Example instruction |
|---|---|
| Entity / group | "Make this pulse when the barrier fires." |
| Component sub-part (`grid.workgroups[2]`) | "Highlight only this workgroup and fade the others." |
| Multiple entities | "Align these and stagger their entrance." |
| Time range on the timeline | "This part drags — tighten it to 3 seconds." |
| Beat | "Split this beat into three steps." |
| Word or phrase in the script | "Show the buffer when I say *storage buffer*." |
| Empty canvas region | "Put a diagram of the dispatch here." |
| Whole scene | "Restyle to the dark theme and fix spacing." |

**Scope semantics (the key mechanism):**

- **Read scope:** the agent receives a compact *context packet* (§7.3) built around the selection: the full subtree, a summary of its ancestors, related beats, the script lines, theme tokens, component schemas and a rendered thumbnail.
- **Write scope:** the agent's changeset may only touch the selection's subtree, its animation tracks and its beat anchors. Ops outside that scope are flagged as **scope escalations**, which the user approves one by one.
- **Ownership override:** inside the selection, the user's instruction counts as consent to modify the user-authored properties the instruction concerns. Outside it, user-authored values stay protected.

**Quick intents** (one-click, each one a structured prompt template plus scope):

- **Animate in / out:** an entrance or exit consistent with the theme's motion tokens.
- **Explain this:** draft narration for the selection and anchor it.
- **Variants:** generate 3 alternative treatments, shown side by side as a variant strip; pick one or blend.
- **Componentize:** turn the selected primitives into a reusable parameterized component.
- **Sync to script:** align the selection's animation to the narration words that mention it.
- **Tidy:** fix layout, alignment and contrast issues the linter found in the selection.
- **Step through:** for simulation components, generate per-step beats (for example, one beat per sort pass).

### 3.2 Provenance and Changesets

Every property value has **provenance**:

```ts
type Provenance = {
  author: 'user' | 'agent' | 'theme' | 'component' | 'import';
  actorId: string;        // user id or agent session id
  changesetId?: string;   // set for agent authorship
  at: number;             // logical clock (Lamport) of last write
};
```

- **Inspector badges** show who owns each value. A user can **pin** a value (hard lock: even scoped agents must ask) or **release** it back to the agent's control.
- **Agents never write directly.** They submit a `Changeset` (an ordered op list, a base version, a scope and a rationale). The changeset lifecycle:

```
draft → validated → linted → proposed → (accepted | partially accepted | rejected) → applied
```

- **Review UX:** a ghost overlay on the canvas, a before/after scrub, and a per-op list grouped by entity. Each group can be accepted or rejected on its own. An accepted changeset becomes **one undo unit**.
- **Rebase:** if the user edits while an agent is working, the changeset is rebased onto the new head. Ops address `(entityId, propertyPath)`, so most of them commute. A real conflict, where both sides wrote the same property, is shown to the user. It is never resolved silently.

### 3.3 Semantic Entities

The entity library has three tiers:

1. **Primitives:** text, rich text, shape, path, image, video, group, connector/arrow, math (LaTeX typeset to vector paths), code block (syntax-highlighted, line-addressable).
2. **Semantic components:** typed, parameterized entities with **addressable parts**. Examples: `Array`, `Matrix`, `Graph`, `Tree`, `Chart`, `Timeline`, `StateMachine`, `SequenceDiagram`.
3. **Domain packs:** for example `vk-compute`: `WorkgroupGrid`, `InvocationLattice`, `StorageBuffer`, `DescriptorSet`, `Pipeline`, `CommandBuffer`, `PipelineBarrier`, `Queue`, `ShaderSource`.

Addressable parts are stable semantic paths (`buf.cells[0..15]`, `code.lines[4..7]`, `grid.workgroups[1].invocations[3]`). They can be selected, styled, animated, overridden and referenced by agents, all without "exploding" the component into loose shapes.

**Identity continuity:** an entity keeps its ID across beats and scenes. When the same ID appears in consecutive beats with different properties, Flow automatically **morphs** between them (magic move for everything, including code edits and component parameter changes).

### 3.4 Script-Anchored Beats

- A **scene** is an ordered list of **beats**. A beat is a narrative step: in presenter mode one click advances one beat; in video export it has a duration.
- The **script track** holds narration text split into segments per beat. Words get timing from text-to-speech output, imported voiceover alignment, or reading-rate estimates.
- **Anchors:** a keyframe's time is never stored as a raw second. It is an anchor expression:

```ts
type TimeAnchor =
  | { kind: 'beat'; beatId: string; edge: 'start' | 'end'; offset: number }
  | { kind: 'word'; segmentId: string; wordIndex: number; edge: 'start' | 'end'; offset: number }
  | { kind: 'key'; keyframeId: string; offset: number }   // relative to another keyframe
  | { kind: 'absolute'; sceneTime: number };              // escape hatch
```

- Change the narration, re-record the voiceover or retime a beat, and every anchored animation follows. This removes the most painful step in making explainer videos: re-syncing motion to audio.
- **Holds:** in presenter mode a beat waits at its end for a cue. In export mode it holds for `beat.hold` or for the narration's length.

### 3.5 Programmable, Interactive Components

- A component is a **pure function of (params, time, seed)** that produces a primitive subtree plus named parts and outputs.
- A **simulation component** adds a deterministic `step(state, params) → state` function. The runtime caches checkpoints, so scrubbing to any time is cheap and fully reproducible.
- Parameters marked `live` show up as controls in presenter mode and in the interactive web export. The presenter can change the input array and replay the sort in front of the audience.
- Components run sandboxed (§6.6). They have no DOM, no network, no clock and no unseeded randomness.

### 3.6 Agent Verify Loop

Agents work with eyes open. Through MCP an agent can:

1. `render_frames(scope, times)`: get PNGs of the selection or scene at specific times or beats.
2. `lint(changeset)`: run the scene linter on the proposed state.
3. Iterate privately on the draft changeset before it is proposed to the user.

**Linter rules (v1):** text overflow or clipping, entity out of safe area, unintended overlap, contrast below WCAG AA, font size below the readability floor for the target resolution, simultaneous competing motion (more than N moving entities in one window), animations anchored to deleted beats or words, beats longer than narration plus a tolerance, and theme token violations.

### 3.7 One Project, Three Outputs

| Output | Behaviour |
|---|---|
| **Presenter** | Fullscreen, beat-by-beat, speaker view (script, next beat, timer), live params, keyboard or clicker |
| **Interactive web** (later) | Self-contained static bundle (WebAssembly build of the runtime and a WebGPU backend for the renderer, plus document and assets); viewers step through beats and play with `live` params |
| **Video** | Frame-accurate MP4/WebM at any resolution and fps, with narration audio, via the same deterministic renderer |
| **Stills** | PNG/PDF of each beat's end state (handouts, thumbnails) |

---

## 4. Example Journey (vkcompute Chapter 3: "Workgroups and Invocations")

1. In the agent panel: *"Chapter 3 of my Vulkan compute series: explain workgroups, local invocation IDs and global IDs. 6 minutes, dark theme, use the vk-compute pack."*
2. The agent proposes a changeset: 4 scenes, 18 beats, a draft script, `WorkgroupGrid` and `ShaderSource` entities, and anchored animations. It has already rendered and linted its own frames. The user accepts.
3. The user drags the grid left, changes its accent colour and rewrites two narration sentences. Those properties are now **user-owned**.
4. The user selects `grid.workgroups[1]` and asks: *"When I say 'global ID', show how this invocation's global ID is computed, step by step."* The agent builds a math entity, anchors three keyframes to the words "global", "workgroup" and "local", and leaves the grid position and colour alone.
5. The user re-records the voiceover. The word timings update and every anchored animation retimes itself.
6. Export: an MP4 for YouTube and an interactive web version where viewers can change `local_size_x` and watch the IDs recompute.

---

## 5. Document Model

### 5.1 Project layout on disk

Diff-friendly and git-friendly, one file per scene, stable key ordering:

```
my-talk.flow/
├── flow.json                 # project manifest: schema version, metadata, output presets
├── theme.json                # design tokens
├── scenes/
│   ├── <sceneId>.json        # entities, tracks, beats, script for one scene
│   └── ...
├── components/               # project-local component sources (JavaScript, run in QuickJS)
├── assets/                   # content-addressed: <sha256>.<ext>
├── audio/                    # voiceover takes + alignment JSON
└── .flow/                    # local, not committed: oplog, caches, agent sessions
```

### 5.2 Core types

```ts
type Id = string; // 21-char nanoid, globally unique, never reused

interface Project {
  schemaVersion: number;
  id: Id;
  meta: { title: string; authors: string[]; createdAt: string };
  output: OutputPreset[];          // resolutions, fps, safe areas
  sceneOrder: Id[];
  componentPacks: PackRef[];       // pinned versions
}

interface Scene {
  id: Id;
  name: string;
  size: { w: number; h: number };  // logical units; renderer scales
  rootId: Id;
  entities: Record<Id, Entity>;    // normalized flat map
  beats: Beat[];                   // ordered
  tracks: Record<Id, Track>;
  script: ScriptSegment[];
  provenance: Record<string, Provenance>; // key: `${entityId}:${propertyPath}`
}

interface Entity {
  id: Id;
  kind: string;                    // 'shape' | 'text' | ... | 'vk.WorkgroupGrid'
  name?: string;
  parentId: Id | null;
  children: Id[];                  // ordered by fractional index
  order: string;                   // fractional index key
  props: Record<string, PropValue>;
  overrides?: Record<PartPath, Record<string, PropValue>>; // per-part overrides on components
  layout?: LayoutConstraint[];
  locked?: boolean;
  hidden?: boolean;
}

type PropValue =
  | { t: 'lit'; v: JsonValue }                         // literal
  | { t: 'tok'; token: string }                         // theme token reference
  | { t: 'bind'; expr: string }                         // reactive expression over other props/outputs
  | { t: 'anim'; trackId: Id };                         // driven by a track

interface Track {
  id: Id;
  target: { entityId: Id; part?: PartPath; prop: string };
  keys: Keyframe[];
}

interface Keyframe {
  id: Id;
  at: TimeAnchor;
  value: JsonValue;
  ease: EaseRef;                   // token ('motion.standard') or cubic-bezier / spring params
}

interface Beat {
  id: Id;
  name?: string;
  hold: number | 'narration';      // export-mode hold
  cue: 'click' | 'auto';           // presenter-mode behaviour
  transition?: TransitionRef;
}

interface ScriptSegment {
  id: Id;
  beatId: Id;
  text: string;
  words?: { text: string; start: number; end: number }[]; // relative to beat start
  voice?: { take: AssetRef } | { tts: TtsPreset };
}
```

### 5.3 Layout constraints

Absolute coordinates are fragile when agents and users edit concurrently. Entities can therefore declare relational layout:

```ts
type LayoutConstraint =
  | { kind: 'pin'; edge: Edge; to: EntityRef | 'frame'; gap: number | Token }
  | { kind: 'align'; axis: 'x' | 'y'; to: EntityRef; mode: 'start' | 'center' | 'end' }
  | { kind: 'stack'; dir: 'row' | 'column'; gap: number | Token; alignItems: Align }
  | { kind: 'attach'; to: PartRef; anchor: Anchor };        // e.g. label attached to buf.cells[3]
```

Constraints are solved per frame after animation evaluation. Moving an entity by hand turns its constraint into an explicit offset; it does not delete the constraint.

### 5.4 Operations

The full mutation vocabulary. Nothing else can change a document.

```ts
type Op =
  | { op: 'entity.create'; entity: Entity; parentId: Id; order: string }
  | { op: 'entity.delete'; id: Id }
  | { op: 'entity.move'; id: Id; parentId: Id; order: string }
  | { op: 'prop.set'; id: Id; part?: PartPath; prop: string; value: PropValue }
  | { op: 'prop.unset'; id: Id; part?: PartPath; prop: string }
  | { op: 'layout.set'; id: Id; constraints: LayoutConstraint[] }
  | { op: 'track.upsert'; track: Track }
  | { op: 'track.delete'; id: Id }
  | { op: 'key.upsert'; trackId: Id; key: Keyframe }
  | { op: 'key.delete'; trackId: Id; keyId: Id }
  | { op: 'beat.insert'; beat: Beat; index: number }
  | { op: 'beat.update'; id: Id; patch: Partial<Beat> }
  | { op: 'beat.delete'; id: Id; reanchor: 'prev' | 'next' | 'drop' }
  | { op: 'script.upsert'; segment: ScriptSegment }
  | { op: 'script.delete'; id: Id }
  | { op: 'scene.create' | 'scene.delete' | 'scene.reorder'; /* ... */ }
  | { op: 'theme.set'; token: string; value: JsonValue }
  | { op: 'provenance.pin' | 'provenance.release'; key: string };

interface OpEnvelope {
  op: Op;
  actor: { kind: 'user' | 'agent' | 'system'; id: string };
  clock: number;                   // Lamport clock
  changesetId?: Id;
  inverse: Op[];                   // computed at apply time; powers undo
}
```

Each op is validated against the schema, plus the component parameter schema when it targets a component, **before** it is applied. An invalid op is rejected with a structured error the agent can act on.

### 5.5 Schema evolution

`schemaVersion` is an integer. Migrations are pure `vN → vN+1` functions, run on load and covered by fixture tests. Component packs version their parameter schemas separately and ship their own migrations.

---

## 6. System Architecture

### 6.1 Foundation: Causality

Flow is a native **C11** application built on **[causality](https://github.com/Rounak-Paul/causality)** (vendored as a git submodule at `vendors/causality`). Causality is a Vulkan-based UI library. It gives Flow:

| Causality provides | Flow uses it for |
|---|---|
| Instance / windows (GLFW), custom or native title bars, fullscreen, multi-window | Editor window, presenter audience window, speaker window |
| Declarative widget tree, CSS stylesheets, flex layout, splitters, tabs, trees, tables, menus, popups, modals, tooltips, colour picker | All editor chrome: outline, inspector, panels, dialogs |
| Reactive signals / effects / computed (`ca_reactive.h`) | Binding document state to UI without polling |
| `ca_viewport` (offscreen Vulkan image composited into the UI, `on_render` / `on_resize`, redraw requests) | Scene canvas, timeline lanes, beat thumbnails |
| Shared GPU (`ca_gpu.h`: `VkDevice`, queues, command pool, VMA allocator, `ca_shader_compile` with an on-disk cache) | Flow's own scene renderer on the same device |
| `ca_window_capture`, frame timing, profile hooks | Screenshots, perf HUD |
| Node graph component | Binding/expression graph view (v0.2) |
| Threads, mutex, condvar, `ca_instance_wake`, `ca_instance_request_frame_after`, continuous mode | Worker threads, playback loop |
| Embedded fonts (UI text + icons), FreeType | UI text; FreeType is shared with the scene text engine |

**Division of labour:**

- **Causality draws the UI.**
- **Flow draws the scene** with its own Vulkan renderer into causality viewports and offscreen targets. Nothing in the scene path goes through a widget toolkit or a browser, so Flow controls every pixel: vector coverage, text, blending, colour pipeline, effects and 3D.

### 6.2 Process and thread topology

**One executable, one process per role.** The same binary runs as the editor (default), as a headless engine (`render`, `export`, `lint`, `op`), or as an MCP bridge (`mcp`).

```
┌──────────────────────────────────── flow (editor process) ─────────────────────────────────────┐
│                                                                                                │
│  Main thread (causality loop)                                                                  │
│   ├─ UI: editor chrome (causality widgets + CSS), presenter windows                            │
│   ├─ Document authority: DocumentStore · OpLog · History · Changesets · ScopeGuard · EventBus  │
│   ├─ Runtime: anchors → tracks → bindings → expansion results → layout → ResolvedScene         │
│   └─ Renderer: DisplayList → Vulkan (viewports, offscreen targets, readback)                   │
│                                                                                                │
│  Component thread   QuickJS runtimes · pack + project components · simulation checkpoints      │
│  IPC thread         local socket server (JSON-RPC 2.0) · requests marshalled to main thread    │
│  Encode thread      ffmpeg sidecar pipe · bounded frame queue (backpressure)                   │
│  Media thread       audio decode / mix (miniaudio) · alignment jobs                            │
│  Session threads    agent process stdio pumps                                                  │
│  Watch thread       filesystem notifications → import diff → ops                               │
└──────────────────────────────────────────┬─────────────────────────────────────────────────────┘
                                           │ Unix domain socket / Windows named pipe, token auth
                         ┌─────────────────┴──────────────────┐
                    flow op | lint | render              flow mcp (stdio) ◄── Claude Code / Codex / ...
```

- **The main thread is the only thread that mutates the document or submits GPU work**, using causality's graphics queue. Every other thread talks to it through a mutex-protected command queue drained each tick, and wakes it with `ca_instance_wake()`. Results return through completion callbacks. This removes data races on the document and queue contention by construction.
- **Single writer:** opening a project takes an exclusive lock on `.flow/lock` and starts the socket server. A second `flow` process (CLI or `mcp`) that finds the lock becomes a **client** of the owner over the socket. If no owner exists, `render`, `export`, `lint` and `op` run the engine **headless in-process**.
- **Headless mode** creates a causality instance with no visible windows and renders only to offscreen targets. *Integration requirement:* causality must support a windowless instance (Vulkan device without a surface), or Flow falls back to a hidden 1×1 window. This is tracked in §14.
- **Long work is time-sliced.** Export and multi-frame `render_frames` requests render a bounded batch of frames per tick, so the editor stays responsive while an export runs.

### 6.2.1 Process services (implemented)

- **Logging (`core/fl_log`, `platform/fl_log_file`):** a process-wide, thread-safe logger. Every record goes to registered **sinks** (observers): coloured console, a rotating file (`flow.log`, rotated at 8 MiB, 3 kept), and a bridge that posts `log.record` events onto the bus for in-app consumers. The last 64 KiB of output is kept in a lock-free ring for crash reports. `FL_PANIC` / `FL_ASSERT` log fatally, flush and abort, so they go through crash capture.
- **Jobs (`core/fl_job`):** a worker pool (logical CPUs − 1) with high/normal/low FIFO queues, dependencies, cooperative waiting (a waiting thread runs other jobs, so nested waits never deadlock), `parallel_for`, and `job.done` completion events on the bus. Worker start/exit hooks attach crash handling.
- **Crash capture (`platform/fl_crash`):** handlers for SIGSEGV/BUS/ILL/FPE/ABRT/TRAP, with an alternate signal stack per thread so stack overflows are caught too. On Windows: an unhandled-exception filter, abort and CRT invalid-parameter handlers, plus a minidump. The async-signal-safe report covers app, version, UTC time, pid, thread, fault and address, the faulting frame, a backtrace and the recent log. A marker lets the next launch surface the previous crash. Default handling then resumes, so OS crash reporters and core dumps still work.
- **Graceful termination (`platform/fl_signal`):** SIGINT/SIGTERM/SIGHUP (or console control events on Windows) are received on a watcher thread and posted as `app.quit_requested`, which closes the main window. A second request force-exits.
- **Startup order:** name the main thread → logger → log file → crash handler → bus → termination watcher (before any other thread) → log bridge → jobs → UI. Shutdown runs in reverse: producers stop before the things they wake.

### 6.3 Source layout

```
CMakeLists.txt
vendors/
  causality/       UI + Vulkan foundation (submodule; brings glfw, glm, vma, freetype, stb)
  yyjson/          JSON read/write
  quickjs-ng/      Component sandbox
  harfbuzz/        Text shaping
  tree-sitter/     Code parsing + highlight (with grammars: glsl, c, cpp, rust, python, js)
  microtex/        LaTeX math typesetting → vector paths (C++, behind a C shim)
  miniaudio/       Audio decode, mix, playback
packs/
  std/  vk/        Component packs (JavaScript), embedded into the binary at build time
shaders/           GLSL → SPIR-V at build time (glslc), embedded as byte arrays
src/
  engine/          Causality-free engine; static library `flow_engine`
    core/          fl_event (event bus) · fl_job (worker pool, deps, parallel_for) · fl_log (logger,
                   sinks, panic/assert) · fl_thread (threads, mutex, condvar, names) · fl_atomic
                   · fl_clock · fl_alloc · fl_array · fl_map · fl_str (interning) · fl_id · fl_json
                   · fl_hash (SHA-256, CRC32C) · fl_embed
    platform/      fl_crash (fault capture, reports, minidumps) · fl_signal (graceful termination)
                   · fl_log_file (rotating log sink) · fl_paths (user dirs, portable mode) · fl_fs
                   · fl_lock · fl_sock (UDS / named pipes) · fl_proc (spawn, pipes, kill) · fl_watch
    schema/        fl_value (tagged JSON-like variant) · fl_types · fl_props (built-in prop registry)
                   · fl_validate · fl_serde · fl_migrate
    doc/           fl_doc · fl_op (encode / apply / invert / expand) · fl_history · fl_prov
                   · fl_changeset · fl_rebase · fl_scope
    project/       fl_project (open / close / lock) · fl_snapshot · fl_oplog · fl_assets · fl_import
    time/          fl_tick · fl_anchor · fl_ease (cubic-bezier, spring)
    runtime/       fl_eval · fl_tracks · fl_bind (expression language) · fl_layout · fl_morph
                   · fl_comp (component host, QuickJS) · fl_sim (checkpoints)
    text/          fl_fontdb · fl_shape (FreeType + HarfBuzz) · fl_math (MicroTeX shim) · fl_code
    render/        fl_gpu (device context handed in by the host) · fl_dl (display list) · fl_r2d
                   · fl_glyphs · fl_r3d · fl_fx · fl_target (offscreen, MSAA, readback, YUV)
    lint/          fl_lint (rules)
    media/         fl_audio · fl_align · fl_encode (embedded ffmpeg sidecar)
    ipc/           fl_rpc (JSON-RPC 2.0) · fl_ipc_server · fl_ipc_client
    agent/         fl_context · fl_mcp · fl_session · fl_intents
  editor/          Causality host; static library `flow_editor`
                   fl_editor (instance, windows, bus pump) · ui/ (canvas, outline, inspector,
                   timeline, beats, script, agent, review, export) · fl_presenter · style.h (all
                   theme tokens, class names and the stylesheet; the only place visuals are defined)
  main.c           mode dispatch: editor | mcp | render | export | lint | op | new
tests/             mirrors src/: unit + golden-frame + fault-injection suites (CTest)
```

**Dependency direction (strict, enforced by CMake target links):**

```
engine/core ◄─ platform ◄─ schema ◄─ doc ◄─ project
                             time ◄─ runtime ◄─ render ◄─ lint
                             text ◄─┘
doc, project, runtime, render, lint, media ◄─ ipc, agent ◄─ editor ◄─ main
```

- **`engine/core/fl_event` is the backbone every module depends on.** Modules talk to each other by publishing and subscribing to events. Direct calls are reserved for queries and constructors.
- `doc` knows nothing about rendering or UI.
- `runtime` and `render` know nothing about agents, IPC or causality widgets. `render` only receives a `VkDevice` context through `fl_gpu`.
- **Only `editor/` includes `causality.h`.** That keeps the engine testable headless and portable to another host later.
- **Code conventions** follow causality's: C11, `fl_` prefix, opaque handles in public headers, descriptor structs for creation, explicit `create`/`destroy` pairs, allocator hooks, overflow-checked failure-atomic growth, and documented ownership on every API.

### 6.4 Event model

Everything that changes publishes an event on the application bus (`src/engine/core/fl_event.h`). Subscribers never poll.

```c
Fl_EventBus    *fl_event_bus_create(const Fl_EventBusDesc *desc);          /* owner = creating thread */
Fl_EventType    fl_event_type_register(Fl_EventBus *, const char *name, size_t payload_size);
Fl_Subscription fl_event_subscribe(Fl_EventBus *, const Fl_SubscribeDesc *); /* type|ANY, fn, priority, owner */
bool            fl_event_unsubscribe(Fl_EventBus *, Fl_Subscription);
size_t          fl_event_unsubscribe_owner(Fl_EventBus *, const void *owner);
Fl_PublishResult fl_event_publish(Fl_EventBus *, Fl_EventType, const void *payload, size_t size);
bool            fl_event_post(Fl_EventBus *, Fl_EventType, const void *payload, size_t size); /* any thread */
size_t          fl_event_bus_dispatch(Fl_EventBus *);                       /* drains posted events */
```

- **Runtime-registered types.** Each module registers its own events by dotted name (`doc.ops_applied`, `playback.tick`, `agent.session`) with a fixed payload size, so there is no central enum. Re-registering the same name and size returns the same type, and a size conflict is rejected.
- **Ordered, synchronous delivery** on the owner thread: higher priority runs first, ties in subscription order. `FL_EVENT_TYPE_ANY` subscribers (logging, recording, remote mirroring) take part in the same ordering. A subscriber can consume an event to stop further delivery.
- **Reentrancy is safe.** A nested publish dispatches immediately, up to a depth limit. Subscriptions added during dispatch join after the outermost dispatch returns. Unsubscribed observers are never called again, even mid-dispatch. Handles carry generations, so stale handles are rejected.
- **Cross-thread producers** use `fl_event_post`. It copies the payload into a mutex-protected queue and calls the bus's wake hook on the empty→non-empty transition. The editor installs `ca_instance_wake` as that hook and drains the queue from a `ca_frame_effect`, so posted events are delivered before layout and render in the same tick.
- **Owner groups:** modules subscribe with an `owner` key and drop everything at teardown with `fl_event_unsubscribe_owner`.
- **UI binding:** editor panels subscribe and update causality signals or widgets. Causality's reactive effects take it from there. Panels never read the document every frame.
- **Socket clients** (MCP bridge, CLI) mirror events through an `ANY` subscriber as JSON-RPC notifications, with bounded per-client queues. A lagging client gets `resync`. It never blocks the main thread.

### 6.5 Evaluation pipeline (per frame, main thread)

```
Document ──► resolve time anchors (beats, words) ──► sample tracks ──► resolve bindings (topo-sorted)
        ──► component expansions (cached; misses requested from component thread) ──► part overrides
        ──► layout solve ──► ResolvedScene ──► DisplayList ──► Vulkan passes ──► viewport | offscreen
```

- **Incremental:** `ResolvedScene` nodes carry a generation counter. Unchanged subtrees reuse their display-list chunks and their cached GPU geometry (tessellations, glyph runs).
- **Expansion misses:** in the editor, the frame renders with the previous result for that entity and redraws on `EXPANSION_READY`. In export, a frame **waits** until every expansion it needs has resolved, so export is always exact.
- **Determinism:** no wall clock, a seeded PRNG per component instance, embedded or project-bundled fonts, integer `Fl_Tick` (`int64_t`, 1/120000 s), a fixed evaluation order, and fixed-function GPU state per pass. On a given GPU and driver, preview, presenter and export are identical.
- **Morph transitions:** between beats, entities with the same ID interpolate their resolved properties. Paths use point-correspondence morphing after subdivision to matching segment counts. Text and code use token-level diffs (insert, delete, move).

### 6.6 Rendering (Flow-owned Vulkan)

Flow has full control of rendering. `fl_render` builds passes on causality's `VkDevice`, using its VMA allocator and pipeline cache.

- **Colour pipeline:** scenes render into **RGBA16F linear-light** targets, so blending and gradients are physically correct and banding-free. They are converted to sRGB (preview) or the export colour space at the end. Premultiplied alpha throughout.
- **Vector paths:** fills use **stencil-then-cover** (non-zero and even-odd) on **MSAA** targets (8× where supported, 4× minimum). Strokes are expanded on the CPU (joins, caps, dashes) into fill geometry and cached per path and stroke style.
- **Analytic primitives:** rects, rounded rects, ellipses, lines and arrows are drawn as **SDF quads** with exact coverage AA, avoiding tessellation.
- **Text:** shaped by **HarfBuzz**, rasterized by **FreeType** into a glyph atlas keyed by `(font, glyph, pixel-size bucket, subpixel offset)`. Glyphs are rasterized at the **actual output pixel size**, so 4K export and deep zooms stay crisp. Very large glyphs (above a threshold) switch to path rendering.
- **Math:** LaTeX is typeset by **MicroTeX** into glyph runs and rules, rendered by the same text and path pipeline, so it scales and morphs like any shape.
- **Code:** **tree-sitter** parses the code and highlight queries produce token runs. Tokens are addressable (`code.lines[4].tokens[2]`), which enables real token-level code morphs.
- **Images and video frames:** sampled textures with mipmaps. Video is decoded on the media thread.
- **Effects:** gaussian blur, glow, drop shadow and motion blur (sub-frame accumulation in export only) run as compute passes on offscreen layers. Opacity groups and blend modes render to transient layers.
- **3D (`scene3d`):** a forward renderer with depth, MSAA, unlit and simple lit materials, lines and points, and orthographic/perspective cameras. It renders into the same frame graph and composites in z-order with 2D content.
- **Frame capture:** export renders to an offscreen target at the output resolution. A compute pass converts it to **YUV 4:2:0** (or keeps RGBA for alpha and ProRes 4444), copies it into a host-visible staging ring (N frames in flight), and hands it to the encode thread. There is no window, no compositor and no IPC copy in the loop.
- **Shaders** are compiled from GLSL to SPIR-V at build time and embedded. `ca_shader_compile` is used only for user-authored shader effects (v0.2).

### 6.7 Component sandbox

- Components are **JavaScript (ES2023 modules)**, the language agents write most fluently. JSDoc typedefs ship with the SDK for editor tooling. Packs (`std`, `vk`) are embedded in the binary. Project components live in `components/*.js`.
- They run in **QuickJS-ng** on the **component thread**. Each pack and the project get their own `JSRuntime` with a memory limit, and the only host bindings are the SDK surface: no file system, network, clock or OS. `Math.random` and `Date` are replaced with deterministic shims.
- **Budgets:** an interrupt handler enforces a CPU deadline per expansion. On a violation the expansion is aborted, the runtime is reset if its heap is suspect, and the entity renders a visible error placeholder. The editor never stalls or crashes.
- **Declarative output:** `expand` returns a plain object tree of primitive nodes (2D or 3D) with part tags. It is converted into C structs and validated. A component never touches the renderer.
- **Caching:** results are memoized on `(kind, version, params hash, tick bucket)`. Simulation components store checkpoints every N steps for fast scrubbing.
- The component contract:

```js
export default defineComponent({
  kind: 'vk.WorkgroupGrid',
  version: 1,
  params: {
    groups:    int({ min: 1, max: 64, default: 4, live: true }),
    localSize: int({ min: 1, max: 32, default: 8, live: true }),
    highlight: partRef('workgroups', { optional: true }),
  },
  parts: { workgroups: 'list', 'workgroups[].invocations': 'list' },
  expand({ params, t, theme, rng }) {
    return /* primitive subtree with part tags */;
  },
});
```

- Each pack also exports a **manifest** (param JSON Schema, parts, examples). The core uses it to validate component props, and agents receive it through `describe_component`.

### 6.8 Persistence and history

- **OpLog:** append-only in `.flow/oplog`, made of length-prefixed binary records with a CRC32C each. A torn tail record (after a crash) is detected and truncated on open. It holds the authoritative session history: undo/redo, changeset rebase and provenance.
- **Snapshots** are the committed scene JSON files (yyjson, sorted keys, stable formatting). They are written atomically (temp file → fsync → rename / `ReplaceFileW` → fsync directory) and debounced after ops.
- On startup the core loads the snapshots. If the oplog head is ahead of them (crash recovery), it replays the remaining ops.
- **fs watching** ignores the core's own writes by content hash, so a save never triggers a re-import loop.
- **Assets** are content-addressed (SHA-256) and deduplicated.
- **Collaboration later:** ops address IDs and property paths with Lamport clocks, and children are ordered by fractional index. That makes the model convergent per property (last-writer-wins) and per list. Multi-user sync can be layered on top without changing the format.

### 6.9 Single portable executable

`flow` ships as **one self-contained executable** per platform. There is no installer, and nothing is required beside it.

| Resource | How it is carried |
|---|---|
| UI fonts, icons | Causality's embedded fonts |
| Scene default fonts (sans, serif, mono, math) | Embedded byte arrays |
| Shaders | SPIR-V embedded at build time |
| Component packs, SDK, intent templates, CSS theme | Embedded resource table (`fl_embed`) |
| ffmpeg | Static LGPL build embedded **zstd-compressed**. Extracted on first export to the cache dir as `ffmpeg-<sha256>`, verified by hash, run as a child process (never linked). Falls back to `ffmpeg` on `PATH` if extraction is impossible. |
| Vulkan | Windows/Linux: the system Vulkan loader and driver (always present with a GPU driver). macOS: **MoltenVK linked statically**, no loader needed. |
| Settings | Per-user config dir by default. **Portable mode:** if `flow.portable` sits next to the executable, all settings and caches live beside it. |

- **Linux** ships an x86_64/aarch64 binary linked against glibc's baseline (built on an old-glibc container) with everything else static.
- **macOS** ships a universal binary. Signing and notarization of the bare executable is supported.
- **Windows** ships a static-CRT `flow.exe`.

### 6.10 Technology stack (decided)

| Concern | Choice | Why |
|---|---|---|
| Language | **C11** (C++17 only inside vendored libs: VMA, MicroTeX, HarfBuzz) | Same language and conventions as causality and Quasar |
| Build | **CMake** ≥ 3.20, CTest | Matches causality |
| UI, windows, GPU context | **causality** (submodule) | Our own Vulkan UI stack: widgets, CSS, reactivity, viewports |
| Graphics API | **Vulkan** (MoltenVK on macOS) | Full control: compute, MSAA, custom passes |
| GPU memory | VMA (through causality) | Shared allocator with the UI |
| JSON | **yyjson** | Fast, C, stable writer for diff-friendly files |
| Scripting / components | **QuickJS-ng** | Embeddable, deterministic, memory and CPU limits, JS fluency for agents |
| Text shaping / raster | **HarfBuzz** + **FreeType** (FreeType shared with causality) | Correct shaping, bidi-ready, quality rasterization |
| Math | **MicroTeX** behind a C shim | LaTeX → vector output |
| Code highlight | **tree-sitter** + highlight queries | C library, token-accurate, enables code morphs |
| Audio | **miniaudio** | Single-file C, decode + mix + playback; presenter clock follows the audio device clock |
| Word alignment | `Fl_Aligner` interface: imported JSON, reading-rate estimate, whisper.cpp backend (v0.2) | Offline-first, swappable |
| Video encode | Embedded **ffmpeg** sidecar | Broad codecs (platform H.264, SVT-AV1, ProRes, VP9) without linking FFmpeg |
| IPC | Own `fl_sock` (UDS / named pipes) + JSON-RPC 2.0 | No dependencies; same protocol for CLI and MCP |
| MCP | Own `fl_mcp` (MCP over stdio, JSON-RPC 2.0) | Small protocol surface; no runtime dependency |
| Hashing | Built-in SHA-256, CRC32C | Content addressing, oplog integrity |
| Tests | CTest: unit, property-style randomized op round-trips (seeded), fault-injection on allocation and I/O, golden frames | Correctness of the document model is the product |
| Sanitizers | ASan + UBSan CI job; `-Wall -Wextra -Werror` | Stability first |
| CI | GitHub Actions: macOS (universal), Windows x64, Ubuntu x64 + aarch64 (Vulkan via lavapipe for headless golden tests) | Every PR builds and tests on all targets |

---

## 7. Agent System

### 7.1 Integration surface

- **MCP server** (primary). Any MCP-capable agent connects with `flow mcp` (stdio). The shim connects to the running app over the local socket, and launches the app in headless mode if it isn't running.
- **CLI** (secondary, for scripting): `flow op apply ops.json`, `flow render --beat b12 --out f.png`, `flow lint`.
- **Format spec.** `docs/format.md` documents the schema, so an agent can read the files even without MCP. **Writes still go through ops.**

### 7.2 MCP tools

| Tool | Purpose |
|---|---|
| `get_context(selectionToken)` | The context packet for the user's current Point-and-Ask selection |
| `query(scope, filter)` | Find entities by kind, name, part, tag or text |
| `describe_component(kind)` | Parameter schema, parts, examples |
| `draft_changeset(scope, ops, rationale)` | Create or extend a private draft; returns validation errors |
| `render_frames(draftId, targets[])` | PNGs of the draft state at beats or times, cropped to scope |
| `lint(draftId)` | Lint issues for the draft state |
| `propose(draftId, summary)` | Send the draft to the user for review |
| `request_scope(draftId, entityIds, reason)` | Ask to escalate write scope |
| `list_assets / add_asset` | Asset management (content-addressed) |

The agent never gets a "write file" capability for project files.

### 7.3 Context packet (token-efficient)

```yaml
selection: { kind: part, ref: "grid#a1.workgroups[1]" }
intent: "When I say 'global ID', show how this invocation's global ID is computed"
scene: { id: s3, name: "Global IDs", size: 1920x1080, beats: 6, currentBeat: b3 }
subtree:            # full detail for the selection
  - grid#a1 vk.WorkgroupGrid { groups: 4, localSize: 8 } owner(pos,color)=user
ancestors:          # one line each
  - root#r0 group
neighbors:          # entities overlapping or adjacent to the selection's bounds
  - code#c2 ShaderSource lines=12 "gl_GlobalInvocationID..."
script:
  b3: "Each invocation also has a global ID, computed from its workgroup and local index."
theme: { tokens: [color.accent, motion.standard, type.mono, space.md] }
userOwned: ["grid#a1:transform.position", "grid#a1:style.accent"]
writeScope: ["grid#a1/**", "tracks:grid#a1/**", "beats:b3", "new-entities"]
thumbnail: <png, selection crop at current t>
```

- Summaries come from a stable, compact serializer (IDs, kinds, only non-default props).
- The full document is never sent by default. The agent pulls more through `query` when it needs it.

### 7.4 Agent sessions

- The editor's agent panel can launch a configured agent (Claude Code headless, Codex CLI or another) with the Flow MCP server attached and the selection token passed in. Or the user can drive an agent from their own terminal against the running app.
- Each session is tagged in the oplog. Several sessions can run in parallel on disjoint scopes. Overlapping scopes are queued.
- The model is not hardwired. Intent templates are plain prompt files any agent can use.

### 7.5 ScopeGuard rules

1. Any op outside `writeScope` turns into an escalation item; it is never silently dropped or applied.
2. Ops on **pinned** properties are always escalations.
3. Ops on **user-owned** properties inside the scope are allowed only if the intent is about them (the user asked about that entity). The review UI highlights them as "changes your edit".
4. Deleting user-created entities always needs explicit confirmation.

---

## 8. Editor UI

```
┌──────────────────────────────────────────────────────────────────────────┐
│ Toolbar: tools · theme · preview mode · Present · Export                 │
├───────────┬──────────────────────────────────────────────┬───────────────┤
│ Scenes &  │                                              │ Inspector     │
│ Beats     │                 Canvas                       │ props +       │
│ (strip)   │   select · drag · snap · guides · parts      │ provenance    │
│           │   ghost overlay for proposed changesets      │ badges · pin  │
│ Outline   │                                              ├───────────────┤
│ (entity   │        [ Ask ▸ ] floating on selection       │ Agent panel   │
│  tree)    │                                              │ chat · intents│
│           │                                              │ changesets    │
├───────────┴──────────────────────────────────────────────┴───────────────┤
│ Script lane:  "Each invocation also has a [global ID] ..."               │
│ Beat lane:    | b1 | b2 | b3 ············ | b4 |                         │
│ Track lanes:  grid.opacity  ◆────◆   code.lines[4].hl  ◆──◆              │
└──────────────────────────────────────────────────────────────────────────┘
```

- **Ask affordance:** a floating `Ask ▸` chip on any selection (canvas, outline, timeline range, script words). `⌘K` opens the same prompt with the selection attached.
- **Part selection:** double-click into a component to select its parts, the way you enter a group.
- **Timeline anchors:** keyframes show their anchor (beat or word glyph). Dragging a keyframe near a word snaps the anchor to it.
- **Changeset review:** a list of entity groups, each with accept/reject toggles, plus a canvas ghost and a before/after scrub. `Enter` accepts all; `Esc` dismisses.
- **Variant strip:** a horizontal row of rendered thumbnails for variant proposals. Clicking one previews it live in the canvas.
- **Presenter:** speaker view with script, next beat, elapsed time and live param controls. The audience view is beat-driven.

---

## 9. MVP Scope

### Build (v0.1, dogfood quality)

- Schema, ops, oplog, provenance, undo/redo, changesets with review and rebase.
- Primitives: text, shape, path, image, group, arrow, code block, math.
- Semantic components: `Array`, `Graph`, plus `pack-vk`: `WorkgroupGrid`, `StorageBuffer`, `ShaderSource`, `PipelineBarrier`.
- Beats, script lane, word anchors (TTS alignment or imported alignment JSON), keyframes with easing tokens.
- Identity morph transitions between beats.
- Editor: canvas, outline, inspector with provenance, timeline, beat strip, script lane, agent panel.
- Point-and-Ask for entities, parts, beats and script words. Quick intents: Animate in, Explain this, Sync to script, Tidy.
- MCP server with the verify loop (render_frames, lint).
- Presenter mode (audience window + speaker window). MP4 export through offscreen capture and the embedded ffmpeg sidecar.
- One self-contained portable executable per platform.

### Defer

PPTX import/export, real-time multi-user collaboration, 3D beyond basic `scene3d` entities, a plugin marketplace, particles/VFX, Variants and Componentize intents (v0.2), the interactive web export (needs a WASM + WebGPU port of runtime and renderer), cloud rendering, and enterprise features.

---

## 10. Milestones

Each milestone ends with a demo built from a real vkcompute chapter segment.

| # | Milestone | Deliverables | Exit criteria |
|---|---|---|---|
| **M0** | Foundations + render spike | CMake build into `bin/`, causality window, CI matrix, `base`, `platform`, `schema`, `events`, `core` (ops, invert, oplog, provenance), randomized round-trip suites; **spike:** `fl_render` drawing MSAA paths, SDF primitives and shaped text into a causality viewport, plus offscreen capture → YUV → ffmpeg | Op round-trip holds under randomized tests (apply → invert → identical); migration harness; spike renders at 60fps on macOS, Windows and Linux; 1080p export runs at least at real-time speed |
| **M1** | Static scenes | `runtime` (no time yet), `render` (2D), `text` (HarfBuzz, MicroTeX, tree-sitter), primitives, layout constraints, `project` persistence, minimal editor canvas, inspector | Build a vkcompute title + diagram scene by hand; files reload identically |
| **M2** | Time | Time anchors in `runtime`, tracks, easing, beats, presenter mode, timeline UI, identity morphs | Present a 10-beat segment with morphs at 60fps on a laptop |
| **M3** | Script | `media` (miniaudio playback, alignment), script lane, word alignment (whisper / imported JSON / TTS), word anchors, retime-on-change | Swap the voiceover take; all anchored motion stays in sync with no manual fixes |
| **M4** | Agents | `agent`, `ipc`, MCP bridge (`flow mcp`), CLI modes, context packets, changesets, ScopeGuard, review UI, verify loop, `lint`, headless mode | Key test (§12) passes on a real chapter |
| **M5** | Components | Component host (QuickJS-ng thread), `scene3d` renderer, simulation checkpoints, `pack-vk`, part selection and overrides | Agent animates `grid.workgroups[i]` parts without exploding the component |
| **M6** | Export & polish | `media` encoder (embedded ffmpeg), audio mixdown, stills, portable release binaries, theme presets, starter templates | Publish one full vkcompute chapter video made end to end in Flow |
| **M7** | Validation | Closed beta (10–15 technical creators), telemetry on edit/agent loops | See §12 metrics |

---

## 11. Technical Risks and Mitigations

| Risk | Mitigation |
|---|---|
| Agents produce invalid or ugly ops | Schema validation with structured errors, the verify loop (render + lint) before proposal, intent templates with examples |
| Agent edits clobber user edits | Provenance, ScopeGuard, changesets, rebase; no write path outside ops |
| Context bloat and token cost | Selection-scoped packets, compact serializer, pull-based `query`, thumbnails instead of full dumps |
| Preview/export mismatch | One Flow-owned renderer for every output; embedded fonts; integer ticks with manual stepping; export waits for all component expansions; golden frames with per-GPU-vendor tolerance |
| Owning the renderer is a large surface (paths, text, effects, 3D) | Staged scope (SDF primitives → stencil-cover paths → text → effects → 3D); golden-frame suite from M0; lavapipe in CI for driver-independent baselines |
| Driver and MoltenVK differences | Conservative Vulkan feature baseline (1.2 + widely supported extensions), feature probing with fallbacks, CI on all three OSes |
| Export throughput (readback + encode) | GPU-side YUV conversion, staging ring with N frames in flight, encode on its own thread; measured in the M0 spike |
| Main-thread contention (UI, document, GPU on one thread) | Time-sliced export and render jobs, component work off-thread, incremental evaluation, frame-timing hooks to catch regressions |
| Codec licensing | ffmpeg as an LGPL sidecar process (not linked), platform H.264 encoders, `cargo-deny` license gate |
| Slow scrubbing on simulations | Checkpointed step caches, memoized component expansion keyed on (params hash, t) |
| Unsafe component code | QuickJS-ng runtimes on a dedicated thread with no host I/O bindings, deterministic shims, memory limits, interrupt-based CPU deadlines |
| Word alignment quality | Accept multiple sources (TTS timestamps, forced alignment JSON, manual nudge); anchors survive realignment by word index plus text match |
| Schema churn breaking projects | Versioned schema, pure migrations, fixture corpus from real dogfood projects |
| Scope creep toward "PowerPoint replacement" | Positioned strictly as an explainer engine until M7 validation |

---

## 12. Validation

**Key test:** an agent produces a 2-minute animated technical explanation. A non-programmer then makes 10 visual edits. The agent then makes 3 scoped revisions. **Zero user edits are lost**, and the result exports to video with no visual drift from preview.

**Metrics (beta):**

- Share of agent changesets accepted without modification (target ≥ 60%).
- Number of user-owned values an agent changed without consent (target **0**, treated as a bug).
- Time to produce a 5-minute explainer compared with the user's current tool (target ≥ 3× faster).
- Share of edits made visually vs. through agents (healthy: both well above 20%).
- Week-4 retention among beta creators.

**Competitive tests to run hands-on:** Motion Canvas (editor + presentation), Remotion Studio, Manim + manim-slides, Theatre.js, Gamma, Keynote, plain "LLM writes HTML". Each one should attempt the same vkcompute segment and be scored on output quality, editability, revision survival and time.

---

## 13. Positioning

- **Category:** AI-native explanation engine.
- **Initial audience:** technical educators, developer advocates, researchers and engineering YouTubers. People who explain mechanisms.
- **Message:** *"Point at anything, say what you want, keep every edit you made."*
- **Wedge:** domain packs (Vulkan/GPU compute first, then algorithms and data structures, networking, ML) that make specialist explainer visuals one prompt away and fully editable.
- **Business model (to decide):** open-source runtime, format and CLI (trust and agent ecosystem), with a commercial editor and hosted render/share features.

---

## 14. Open Decisions

1. Final product name and legal/domain clearance.
2. TTS provider(s) and forced-alignment approach for imported voiceover.
3. Whether a local agent session is launched by the editor or always brought by the user.
4. Licensing split between the open runtime and the commercial editor.
5. Interactive web export path: WebAssembly runtime plus a WebGPU backend for `fl_render`, or a separate player.
6. Component pack distribution (embedded, a Flow registry, or git) and version pinning UX.
7. Causality headless support: a windowless instance (device without surface) for `render` / `export` / `lint`, or the hidden 1×1 window fallback.
8. Causality main-loop integration: whether Flow drives `ca_instance_tick` itself (needed for the IPC/event queue drain each tick) or uses a per-tick hook.

---

## 15. Glossary

| Term | Meaning |
|---|---|
| **Entity** | Any node in the scene graph (primitive or component instance) |
| **Part** | A stable, addressable sub-element of a component (`grid.workgroups[2]`) |
| **Beat** | A narrative step; one presenter click, one timed segment in export |
| **Anchor** | A symbolic time reference (beat edge, script word, other keyframe) |
| **Op** | An atomic, validated, invertible document mutation |
| **Changeset** | An agent's proposed group of ops with a scope, rationale and review state |
| **Provenance** | Per-property record of who last authored a value |
| **Scope** | The read/write boundary derived from the user's selection |
| **Context packet** | Compact, selection-centred document summary sent to an agent |
| **Verify loop** | An agent rendering and linting its own draft before proposing it |
