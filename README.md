# XXMI MCP Debugger

Standalone MCP bridge and Codex plugin for debugging a running
ZZMI/3DMigoto instance without replacing or modifying the official
`d3d11.dll`.

## Components

- `standalone/` builds `mcp.dll`, a small diagnostic DLL that attaches to the
  official 3DMigoto instance in the game process.
- `plugins/xxmi-debug/` contains the Codex MCP server and
  `xxmi-mod-debug` skill.

The bridge exposes only the runtime operations needed for Mod debugging:

- discover running instances and report readiness;
- read 3DMigoto logs and FrameAnalysis output;
- request one FrameAnalysis capture;
- reload shaders;
- request an official configuration reload;
- capture the current immediate-context D3D11 pipeline state;
- read persistent Mod variables and unknown-setting diagnostics;
- capture one exact immediate-context draw;
- compare repeated draw captures across a bounded frame sequence;
- query saved resource bindings and override/skip traces;
- report frame timing, hunting state, capture state, and compatibility
  diagnostics.

The configuration reload path sets 3DMigoto's own
`gReloadConfigPending` field. It does not call `ReloadConfig()` directly.
If official signature or `Globals` validation fails, only that capability is
disabled and the reason is written to `xxmi_mcp.log`.

## Requirements

- Windows x64
- Visual Studio 2022 C++ tools with platform toolset `v143` for building
- Python 3.10 or newer for the Codex MCP server
- A running XXMI/3DMigoto game process

## Build

```powershell
cd standalone
msbuild .\MCPStandalone\MCPStandalone.sln /m `
  /p:Configuration="Zip Release" /p:Platform=x64
```

The output is:

```text
standalone/MCPStandalone/x64/Zip Release/mcp.dll
```

Run all native and pipe tests:

```powershell
python -m pip install -r requirements-dev.txt
.\standalone\MCPStandalone\Tests\run_tests.ps1
```

Set `XXMI_OFFICIAL_DLL` to test a specific official `d3d11.dll`:

```powershell
$env:XXMI_OFFICIAL_DLL = "C:\path\to\official\d3d11.dll"
.\standalone\MCPStandalone\Tests\run_tests.ps1
```

Build the release archive:

```powershell
.\scripts\package-release.ps1
```

## Install

1. Inject the official XXMI/3DMigoto `d3d11.dll` first.
2. Inject `mcp.dll` into the same game process after the official DLL.
3. Install or load `plugins/xxmi-debug` as a Codex local plugin.
4. Restart the game after replacing `mcp.dll`; the DLL stays loaded until the
   process exits.

Do not add an `[MCP]` section or MCP switch to `d3dx.ini`.

The plugin uses the portable `${PLUGIN_ROOT}` placeholder from Codex for the
Python server path.

## Runtime Files

`mcp.dll` writes `xxmi_mcp.log` beside the DLL. The log rotates at 2 MiB and
keeps one previous file.

Live probe snapshots are written under:

```text
<official d3d11.dll directory>\xxmi_mcp_snapshots\
```

Each JSON file is capped at 2 MiB and the newest 20 files are retained.
`xxmi_live_draw_snapshot` installs the official `BeforeDraw` hook only for the
requested capture and leaves it pass-through until the next Present can remove
it safely.

The MCP endpoint uses a local Windows named pipe:

```text
\\.\pipe\xxmi-mcp-v1-<pid>-<instance>
```

It does not open a network listener.

## Compatibility

The adapter resolves the official functions and `Globals` fields at runtime.
It does not use a fixed XXMI version allowlist. Missing, ambiguous, or invalid
signatures fail closed and produce an actionable log entry.

The one-shot draw probe resolves `HackerContext::BeforeDraw` from the live
draw-method vtable and validated direct call targets. If the official layout
or call shape changes, only that capability fails, the reason is logged, and
normal rendering continues.

Use `MCPStandalone/Tools/analyze_migoto.py` and
`MCPStandalone/Tools/generate_signatures.py` when a new official DLL changes
one of the supported anchors.

## License

GPLv3. The standalone DLL links the Nektra `NktHookLib` static library.
See `LICENSE.GPL.txt` and `THIRD_PARTY_NOTICES.md`.
