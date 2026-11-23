# XXMI Live Probe Design

## Intent

Extend the standalone XXMI MCP bridge with on-demand live data that cannot be
recovered reliably from a saved FrameAnalysis or a log file. The target user
is a Mod author checking why a draw, texture, shader, render state, dynamic
buffer value, or Mod condition differs from the intended result.

The feature must remain read-only by default, must not add a permanent
per-draw hook, and must fail closed if an official structure cannot be
validated.

## Goals

- Report the immediate D3D11 pipeline state when requested.
- Capture metadata for one next-frame draw and restore normal drawing
  immediately after capture.
- Compare repeated live draw snapshots without continuously recording every
  draw.
- Expose Mod runtime variables that affect conditions and command lists.
- Query resources referenced by the latest snapshot by slot, hash, or pointer.
- Report available Mod override and skip information without claiming data
  that was not observed.
- Write large results to bounded JSON files and return a short summary through
  the existing named pipe.

## Non-Goals

- Do not dump arbitrary texture or buffer contents in the first version.
- Do not add persistent per-draw, per-copy, or per-resource hooks.
- Do not support deferred-context or command-list capture in the first version.
- Do not derive game-specific bone names, character IDs, or object names from
  raw D3D data.
- Do not perform GPU synchronization, `Map`, `CopyResource`, or readback for
  ordinary status or live context queries.
- Do not change the official `d3d11.dll` or add configuration to `d3dx.ini`.

## Approaches Considered

### Persistent Draw Hook

Hook every draw entry and update a ring buffer.

Advantages:

- Exact state for every draw.
- Natural basis for long-running histories.

Rejected because:

- Runs on the hottest path.
- Can affect frame pacing even when the probe is idle.
- Increases crash exposure across every game render path.

### Present-Only State Reader

Read the current D3D11 state from the existing Present hook.

Advantages:

- Lowest risk.
- No new draw interception.
- Cheap enough for on-demand queries.

Limits:

- Present usually observes frame-end state, not the state at an arbitrary draw.
- Cannot reliably capture one specific draw or compare a draw across frames.

### Hybrid Selected Approach

Use the existing Present hook for low-risk status and immediate-context data,
and temporarily hook the official `HackerContext::BeforeDraw` function only
while one snapshot request is armed. The hook calls the original function and
captures state both before and after Mod processing, then removes the hook at
a Present boundary.

Advantages:

- Zero idle per-draw overhead.
- Exact entry state for the requested draw.
- Bounds the expensive work to one explicit command.

Costs:

- More complex hook installation and removal.
- One capture frame can take approximately 0.3-2 ms.
- Only immediate-context draws are supported; deferred contexts and command
  lists are outside the first version.

## Chosen Architecture

The standalone DLL gets four native request types:

- `LiveContext`: capture immediate-context state at the next Present.
- `RuntimeVariables`: capture known live Mod state and configuration state.
- `LiveDrawSnapshot`: capture one next-frame draw.
- `WatchDrawDelta`: collect a bounded sequence of one-shot draw snapshots and
  summarize values that changed between captures.

`LiveDrawSnapshot` captures the first immediate-context draw observed after
arming. `WatchDrawDelta` captures the first immediate-context draw in each of
the requested frames; it does not infer a stable draw by ordinal. The delta
records `signature_changed=true` when draw type, topology, or a readable shader
hash changes, so callers do not mistake unrelated draws for one dynamic draw.

The Codex plugin adds two query tools that work from saved snapshot files:

- `xxmi_probe_resource`: filter a live snapshot by slot, hash, or pointer.
- `xxmi_override_trace`: return the observed Mod/override and draw-result data
  from the latest live draw snapshot.

`WatchDrawDelta` is implemented in the plugin by repeatedly issuing
`LiveDrawSnapshot`; the native DLL only has to guarantee one-shot capture.
This keeps the high-risk native path small.

## Snapshot Data Contract

Every live snapshot file contains:

- format version and snapshot kind;
- process ID, official DLL hash, `mcp.dll` hash, frame number, and capture time;
- hunting, analysis-active, selected shader, and selected buffer values when
  validated;
- limits hit and missing-field reasons;
- arrays for shaders, input-layout elements, vertex buffers, index buffer,
  shader resources, render targets, depth target, UAVs, and sampler metadata.

Draw snapshots additionally contain:

- draw type and raw draw arguments;
- primitive topology;
- viewport and scissor;
- blend, depth-stencil, and rasterizer descriptors;
- which post-command slots changed during the official draw call;
- the observed skip and hunting-skip flags after Mod processing.

Resource records include:

- stage and slot;
- pointer identity;
- view and resource dimension/format;
- buffer or texture descriptor fields;
- current and original hash only when an existing validated hash is available;
- explicit `hash_unavailable` instead of guessing.

## Bounds and Performance

- Live immediate-context capture: on demand only.
- Live draw capture: exactly one draw.
- Watch sequence: maximum 30 captures and minimum 2.
- Maximum 128 resource bindings, 16 vertex buffers, 32 input elements, and
  16 viewports/scissors per snapshot.
- Snapshot files: maximum 2 MiB each, newest 20 retained.
- No file, heap growth, hashing, or GPU wait inside the before-draw hook.
- Pipe response remains below 16 KiB and contains paths and small summaries.

## Failure Handling

- Each probe resolves or validates its own required signatures.
- A failed probe is removed from STATUS capabilities.
- A request returns a capability-specific error with the failed field or
  structure.
- The before-draw hook passes every non-target call through to the original
  function. Hook traversal and removal are serialized with a shared/exclusive
  lock so removal cannot race an active trampoline.
- A failed or timed-out Draw request is marked `remove_pending` and retried at
  the next Present and adapter shutdown. If the device is lost and no safe
  Present boundary exists, the hook is not forcibly removed while the process
  is live; the state and official module address are logged.
- The native adapter serializes capture requests and rejects concurrent arms.
- If the next expected frame has no draw, the request returns `not_matched`.

## Validation

- Unit tests for snapshot serialization, bounds, escaping, retention, queue
  handling, capability gating, and vtable restoration.
- Protocol tests for all new commands and error responses.
- Plugin tests for tool schemas, snapshot filtering, delta generation, and
  missing-file behavior.
- Native build and pipe tests in Debug and Release.
- In-game validation with the installed standalone DLL after the user starts
  the game. No Codex process may interact with the game window.

## Approval Note

The user explicitly requested the design and plan be written and then executed
in this session. This instruction is treated as approval of the selected
hybrid approach and as authorization to proceed after the plan is saved.
