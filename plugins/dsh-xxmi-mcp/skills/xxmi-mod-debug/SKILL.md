---
name: xxmi-mod-debug
description: "诊断/调试运行中的 XXMI/3DMigoto Mod（状态、日志、帧分析、帧抓取、配置或着色器重载、live D3D11 探针），走 xxmi-debug MCP 工具（mcp__xxmi-debug__*）。Diagnose a running XXMI/3DMigoto Mod through the xxmi-debug MCP tools — status, logs, frame analysis, capture, and config or shader reloads."
---

# XXMI Mod Debugging

DSH 把这些工具注册为 `mcp__xxmi-debug__<tool>`；下文写完整名，少数段落沿用
`<tool>` 简写（例如 `xxmi_live_draw_snapshot` 即 `mcp__xxmi-debug__xxmi_live_draw_snapshot`）。
Use the `xxmi-debug` MCP tools as the runtime channel. Never find, focus, inspect, capture, or send input to a game window.

## Workflow

1. Call `mcp__xxmi-debug__xxmi_list_instances`. Pass the returned `instance_id` to later calls. If several instances exist, disambiguate by `process_path`, `config_path`, or user-supplied context; do not guess.
2. Call `mcp__xxmi-debug__xxmi_status` before diagnosing. Use its `log_path`, `analysis_root`, `analysis_path`, `config_path`, and state fields as the native-reported paths and current state.
3. If `log_enabled=0`, do not treat an old `d3d11_log.txt` as current evidence. Multiple processes sharing one DLL directory also share that log, so cross-check `process_path` and `config_path` before attributing entries. Search recent evidence before opening a broad log; `mcp__xxmi-debug__xxmi_read_log` is case-sensitive, returns the newest matches when `tail` is true, and preserves chronological order.
4. For an existing capture, use `mcp__xxmi-debug__xxmi_list_frame_analyses`, then `mcp__xxmi-debug__xxmi_read_frame_analysis` with a targeted hash, draw, shader, texture, or error search.
5. Trigger `mcp__xxmi-debug__xxmi_trigger_frame_capture` only when existing evidence cannot answer the question. Report the accepted result and `analysis_path`; do not repeat the request while `analysis_active` is true.
6. Use `mcp__xxmi-debug__xxmi_reload` with `target=config` after INI or Mod changes, and `target=shaders` after shader overrides change. Check status or logs afterward instead of assuming the reload succeeded.
7. Use `mcp__xxmi-debug__xxmi_live_context` for the current D3D11 frame-end state, `mcp__xxmi-debug__xxmi_runtime_variables` for Mod conditions and user variables, and `mcp__xxmi-debug__xxmi_live_draw_snapshot` for one exact immediate-context draw. These probes write bounded JSON files and return their paths.
8. Use `mcp__xxmi-debug__xxmi_watch_draw_delta` only when one live draw is insufficient and the dynamic values need comparison across a bounded number of frames. Use `mcp__xxmi-debug__xxmi_probe_resource` and `mcp__xxmi-debug__xxmi_override_trace` to inspect the latest saved snapshot.

`xxmi_live_draw_snapshot` captures the first immediate-context draw after the
request. If it returns `not_matched`, do not retry blindly; check whether the
game is paused, loading, or presenting without draws. `xxmi_watch_draw_delta`
accepts 2 to 30 frames and stops on the first native error.

`xxmi_probe_resource` filters saved resource bindings by stage, slot, hash, or
pointer. Pointer fields are omitted unless `include_pointers=true`.
`xxmi_override_trace` reports only the observed skip, hunting-skip, and
post-command slots. Treat `missing_fields` as unavailable data, not as zero.

Treat frame capture as a one-shot diagnostic action: the DLL temporarily enables hunting when needed and restores the prior mode after the capture. It rejects `analyse_options=hold` and can cause a temporary one-frame stall while 3DMigoto writes resources. Do not change game input yourself.

Treat returned file paths as read-only telemetry unless the user explicitly authorizes editing or exporting those files. Keep searches narrow and limits small enough to avoid pulling an entire frame-analysis log into context.
