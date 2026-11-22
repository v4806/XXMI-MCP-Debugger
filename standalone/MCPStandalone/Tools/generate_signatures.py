from __future__ import annotations

import argparse
import json
from pathlib import Path


TARGET_NAMES = {
    "analyse_frame": "AnalyseFrame",
    "reload_config": "ReloadConfig",
    "reload_fixes": "ReloadFixes",
}


def _cpp_values(text: str) -> str:
    return text


def generate(report: dict) -> str:
    lines = [
        "#pragma once",
        "",
        "#include <array>",
        "#include <cstdint>",
        "",
        "namespace signatures_data {",
        "",
    ]
    for key, name in TARGET_NAMES.items():
        functions = report["targets"][key]["functions"]
        unique = next((item for item in functions if item.get("unique")), None)
        if unique is None:
            raise ValueError(f"no unique signature for {key}")
        pattern = unique["pattern"]
        mask = unique["mask"]
        count = len(pattern.split(","))
        lines.extend(
            [
                f"inline constexpr std::array<std::uint8_t, {count}> "
                f"k{name}Pattern = {{",
                f"    {_cpp_values(pattern)},",
                "};",
                f"inline constexpr std::array<std::uint8_t, {count}> "
                f"k{name}Mask = {{",
                f"    {_cpp_values(mask)},",
                "};",
                "",
            ]
        )

    candidate = next(
        item
        for item in report["globals_pointer_candidates"]
        if item["signature"].get("unique")
    )
    signature = candidate["signature"]
    count = len(signature["pattern"].split(","))
    lines.extend(
        [
            f"inline constexpr std::array<std::uint8_t, {count}> "
            "kGlobalsLoadPattern = {",
            f"    {_cpp_values(signature['pattern'])},",
            "};",
            f"inline constexpr std::array<std::uint8_t, {count}> "
            "kGlobalsLoadMask = {",
            f"    {_cpp_values(signature['mask'])},",
            "};",
            "",
            "}  // namespace signatures_data",
            "",
        ]
    )
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("analysis", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = json.loads(args.analysis.read_text(encoding="utf-8"))
    args.output.write_text(generate(report), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
