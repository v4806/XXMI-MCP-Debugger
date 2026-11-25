import importlib.util
import json
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CODEX_DIR = ROOT / "plugins" / "xxmi-debug" / "mcp"
DSH_DIR = ROOT / "plugins" / "dsh-xxmi-mcp" / "mcp"


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


class DshServerParityTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.codex = load_module("codex_server", CODEX_DIR / "server.py")
        cls.dsh = load_module("dsh_server", DSH_DIR / "server.py")

    def test_both_flavors_advertise_the_same_tools(self):
        codex_names = {tool["name"] for tool in self.codex.TOOL_DEFINITIONS}
        dsh_names = {tool["name"] for tool in self.dsh.TOOL_DEFINITIONS}
        self.assertEqual(codex_names, dsh_names)

    def test_every_dsh_tool_has_a_handler(self):
        for tool in self.dsh.TOOL_DEFINITIONS:
            self.assertIn(tool["name"], self.dsh.TOOL_HANDLERS)

    def test_dsh_renders_structured_data_as_text(self):
        text = self.dsh._render_tool_text("captured", {"frame": 42})
        self.assertIn("captured", text)
        self.assertEqual(
            json.loads(text.split("\n\n", 1)[1]),
            {"frame": 42},
        )

    def test_dsh_keeps_a_plain_summary_without_structured_data(self):
        self.assertEqual(
            self.dsh._render_tool_text("captured", None), "captured")
        self.assertEqual(
            self.dsh._render_tool_text("captured", {}), "captured")


if __name__ == "__main__":
    unittest.main()
