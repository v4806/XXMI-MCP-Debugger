# XXMI Live Probes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add bounded, on-demand live Mod debugging probes to the standalone XXMI MCP DLL and Codex plugin without persistent per-draw overhead.

**Architecture:** Capture low-risk immediate-context state from the existing Present hook. For exact draw metadata, temporarily install a call-through hook on official `HackerContext::BeforeDraw`, capture state before and after Mod processing, remove the hook at the next Present boundary, then write a bounded JSON snapshot. Expose large snapshots through files and small summaries through the existing named pipe.

**Tech Stack:** C++20, Win32, D3D11/DXGI, MSVC v143, Python 3 standard library, Codex MCP stdio server.

**Spec:** `docs/superpowers/specs/2026-11-23-xxmi-live-probes-design.md`

## Global Constraints

- Never add a persistent per-draw hook.
- Never change `d3dx.ini` or the official `d3d11.dll`.
- Ordinary STATUS and live-context reads must not call `Map`, `CopyResource`, `Flush`, or GPU wait APIs.
- A live draw request captures exactly one immediate-context draw and always passes calls through to the original `BeforeDraw`.
- A live draw request captures the first immediate-context draw after arming; it does not target a draw ordinal.
- Watch mode captures the first immediate-context draw in each requested frame and marks changed draw signatures instead of assuming object identity.
- Reject concurrent live draw requests.
- Maximum watch sequence length is 30; minimum is 2.
- Maximum snapshot size is 2 MiB; retain the newest 20 snapshots.
- Maximum captured counts: 128 resource bindings, 16 vertex buffers, 32 input-layout elements, and 16 viewports/scissors.
- Missing data must be represented explicitly with `missing_fields` or `limits_hit`; never invent hashes or descriptors.
- Existing frame capture, reload, log, status, and fail-closed signature behavior must remain intact.
- All changes occur on branch `feature/live-probes`.

## Review Focus

- A capture armed while the device or context disappears must release COM objects and restore hooks without crashing the game.
- A draw capture must remove the temporary `BeforeDraw` hook even when a getter, allocation, or callback fails.
- If no safe Present boundary exists after device loss, the hook must remain in pass-through `remove_pending` state rather than being forcibly removed during a possible active call.
- Snapshot escaping and truncation must keep every emitted JSON file parseable and below the configured limit.
- Empty or unsupported data must report missing fields rather than returning zero hashes that look valid.
- Deferred-context or command-list draws must not be reported as captured by the immediate-context probe.

---

### Task 1: Snapshot Data Model and JSON Writer

**Files:**
- Create: `standalone/MCPStandalone/src/ProbeTypes.h`
- Create: `standalone/MCPStandalone/src/ProbeJson.h`
- Create: `standalone/MCPStandalone/src/ProbeJson.cpp`
- Create: `standalone/MCPStandalone/Tests/ProbeJsonTests.cpp`
- Modify: `standalone/MCPStandalone/MCPStandalone.vcxproj`
- Modify: `standalone/MCPStandalone/Tests/MCPStandaloneTests.vcxproj`

**Interfaces:**
- Produces:
  - `struct ProbeResourceInfo`
  - `struct LiveProbeSnapshot`
  - `std::string SerializeProbeSnapshot(const LiveProbeSnapshot& snapshot)`
  - `bool WriteProbeSnapshot(const std::filesystem::path& path, const LiveProbeSnapshot& snapshot, std::string& error)`

- [ ] **Step 1: Write failing serialization tests**

```cpp
TEST_CASE(ProbeJsonEscapesStringsAndReportsMissingFields) {
    LiveProbeSnapshot snapshot;
    snapshot.kind = "live_draw";
    snapshot.d3dx_path = "C:\\game\\quote\"path";
    snapshot.missing_fields = {"vs.hash"};
    const auto json = SerializeProbeSnapshot(snapshot);
    REQUIRE(json.find("C:\\\\game\\\\quote\\\"path") != std::string::npos);
    REQUIRE(json.find("\"missing_fields\":[\"vs.hash\"]") != std::string::npos);
}

TEST_CASE(ProbeJsonHonorsResourceAndSizeLimits) {
    LiveProbeSnapshot snapshot;
    snapshot.resources.resize(129);
    REQUIRE_EQ(snapshot.resources.size(), 129U);
    const auto json = SerializeProbeSnapshot(snapshot);
    REQUIRE(json.find("\"limits_hit\"") != std::string::npos);
    REQUIRE(json.size() <= 2U * 1024U * 1024U);
}
```

- [ ] **Step 2: Run the test project and verify the tests fail**

Run:

```powershell
msbuild .\standalone\MCPStandalone\Tests\MCPStandaloneTests.vcxproj /m /p:Configuration=Debug /p:Platform=x64 /v:minimal
```

Expected: FAIL because `ProbeTypes.h`, `ProbeJson.h`, and the test source are not in the project.

- [ ] **Step 3: Define the snapshot model and bounded JSON writer**

`LiveProbeSnapshot` contains scalar identity fields, vectors of resources,
shaders, input elements, vertex buffers, one optional index buffer, views,
samplers, and string vectors `missing_fields` and `limits_hit`. The writer
emits stable key order, escapes JSON strings, caps collection counts, and
returns a parseable JSON object.

- [ ] **Step 4: Run the Debug and Release native tests**

Expected: all existing tests plus the two new tests pass.

- [ ] **Step 5: Commit**

```powershell
git add standalone/MCPStandalone/src/ProbeTypes.h standalone/MCPStandalone/src/ProbeJson.h standalone/MCPStandalone/src/ProbeJson.cpp standalone/MCPStandalone/Tests/ProbeJsonTests.cpp standalone/MCPStandalone/MCPStandalone.vcxproj standalone/MCPStandalone/Tests/MCPStandaloneTests.vcxproj
git commit -m "feat: add bounded live probe snapshot model"
```

### Task 2: Native Commands and MCP Tool Contracts

**Files:**
- Modify: `standalone/MCPStandalone/src/AdapterTypes.h`
- Modify: `standalone/MCPStandalone/src/MCPBridge.cpp`
- Modify: `standalone/MCPStandalone/src/MCPBridge.h`
- Modify: `standalone/MCPStandalone/Tests/MCPBridgeTests.cpp`
- Modify: `plugins/xxmi-debug/mcp/server.py`
- Modify: `tests/test_server.py`
- Modify: `plugins/xxmi-debug/skills/xxmi-mod-debug/SKILL.md`

**Interfaces:**
- Produces native commands:
  - `LiveContext = 6`
  - `RuntimeVariables = 7`
  - `LiveDrawSnapshot = 8`
- Produces plugin tools:
  - `xxmi_live_context`
  - `xxmi_runtime_variables`
  - `xxmi_live_draw_snapshot`
  - `xxmi_watch_draw_delta`
  - `xxmi_probe_resource`
  - `xxmi_override_trace`
- Consumes Task 1 summary paths from `CommandResult.payload`.

- [ ] **Step 1: Add failing protocol and schema tests**

```python
def test_live_probe_commands_are_whitelisted(self):
    self.assertIn(server.COMMAND_LIVE_CONTEXT, server.SUPPORTED_COMMANDS)
    self.assertIn(server.COMMAND_RUNTIME_VARIABLES, server.SUPPORTED_COMMANDS)
    self.assertIn(server.COMMAND_LIVE_DRAW_SNAPSHOT, server.SUPPORTED_COMMANDS)

def test_live_probe_tools_expose_expected_names(self):
    names = {tool["name"] for tool in server.TOOL_DEFINITIONS}
    self.assertTrue({
        "xxmi_live_context",
        "xxmi_runtime_variables",
        "xxmi_live_draw_snapshot",
        "xxmi_watch_draw_delta",
        "xxmi_probe_resource",
        "xxmi_override_trace",
    }.issubset(names))
```

Add C++ assertions that command timeouts return `1500`, `1500`, and `3000`
for commands `6`, `7`, and `8`, and that unsupported commands still fail.

- [ ] **Step 2: Run tests and verify failure**

Run native and plugin tests.

Expected: FAIL because the constants and tools do not exist.

- [ ] **Step 3: Add command enums, timeout routing, response summaries, and schemas**

Native control commands are queued on the render thread. Large data is
returned by file path. Plugin tools validate integers, cap watch lengths,
and return actionable errors for missing snapshots.

- [ ] **Step 4: Run native and plugin tests**

Expected: all pass.

- [ ] **Step 5: Commit**

```powershell
git add standalone/MCPStandalone/src/AdapterTypes.h standalone/MCPStandalone/src/MCPBridge.cpp standalone/MCPStandalone/src/MCPBridge.h standalone/MCPStandalone/Tests/MCPBridgeTests.cpp plugins/xxmi-debug/mcp/server.py tests/test_server.py plugins/xxmi-debug/skills/xxmi-mod-debug/SKILL.md
git commit -m "feat: add live probe MCP commands"
```

### Task 3: Immediate Context Snapshot

**Files:**
- Create: `standalone/MCPStandalone/src/LiveStateCapture.h`
- Create: `standalone/MCPStandalone/src/LiveStateCapture.cpp`
- Create: `standalone/MCPStandalone/Tests/LiveStateCaptureTests.cpp`
- Modify: `standalone/MCPStandalone/src/MigotoAdapter.h`
- Modify: `standalone/MCPStandalone/src/MigotoAdapter.cpp`
- Modify: both `.vcxproj` files

**Interfaces:**
- Produces:
  - `bool CaptureImmediateContext(ID3D11Device1* device, LiveProbeSnapshot& snapshot, std::string& error)`
  - `std::string ProbeSnapshotDirectory(const std::string& analysis_root)`
- Consumes `LiveProbeSnapshot` and `WriteProbeSnapshot`.

- [ ] **Step 1: Write failing tests for format names and snapshot directory**

```cpp
TEST_CASE(LiveStateCaptureNamesKnownFormats) {
    REQUIRE_EQ(ProbeFormatName(DXGI_FORMAT_R8G8B8A8_UNORM), "R8G8B8A8_UNORM");
    REQUIRE_EQ(ProbeResourceDimensionName(D3D11_RESOURCE_DIMENSION_TEXTURE2D), "texture2d");
}

TEST_CASE(LiveStateCaptureBuildsBoundedSnapshotDirectory) {
    const auto path = ProbeSnapshotDirectory("D:\\game\\XXMI");
    REQUIRE(path.find("xxmi_mcp_snapshots") != std::string::npos);
}
```

- [ ] **Step 2: Run tests and verify failure**

Expected: FAIL because the conversion functions do not exist.

- [ ] **Step 3: Implement immediate-context capture**

At the existing Present callback, obtain `ID3D11DeviceContext1`, query
topology, input layout, shader stages, SRVs, VB/IB, RTV/DSV/UAVs, sampler
counts, viewport, scissor, and blend/depth/rasterizer state. Release every
COM reference. Unknown hashes are reported as unavailable.

- [ ] **Step 4: Add `LiveContext` execution**

Queue the command, capture at the next frame, write
`live-context-<frame>-<request>.json`, and return:
`command=live_context`, `result=captured`, `snapshot_path=...`.

- [ ] **Step 5: Run native tests**

Expected: all pass.

- [ ] **Step 6: Commit**

```powershell
git add standalone/MCPStandalone/src/LiveStateCapture.h standalone/MCPStandalone/src/LiveStateCapture.cpp standalone/MCPStandalone/Tests/LiveStateCaptureTests.cpp standalone/MCPStandalone/src/MigotoAdapter.h standalone/MCPStandalone/src/MigotoAdapter.cpp standalone/MCPStandalone/MCPStandalone.vcxproj standalone/MCPStandalone/Tests/MCPStandaloneTests.vcxproj
git commit -m "feat: capture live D3D11 context state"
```

### Task 4: Runtime Variable Snapshot

**Files:**
- Create: `standalone/MCPStandalone/src/RuntimeVariables.h`
- Create: `standalone/MCPStandalone/src/RuntimeVariables.cpp`
- Create: `standalone/MCPStandalone/Tests/RuntimeVariablesTests.cpp`
- Modify: `standalone/MCPStandalone/src/MigotoAdapter.cpp`
- Modify: `standalone/MCPStandalone/src/MigotoAdapter.h`
- Modify: both `.vcxproj` files
- Modify: `plugins/xxmi-debug/mcp/server.py`

**Interfaces:**
- Produces:
  - `bool ReadRuntimeConfig(const std::filesystem::path& config_root, RuntimeConfigSnapshot& snapshot, std::string& error)`
  - `std::string SerializeRuntimeVariables(const RuntimeConfigSnapshot& snapshot)`
- Consumes validated hunting/analysis values already available to `MigotoAdapter`.

- [ ] **Step 1: Write failing parser tests**

```cpp
TEST_CASE(RuntimeVariablesParseUserConfigValues) {
    TemporaryFile user("\\mods\\test.ini\\visible = 1.000000\n");
    RuntimeConfigSnapshot snapshot;
    std::string error;
    REQUIRE(ReadRuntimeConfig(user.root(), snapshot, error));
    REQUIRE_EQ(snapshot.persistent_values[0].name, "\\mods\\test.ini\\visible");
    REQUIRE_EQ(snapshot.persistent_values[0].value, 1.0f);
}

TEST_CASE(RuntimeVariablesReportUnknownSettingsWithoutDuplicates) {
    TemporaryFile log("Unknown settings clean-up will remove them\n");
    REQUIRE_EQ(CountUnknownSettings(log.path()), 1U);
}
```

- [ ] **Step 2: Run tests and verify failure**

Expected: FAIL because the parser does not exist.

- [ ] **Step 3: Implement bounded config and log parsing**

Read `d3dx.ini`, `d3dx_user.ini`, and recent log lines. Return persistent
variables, unknown-setting counts, parse warnings, IniParams slot when
validated, hunting, analysis state, and log/config paths. Limit variables
to 1000 and each string to 512 bytes.

- [ ] **Step 4: Add native `RuntimeVariables` response**

Return a JSON snapshot path and counts. The plugin tool also returns the
small `persistent_values` array directly when it fits.

- [ ] **Step 5: Run native and plugin tests**

Expected: all pass.

- [ ] **Step 6: Commit**

```powershell
git add standalone/MCPStandalone/src/RuntimeVariables.h standalone/MCPStandalone/src/RuntimeVariables.cpp standalone/MCPStandalone/Tests/RuntimeVariablesTests.cpp standalone/MCPStandalone/src/MigotoAdapter.cpp standalone/MCPStandalone/src/MigotoAdapter.h standalone/MCPStandalone/MCPStandalone.vcxproj standalone/MCPStandalone/Tests/MCPStandaloneTests.vcxproj plugins/xxmi-debug/mcp/server.py
git commit -m "feat: expose Mod runtime variables"
```

### Task 5: One-Shot Live Draw Snapshot

**Files:**
- Create: `standalone/MCPStandalone/src/DrawProbe.h`
- Create: `standalone/MCPStandalone/src/DrawProbe.cpp`
- Create: `standalone/MCPStandalone/Tests/DrawProbeTests.cpp`
- Modify: `standalone/MCPStandalone/src/MigotoAdapter.cpp`
- Modify: `standalone/MCPStandalone/src/MigotoAdapter.h`
- Modify: both `.vcxproj` files

**Interfaces:**
- Produces:
  - `enum class DrawProbeState`
  - `class BeforeDrawProbeController`
  - `bool Arm(void* official_before_draw, void* immediate_context, std::uint32_t frame, std::string& error)`
  - `DrawProbeState State() const`
  - `void OnPresent(std::uint32_t frame)`
- Consumes `CaptureImmediateContext`.

- [ ] **Step 1: Write failing hook state tests**

```cpp
TEST_CASE(BeforeDrawProbeCapturesOnlyTargetContext) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    REQUIRE(probe.Arm(hook.original(), &target_context, 10, error));
    hook.Invoke(&other_context, &draw);
    REQUIRE(probe.State() == DrawProbeState::Armed);
    hook.Invoke(&target_context, &draw);
    REQUIRE(probe.State() == DrawProbeState::Captured);
}

TEST_CASE(BeforeDrawProbeTimesOutAndUnhooks) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    REQUIRE(probe.Arm(hook.original(), &target_context, 10, error));
    probe.OnPresent(11);
    REQUIRE(hook.unhooked());
    REQUIRE(probe.State() == DrawProbeState::NotMatched);
}

TEST_CASE(BeforeDrawProbeWaitsForActivePassThroughCalls) {
    FakeBeforeDrawHook hook;
    BeforeDrawProbeController probe(hook);
    REQUIRE(probe.Arm(hook.original(), &target_context, 10, error));
    hook.BeginPassThrough();
    probe.OnPresent(11);
    REQUIRE(!hook.unhooked());
    REQUIRE(probe.State() == DrawProbeState::RemovePending);
    hook.EndPassThrough();
    probe.TryRemoveHook();
    REQUIRE(hook.unhooked());
}
```

- [ ] **Step 2: Run tests and verify failure**

Expected: FAIL because `BeforeDrawProbeController` does not exist.

- [ ] **Step 3: Resolve and temporarily hook official `BeforeDraw`**

Add a validated `BeforeDraw` function resolver using its function bytes and
PData entry. Install one call-through hook at the next Present. The hook must
capture `HackerContext*` and `DrawContext*`, filter on the armed immediate
context pointer, and immediately call the original function for every call.
Keep the hook installed as pass-through until the next Present so it is not
removed while an active trampoline may still be on the stack.

Protect hook entry and removal with an SRWLOCK: hook calls hold a shared lock
across capture and the original call; removal requires the exclusive lock.
Set `remove_pending` on timeout or device loss and retry at Present and
adapter shutdown. Never force-unhook while the exclusive lock cannot be
obtained.

- [ ] **Step 4: Capture draw arguments and pipeline metadata**

Capture draw type, counts, first offsets, topology, shaders, input elements,
VB/IB, SRVs, targets, samplers, and state using one getter call per array
before Mod processing. After the original `BeforeDraw` returns, capture
`call_info.skip`, `call_info.hunting_skip`, and non-null post-command slots.
Record unavailable indirect arguments explicitly.

- [ ] **Step 5: Execute `LiveDrawSnapshot` and write the JSON file**

The adapter serializes one command at a time and returns a summary path.

- [ ] **Step 6: Run native tests and real-game smoke test**

Verify timeout unhooking, no device loss, and no persistent FPS drop.

- [ ] **Step 7: Commit**

```powershell
git add standalone/MCPStandalone/src/DrawProbe.h standalone/MCPStandalone/src/DrawProbe.cpp standalone/MCPStandalone/Tests/DrawProbeTests.cpp standalone/MCPStandalone/src/MigotoAdapter.cpp standalone/MCPStandalone/src/MigotoAdapter.h standalone/MCPStandalone/MCPStandalone.vcxproj standalone/MCPStandalone/Tests/MCPStandaloneTests.vcxproj
git commit -m "feat: capture one live draw snapshot"
```

### Task 6: Snapshot Query Tools

**Files:**
- Modify: `plugins/xxmi-debug/mcp/server.py`
- Modify: `tests/test_server.py`
- Modify: `plugins/xxmi-debug/skills/xxmi-mod-debug/SKILL.md`

**Interfaces:**
- Consumes snapshot files produced by Tasks 3-5.
- Produces:
  - `xxmi_probe_resource(slot, stage, hash, pointer)`
  - `xxmi_override_trace(snapshot_path?, limit)`

- [ ] **Step 1: Write failing filter tests**

```python
def test_probe_resource_filters_by_stage_and_slot(tmp_path):
    snapshot = {"resources": [
        {"stage": "ps", "slot": 2, "hash": "01234567"},
        {"stage": "vs", "slot": 2, "hash": "89abcdef"},
    ]}
    path = tmp_path / "snapshot.json"
    path.write_text(json.dumps(snapshot))
    result, _ = server._tool_probe_resource({
        "snapshot_path": str(path), "stage": "ps", "slot": 2
    })
    assert [item["hash"] for item in result["resources"]] == ["01234567"]

def test_override_trace_reports_missing_data(tmp_path):
    path = tmp_path / "snapshot.json"
    path.write_text(json.dumps({"missing_fields": ["skip_state"]}))
    result, _ = server._tool_override_trace({"snapshot_path": str(path)})
    assert result["missing_fields"] == ["skip_state"]
```

- [ ] **Step 2: Run tests and verify failure**

Expected: FAIL because the handlers and schemas do not exist.

- [ ] **Step 3: Implement bounded lookup and trace formatting**

No resource content is read. Pointer and hash comparisons are exact.
Missing fields are returned explicitly. Raw pointer values require an
explicit `include_pointers=true`.

- [ ] **Step 4: Run plugin tests**

Expected: all pass.

- [ ] **Step 5: Commit**

```powershell
git add plugins/xxmi-debug/mcp/server.py tests/test_server.py plugins/xxmi-debug/skills/xxmi-mod-debug/SKILL.md
git commit -m "feat: query saved live probe metadata"
```

### Task 7: Watch Draw Delta

**Files:**
- Modify: `plugins/xxmi-debug/mcp/server.py`
- Modify: `tests/test_server.py`

**Interfaces:**
- Consumes repeated `LiveDrawSnapshot` responses.
- Produces `xxmi_watch_draw_delta(frames: 2..30)`.

- [ ] **Step 1: Write failing delta tests**

```python
def test_delta_reports_changed_hashes_and_values():
    before = {"draw": {"index_count": 12}, "resources": [{"stage": "ps", "slot": 1, "hash": "aa"}]}
    after = {"draw": {"index_count": 24}, "resources": [{"stage": "ps", "slot": 1, "hash": "bb"}]}
    delta = server.build_snapshot_delta(before, after)
    assert delta["draw"]["index_count"] == {"before": 12, "after": 24}
    assert delta["resources"][0]["hash"] == {"before": "aa", "after": "bb"}

def test_delta_marks_changed_draw_signatures():
    before = {"draw": {"type": "DrawIndexed"}, "shaders": [{"stage": "ps", "hash": "aa"}]}
    after = {"draw": {"type": "DrawIndexed"}, "shaders": [{"stage": "ps", "hash": "bb"}]}
    delta = server.build_snapshot_delta(before, after)
    assert delta["signature_changed"] is True

def test_delta_rejects_more_than_thirty_frames():
    with self.assertRaises(server.ToolArgumentError):
        server._tool_watch_draw_delta({"frames": 31})
```

- [ ] **Step 2: Run tests and verify failure**

Expected: FAIL because `build_snapshot_delta` and the tool do not exist.

- [ ] **Step 3: Implement bounded repeated snapshots**

Issue `LiveDrawSnapshot` sequentially. Stop early on an error. Write the
combined result to one bounded JSON file, and return changed paths only.
Each iteration captures the first immediate-context draw of its frame.

- [ ] **Step 4: Run plugin tests**

Expected: all pass.

- [ ] **Step 5: Commit**

```powershell
git add plugins/xxmi-debug/mcp/server.py tests/test_server.py
git commit -m "feat: compare repeated live draw snapshots"
```

### Task 8: End-to-End Verification, Documentation, and Packaging

**Files:**
- Modify: `README.md`
- Modify: `plugins/xxmi-debug/skills/xxmi-mod-debug/SKILL.md`
- Modify: `scripts/package-release.ps1` only if the release archive needs the new files
- Modify: version in `plugins/xxmi-debug/.codex-plugin/plugin.json` and `README.md` only after all features pass

**Interfaces:**
- Consumes all prior tasks.
- Produces release-ready source, plugin metadata, and package archive.

- [ ] **Step 1: Add documentation tests or schema assertions**

Verify every advertised tool exists in `TOOL_DEFINITIONS` and every tool
handler is present.

- [ ] **Step 2: Run complete native build and tests**

```powershell
$env:XXMI_OFFICIAL_DLL = "D:\Temp\XXMI-PACKAGE-v1.3.3\d3d11.dll"
.\standalone\MCPStandalone\Tests\run_tests.ps1
```

Expected: Debug/Release native tests and pipe tests pass.

- [ ] **Step 3: Run plugin tests**

```powershell
$env:PYTHONDONTWRITEBYTECODE = "1"
python -m unittest discover -s tests -p "test_*.py" -v
```

Expected: all pass.

- [ ] **Step 4: Build the release package**

```powershell
.\scripts\package-release.ps1
```

Expected: a versioned ZIP and matching SHA-256 output.

- [ ] **Step 5: In-game validation**

After the user starts the game, use only the MCP tools to verify:

- status and existing controls;
- `xxmi_live_context`;
- `xxmi_runtime_variables`;
- `xxmi_live_draw_snapshot`;
- `xxmi_watch_draw_delta`;
- `xxmi_probe_resource`;
- `xxmi_override_trace`;
- no device loss and no sustained FPS regression.

- [ ] **Step 6: Commit documentation and release metadata**

```powershell
git add README.md plugins/xxmi-debug/skills/xxmi-mod-debug/SKILL.md plugins/xxmi-debug/.codex-plugin/plugin.json scripts/package-release.ps1
git commit -m "docs: document live probe debugging"
```
