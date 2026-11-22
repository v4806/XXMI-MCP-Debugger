# Standalone XXMI MCP DLL

`mcp.dll` is an independent diagnostic bridge for an already-running
ZZMI/3DMigoto instance. It does not replace or modify the official
`d3d11.dll`.

## Build

```powershell
& $env:MSBUILD .\MCPStandalone\MCPStandalone.sln /m `
  /p:Configuration="Zip Release" /p:Platform=x64
```

The output is `MCPStandalone\x64\Zip Release\mcp.dll`.

Run the complete local verification with:

```powershell
.\MCPStandalone\Tests\run_tests.ps1
```

## Install

1. Open XXMI Launcher settings for ZZMI.
2. Open **Advanced**.
3. Enable **Inject Libraries**.
4. Add the absolute path to `mcp.dll`.
5. Prefer XXMI DLL injection mode **Direct**. It loads the official
   `d3d11.dll` before `mcp.dll`.

The DLL pins itself in the game process until process exit. The game must be
restarted to update or remove `mcp.dll`; trying to unload it during a session is
not supported.

Hook mode is also accepted. The adapter waits for the official DLL and reports
`adapter timed out waiting for official d3d11.dll` if device creation was
already in progress or the module never loaded.

## Runtime

The existing `xxmi-debug` Codex plugin discovers
`\\.\pipe\xxmi-mcp-v1-<pid>-<instance>` and uses the unchanged `MCP1`
protocol.

STATUS includes:

- adapter mode and resolved capabilities;
- truthful readiness fields (`adapter_ready`, `capture_state`,
  `device_captured`, `present_hooked`);
- game, DLL, config, log, analysis, and shader paths;
- measured frame count, FPS, frame time, and uptime;
- hunting, frame-analysis state, and `analysis_path` when the semantic anchors
  resolve;
- the current `capture_restore_hunting` value;
- resolver error count, log path, and official DLL SHA-256.

Control commands are queued and executed on the render thread from a
call-through Present hook. A frame-analysis request is reported as accepted
only after the official callback leaves `analyse_frame` active and
`ANALYSIS_PATH` non-empty. The `analyse_options` state is reported as
`unresolved`; `hold` is not inferred from an invented offset.

## Compatibility Diagnostics

The module writes `xxmi_mcp.log` next to `mcp.dll`. It rotates at 2 MiB and
keeps one previous file.

Compatibility failures include the failed capability, signature result,
candidate addresses, `.pdata` validation, `Globals` pointer validation, hook
installation errors, request IDs, and exception codes. Unsupported operations
return a capability-specific error without disabling unrelated read-only
features.

The file version, image timestamp, image size, and SHA-256 identify the
official binary for future adapter updates. They are diagnostic fields, not an
allowlist.

## Re-analyze a New Official DLL

```powershell
python -m MCPStandalone.Tools.analyze_migoto `
  C:\path\to\official\d3d11.dll `
  --json MCPStandalone\analysis\current.json

python -m MCPStandalone.Tools.generate_signatures `
  MCPStandalone\analysis\current.json `
  MCPStandalone\src\MigotoSignaturesData.h
```

Regenerate signatures only after verifying that every target resolves exactly
once at a real `.pdata` function start. The adapter intentionally fails closed
on absent or ambiguous matches.
