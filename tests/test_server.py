import os
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[1]
SERVER_DIR = ROOT / "plugins" / "xxmi-debug" / "mcp"
sys.path.insert(0, str(SERVER_DIR))
sys.path.insert(0, str(Path(__file__).resolve().parent))

import server  # noqa: E402
from mock_pipe import MockPipeServer  # noqa: E402


class NativeProtocolTests(unittest.TestCase):
    def test_live_probe_commands_are_whitelisted(self):
        self.assertIn(server.COMMAND_LIVE_CONTEXT, server.SUPPORTED_COMMANDS)
        self.assertIn(
            server.COMMAND_RUNTIME_VARIABLES,
            server.SUPPORTED_COMMANDS,
        )
        self.assertIn(
            server.COMMAND_LIVE_DRAW_SNAPSHOT,
            server.SUPPORTED_COMMANDS,
        )

    def test_live_probe_tools_expose_expected_names(self):
        names = {tool["name"] for tool in server.TOOL_DEFINITIONS}
        self.assertTrue(
            {
                "xxmi_live_context",
                "xxmi_runtime_variables",
                "xxmi_live_draw_snapshot",
                "xxmi_watch_draw_delta",
                "xxmi_probe_resource",
                "xxmi_override_trace",
            }.issubset(names)
        )

    def test_every_advertised_tool_has_a_handler(self):
        for tool in server.TOOL_DEFINITIONS:
            self.assertIn(tool["name"], server.TOOL_HANDLERS)

    def test_config_reload_uses_extended_native_timeout(self):
        instance = server.Instance(pid=4321, instance="a1b2c3d4")
        with (
            patch.object(server, "_resolve_instance", return_value=instance),
            patch.object(
                server,
                "send_pipe_command",
                return_value={"result": "queued"},
            ) as send,
        ):
            server._tool_reload(
                {"target": "config", "instance_id": instance.instance_id}
            )

        send.assert_called_once_with(
            instance,
            server.COMMAND_RELOAD_CONFIG,
            timeout_s=server.RELOAD_CONFIG_TIMEOUT_S,
        )

    def test_runtime_variables_tool_returns_small_snapshot_values(self):
        instance = server.Instance(pid=4321, instance="a1b2c3d4")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "runtime-variables.json"
            path.write_text(
                json.dumps(
                    {
                        "persistent_values": [
                            {"name": "\\mods\\test.ini\\visible", "value": 1.0}
                        ],
                        "unknown_setting_count": 0,
                    }
                ),
                encoding="utf-8",
            )
            with (
                patch.object(
                    server, "_resolve_instance", return_value=instance
                ),
                patch.object(
                    server,
                    "send_pipe_command",
                    return_value={
                        "command": "runtime_variables",
                        "result": "captured",
                        "snapshot_path": str(path),
                    },
                ),
            ):
                result, summary = server._tool_runtime_variables(
                    {"instance_id": instance.instance_id}
                )

        self.assertEqual(
            result["persistent_values"],
            [{"name": "\\mods\\test.ini\\visible", "value": 1.0}],
        )
        self.assertIn("Runtime variables captured", summary)

    def test_probe_resource_filters_by_stage_and_slot(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "probe.json"
            path.write_text(
                json.dumps(
                    {
                        "resources": [
                            {
                                "stage": "ps",
                                "slot": 2,
                                "hash": "01234567",
                                "pointer": "0x000000001234",
                            },
                            {
                                "stage": "vs",
                                "slot": 2,
                                "hash": "89abcdef",
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )
            result, _ = server._tool_probe_resource(
                {"snapshot_path": str(path), "stage": "ps", "slot": 2}
            )
        self.assertEqual(
            [item["hash"] for item in result["resources"]],
            ["01234567"],
        )
        self.assertNotIn("pointer", result["resources"][0])

    def test_override_trace_reports_missing_data(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "probe.json"
            path.write_text(
                json.dumps({"missing_fields": ["skip_state"]}),
                encoding="utf-8",
            )
            result, _ = server._tool_override_trace(
                {"snapshot_path": str(path)}
            )
        self.assertEqual(result["missing_fields"], ["skip_state"])

    def test_delta_reports_changed_hashes_and_values(self):
        before = {
            "draw": {"index_count": 12},
            "resources": [
                {"stage": "ps", "slot": 1, "hash": "aa"}
            ],
        }
        after = {
            "draw": {"index_count": 24},
            "resources": [
                {"stage": "ps", "slot": 1, "hash": "bb"}
            ],
        }
        delta = server.build_snapshot_delta(before, after)
        self.assertEqual(
            delta["draw"]["index_count"],
            {"before": 12, "after": 24},
        )
        self.assertEqual(
            delta["resources"][0]["hash"],
            {"before": "aa", "after": "bb"},
        )

    def test_delta_marks_changed_draw_signatures(self):
        before = {
            "draw": {"type": "DrawIndexed"},
            "shaders": [{"stage": "ps", "hash": "aa"}],
        }
        after = {
            "draw": {"type": "DrawIndexed"},
            "shaders": [{"stage": "ps", "hash": "bb"}],
        }
        delta = server.build_snapshot_delta(before, after)
        self.assertTrue(delta["signature_changed"])

    def test_delta_rejects_more_than_thirty_frames(self):
        with self.assertRaises(server.ToolArgumentError):
            server._tool_watch_draw_delta({"frames": 31})

    def test_status_retries_while_render_thread_is_temporarily_blocked(self):
        instance = server.Instance(pid=4321, instance="a1b2c3d4")
        timeout = server.NativePipeError(
            6,
            "render_timeout",
            "the game did not reach a frame boundary before the timeout",
        )
        pipe_busy = server.XXMIError(
            "PIPE_BUSY",
            "Waiting for native pipe: the XXMI named pipe is busy.",
        )
        pipe_recreating = server.XXMIError(
            "INSTANCE_GONE",
            "Waiting for native pipe: the XXMI named pipe is unavailable.",
        )

        with (
            patch.object(
                server,
                "send_pipe_command",
                side_effect=[
                    timeout,
                    pipe_busy,
                    pipe_recreating,
                    {"fps": "60.0", "config_generation": "2"},
                ],
            ) as send,
            patch.object(server.time, "sleep"),
        ):
            status = server._status_for(instance)

        self.assertEqual(status["fps"], "60.0")
        self.assertEqual(status["config_generation"], "2")
        self.assertEqual(send.call_count, 4)

    def test_jsonrpc_notification_does_not_return_a_response(self):
        self.assertIsNone(
            server.process_message(
                {"jsonrpc": "2.0", "method": "tools/list", "params": {}}
            )
        )

    def test_jsonrpc_initialize_reports_plugin_version(self):
        response = server.process_message(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "initialize",
                "params": {"protocolVersion": "2025-11-25"},
            }
        )

        self.assertEqual(
            response["result"]["serverInfo"],
            {"name": "xxmi-debug", "version": "0.1.7"},
        )

    def test_jsonrpc_request_rejects_non_object_params(self):
        response = server.process_message(
            {
                "jsonrpc": "2.0",
                "id": 1,
                "method": "tools/list",
                "params": [],
            }
        )

        self.assertEqual(response["error"]["code"], -32602)

    def test_response_parser_preserves_equals_after_first_separator(self):
        parsed = server.parse_key_value_lines(
            b"command=frame_analysis\n"
            b"result=accepted\n"
            b"analysis_path=C:\\Game\\FrameAnalysis-2026-11-22=1\n"
        )

        self.assertEqual(parsed["command"], "frame_analysis")
        self.assertEqual(parsed["result"], "accepted")
        self.assertEqual(
            parsed["analysis_path"],
            "C:\\Game\\FrameAnalysis-2026-11-22=1",
        )

    def test_named_pipe_request_and_response_follow_fixed_binary_protocol(self):
        instance = server.Instance(pid=4321, instance="a1b2c3d4")
        payload = b"protocol=1\npid=4321\nfps=60.0\n"

        with MockPipeServer(instance.pipe_name, payload=payload) as mock:
            self.assertIn(instance, server.discover_instances())
            result = server.send_pipe_command(
                instance,
                server.COMMAND_STATUS,
                request_id=0x10203040,
                timeout_s=1.0,
            )

        self.assertIsNotNone(mock.request)
        self.assertEqual(mock.request["magic"], server.REQUEST_MAGIC)
        self.assertEqual(mock.request["version"], server.PROTOCOL_VERSION)
        self.assertEqual(mock.request["command"], server.COMMAND_STATUS)
        self.assertEqual(mock.request["request_id"], 0x10203040)
        self.assertEqual(mock.request["payload"], b"")
        self.assertEqual(result["protocol"], "1")
        self.assertEqual(result["fps"], "60.0")

    def test_message_mode_response_is_read_as_one_framed_message(self):
        instance = server.Instance(pid=4322, instance="b1b2c3d4")
        payload = b"protocol=1\npid=4322\nblob=" + (b"x" * 5000) + b"\n"

        with MockPipeServer(
            instance.pipe_name,
            payload=payload,
            message_mode=True,
        ):
            result = server.send_pipe_command(
                instance,
                server.COMMAND_STATUS,
                timeout_s=1.0,
            )

        self.assertEqual(result["protocol"], "1")
        self.assertEqual(len(result["blob"]), 5000)

    def test_native_stopping_status_raises_stable_error_fields(self):
        instance = server.Instance(pid=9876, instance="deadbeef")
        payload = b"error=stopping\nerror_message=MCP bridge is stopping\n"

        with MockPipeServer(instance.pipe_name, payload=payload, status=7):
            with self.assertRaises(server.NativePipeError) as caught:
                server.send_pipe_command(
                    instance,
                    server.COMMAND_FRAME_ANALYSIS,
                    timeout_s=1.0,
                )

        self.assertEqual(caught.exception.code, "stopping")
        self.assertEqual(caught.exception.status, 7)
        self.assertEqual(
            caught.exception.message,
            "MCP bridge is stopping",
        )

    def test_native_busy_status_is_distinct_from_stopping(self):
        instance = server.Instance(pid=9876, instance="feedface")
        payload = b"error=busy\nerror_message=frame analysis already active\n"

        with MockPipeServer(instance.pipe_name, payload=payload, status=3):
            with self.assertRaises(server.NativePipeError) as caught:
                server.send_pipe_command(
                    instance,
                    server.COMMAND_FRAME_ANALYSIS,
                    timeout_s=1.0,
                )

        self.assertEqual(caught.exception.code, "busy")
        self.assertEqual(caught.exception.status, 3)

    def test_multiple_instances_require_explicit_selection(self):
        instances = [
            server.Instance(pid=100, instance="11111111"),
            server.Instance(pid=100, instance="22222222"),
        ]

        with self.assertRaises(server.InstanceSelectionError) as caught:
            server.select_instance(instances, None)

        self.assertEqual(caught.exception.code, "AMBIGUOUS_INSTANCE")


class LogPaginationTests(unittest.TestCase):
    def test_disabled_native_log_does_not_read_stale_file(self):
        with self.assertRaises(server.XXMIError) as caught:
            server._log_path_from_status(
                {"log_enabled": "0", "log_path": r"D:\Game\d3d11_log.txt"}
            )

        self.assertEqual(caught.exception.code, "LOG_DISABLED")

    def test_analysis_root_prefers_stable_native_field(self):
        self.assertEqual(
            server._analysis_root_from_status(
                {"analysis_root": r"D:\Game\XXMI", "dll_path": r"D:\Other\d3d11.dll"}
            ),
            r"D:\Game\XXMI",
        )

    def test_analysis_root_can_fall_back_to_dll_directory(self):
        self.assertEqual(
            server._analysis_root_from_status(
                {"dll_path": r"D:\Game\XXMI\d3d11.dll"}
            ),
            r"D:\Game\XXMI",
        )

    def test_tail_search_limit_returns_latest_matches_and_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "d3d11_log.txt"
            path.write_text(
                "hit=1\nmiss\nhit=2\nmiss\nhit=3\nmiss\nhit=4\n",
                encoding="utf-8",
            )

            page = server.read_text_lines(
                path,
                tail=True,
                search="hit=",
                limit=2,
            )

        self.assertEqual(page["lines"], ["hit=3", "hit=4"])
        self.assertTrue(page["truncated"])

    def test_head_search_limit_returns_first_matches(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "d3d11_log.txt"
            path.write_text(
                "hit=1\nmiss\nhit=2\nmiss\nhit=3\n",
                encoding="utf-8",
            )

            page = server.read_text_lines(
                path,
                tail=False,
                search="hit=",
                limit=2,
            )

        self.assertEqual(page["lines"], ["hit=1", "hit=2"])
        self.assertTrue(page["truncated"])

    def test_tail_without_search_returns_last_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "d3d11_log.txt"
            path.write_text("one\ntwo\nthree\nfour\n", encoding="utf-8")

            page = server.read_text_lines(
                path,
                tail=True,
                search=None,
                limit=2,
            )

        self.assertEqual(page["lines"], ["three", "four"])
        self.assertTrue(page["truncated"])

    def test_frame_analysis_log_read_stays_inside_analysis_root(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            analysis = root / "FrameAnalysis-2026-11-22-120000"
            analysis.mkdir()
            (analysis / "log.txt").write_text(
                "DrawIndexed(12)\nHash=abc\nDrawIndexed(13)\nHash=def\n",
                encoding="utf-8",
            )

            page = server.read_frame_analysis_log(
                root,
                analysis.name,
                search="Hash=",
                tail=True,
                limit=1,
            )

        self.assertEqual(page["lines"], ["Hash=def"])
        self.assertTrue(page["truncated"])
        self.assertEqual(Path(page["path"]).name, "log.txt")


if __name__ == "__main__":
    unittest.main()
