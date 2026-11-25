# dsh-xxmi-mcp（DSH 侧插件端）

把 [v4806/XXMI-MCP-Debugger](https://github.com/v4806/XXMI-MCP-Debugger) 的
`xxmi-debug` MCP 服务器接进 DeepSeek Harness（DSH），以 `mcp__xxmi-debug__*`
暴露 13 个工具。

这是与 Codex 插件端**并列**的独立一套，两边互不覆盖：

| 目录 | 面向 | 主要文件 |
|---|---|---|
| `plugins/xxmi-debug/` | Codex | `.codex-plugin/plugin.json`、`.mcp.json`、`skills/xxmi-mod-debug/`（含 `agents/openai.yaml`） |
| `plugins/dsh-xxmi-mcp/` | DSH | `package.json`、`cordis.patch.yml`、`locale/`、`docs/DETAIL.md`、`skills/xxmi-mod-debug/SKILL.md` |

两份 `mcp/server.py` 来自同一份上游实现，DSH 这份只多一个 `_render_tool_text()`：
DSH 只渲染 text content block，Codex 读 `structuredContent`，所以把结构化结果附在
文本后面，两种客户端都能拿到数据。

## 安装到 DSH

1. 复制本目录到 `<DSH_HOME>/plugins/dsh-xxmi-mcp/`。`cordis.patch.yml` 用
   `dshHomePath('plugins/dsh-xxmi-mcp/mcp/server.py')` 定位服务器脚本，目录名必须一致。
2. 复制 `skills/xxmi-mod-debug/` 到 `<DSH_HOME>/skills/xxmi-mod-debug/`。
3. 在 DSH 里启用 bundle `dsh-xxmi-mcp`，让 MCP 客户端重连后即可看到
   `mcp__xxmi-debug__*` 工具。
4. 游戏侧还需要注入仓库构建的 `mcp.dll`，前置条件见 [docs/DETAIL.md](docs/DETAIL.md)。

## 与 Codex 版的差异

- 工具名：DSH 注册为 `mcp__xxmi-debug__xxmi_status`；Codex 侧为 `xxmi_status`。
- 服务器路径：DSH 用 `dshHomePath()`，Codex 用 `${PLUGIN_ROOT}`。
- 技能文件：DSH 只需要 `SKILL.md`（带 `name` 前置元数据），正文里的工具名写全前缀。
- 结果渲染：见上面的 `_render_tool_text()`。

## 许可与来源

GPLv3。`mcp/server.py`、`LICENSE.GPL.txt`、`THIRD_PARTY_NOTICES.md` 来自上游仓库；
`package.json`、`cordis.patch.yml`、`locale/`、`docs/`、`icon.svg` 与 `skills/` 中的
DSH 技能是本仓库为 DSH 新增的。
