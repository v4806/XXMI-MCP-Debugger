# XXMI MCP 调试器（DSH 移植）

把 [v4806/XXMI-MCP-Debugger](https://github.com/v4806/XXMI-MCP-Debugger) 的
`xxmi-debug` MCP 服务器接进 DeepSeek Harness（DSH）：内置 `mcp/server.py`，由一个
配置型 bundle 以 stdio 方式接进 `@deepseek-ai/dsh-mcp-client`，工具名形如
`mcp__xxmi-debug__xxmi_status`。

Codex 插件端保持在 `plugins/xxmi-debug/` 不动；本目录是 DSH 专用的一套，两者并存。

## 与 Codex 版的唯一代码差异

`mcp/server.py` 与 `plugins/xxmi-debug/mcp/server.py` 一致，只多一个
`_render_tool_text()`：DSH 只渲染 text content block，Codex 读
`structuredContent`，所以该函数把结构化结果以格式化 JSON 追加到文本后面。空结果
时保持原来的纯文本摘要。

配套技能 `skills/xxmi-mod-debug/SKILL.md` 与 Codex 侧同源，工具名改为 DSH 的
`mcp__xxmi-debug__*` 形式，并安装到 `<DSH_HOME>/skills/xxmi-mod-debug/`。

## 前置条件

1. 游戏先注入**官方** XXMI/3DMigoto 的 `d3d11.dll`。
2. 再注入本仓库构建出的 `mcp.dll`（XXMI Launcher → ZZMI 设置 → Advanced → 启用
   **Inject Libraries** → 添加 `mcp.dll` 绝对路径；优先用 Direct 注入模式）。
3. 该 DLL 生成的命名管道形如 `\\.\pipe\xxmi-mcp-v1-<pid>-<instance>`；本插件只通过
   本地管道与它通信，不开放网络监听。

本 bundle 只负责 MCP 连接，不包含、也不构建 `mcp.dll`。DLL 缺失或未注入时工具仍可
调用，但 `xxmi_list_instances` 会返回 0 个实例。

## 配置

`cordis.patch.yml` 默认值：

| 项 | 值 |
|---|---|
| server name | `xxmi-debug`（决定工具前缀） |
| transport | `stdio` |
| command | `C:/Program Files/Python/python.exe`（3.10+，仅标准库） |
| args | `-u`、`<dshHome>/plugins/dsh-xxmi-mcp/mcp/server.py` |
| cwd | `<dshHome>/plugins/dsh-xxmi-mcp/mcp` |
| startupTimeoutMs | 20000 |

如果 Python 装在别处，改 `command` 后重启 DSH。MCP 客户端会自动重连并刷新工具列表。

## 工具

- `xxmi_list_instances` — 发现实例及状态
- `xxmi_status` — 读取运行状态字段（含 `screen_width` / `screen_height`）
- `xxmi_read_log` — 读 `d3d11_log.txt`（子串搜索 / 最新尾部 / 限行）
- `xxmi_list_frame_analyses` / `xxmi_read_frame_analysis` — 帧分析目录与日志
- `xxmi_trigger_frame_capture` — 请求一次帧分析抓取
- `xxmi_reload` — `target=config|shaders` 重载
- `xxmi_live_context` — 抓取当前 immediate-context D3D11 状态快照
- `xxmi_runtime_variables` — Mod 运行时变量与未知设置诊断
- `xxmi_live_draw_snapshot` — 抓取下一次 exact draw
- `xxmi_watch_draw_delta` — 2–30 帧内的 draw 变化对比
- `xxmi_probe_resource` / `xxmi_override_trace` — 查询已保存快照中的资源绑定与覆盖轨迹

## 许可与来源

- `mcp/server.py`、`LICENSE.GPL.txt`、`THIRD_PARTY_NOTICES.md` 来自上游仓库（GPLv3），
  除上述 `_render_tool_text()` 外未做其他修改。
- 仅新增 DSH 的 `package.json` / `cordis.patch.yml` / `locale` / 文档 / 图标 / 技能。
