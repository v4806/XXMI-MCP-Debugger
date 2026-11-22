from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import struct
from typing import Iterable, Iterator

import capstone
import pefile


TARGETS = {
    "analyse_frame": (
        b"Frame analysis directory: %s\n",
        "Frame analysis directory: %s\n".encode("utf-16le"),
    ),
    "reload_config": (
        b"Reloading d3dx.ini...\n",
        " Reloading d3dx.ini...\n".encode("utf-16le"),
        "Reloading d3dx.ini...\n".encode("utf-16le"),
        b" Reloading d3dx.ini...\n",
    ),
    "reload_fixes": (
        b"> reloading *_replace.txt fixes from ShaderFixes\n",
    ),
}


@dataclass(frozen=True)
class MaskedBytes:
    pattern: bytes
    mask: bytes


@dataclass(frozen=True)
class FunctionRecord:
    begin_rva: int
    end_rva: int


def mask_bytes(data: bytes, length: int, masked_offsets: set[int]) -> MaskedBytes:
    if length < 0 or length > len(data):
        raise ValueError("signature length must fit within the source bytes")
    if any(offset < 0 or offset >= length for offset in masked_offsets):
        raise ValueError("masked offset must lie within the signature length")

    pattern = bytearray(data[:length])
    mask = bytearray(b"\xff" * length)
    for offset in masked_offsets:
        pattern[offset] = 0
        mask[offset] = 0xCC
    return MaskedBytes(bytes(pattern), bytes(mask))


def format_cpp_bytes(data: bytes) -> str:
    return ", ".join(f"0x{byte:02X}" for byte in data)


def _read_rva(pe: pefile.PE, rva: int, size: int) -> bytes:
    return pe.get_data(rva, size)


def _rva_section(pe: pefile.PE, rva: int):
    for section in pe.sections:
        start = section.VirtualAddress
        end = start + max(section.Misc_VirtualSize, section.SizeOfRawData)
        if start <= rva < end:
            return section
    return None


def _find_all(haystack: bytes, needle: bytes) -> Iterator[int]:
    start = 0
    while True:
        index = haystack.find(needle, start)
        if index < 0:
            return
        yield index
        start = index + 1


def _find_anchor_rvas(pe: pefile.PE, anchor: bytes) -> list[int]:
    result: list[int] = []
    for section in pe.sections:
        data = pe.get_data(section.VirtualAddress, section.SizeOfRawData)
        result.extend(
            section.VirtualAddress + offset
            for offset in _find_all(data, anchor)
        )
    return sorted(set(result))


def _function_ranges(pe: pefile.PE) -> list[FunctionRecord]:
    directory = pe.OPTIONAL_HEADER.DATA_DIRECTORY[
        pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]
    ]
    if directory.VirtualAddress == 0 or directory.Size < 12:
        return []
    data = _read_rva(pe, directory.VirtualAddress, directory.Size)
    records = []
    for offset in range(0, len(data) - (len(data) % 12), 12):
        begin, end, _unwind = struct.unpack_from("<III", data, offset)
        if begin < end:
            records.append(FunctionRecord(begin, end))
    return sorted(records, key=lambda record: record.begin_rva)


def _iter_instructions(
    pe: pefile.PE,
    function: FunctionRecord,
) -> Iterator[capstone.CsInsn]:
    disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    disassembler.detail = True
    disassembler.skipdata = True
    data = _read_rva(pe, function.begin_rva, function.end_rva - function.begin_rva)
    yield from disassembler.disasm(
        data,
        pe.OPTIONAL_HEADER.ImageBase + function.begin_rva,
    )


def _instruction_rva(pe: pefile.PE, instruction: capstone.CsInsn) -> int:
    return instruction.address - pe.OPTIONAL_HEADER.ImageBase


def _rip_targets(
    pe: pefile.PE,
    instruction: capstone.CsInsn,
) -> Iterator[int]:
    if instruction.id == 0:
        return
    for operand in instruction.operands:
        if operand.type != capstone.x86.X86_OP_MEM:
            continue
        if operand.mem.base != capstone.x86.X86_REG_RIP:
            continue
        target = (
            instruction.address + instruction.size + operand.mem.disp
        )
        yield target - pe.OPTIONAL_HEADER.ImageBase


def _xref_functions(
    pe: pefile.PE,
    functions: list[FunctionRecord],
    target_rvas: set[int],
) -> list[int]:
    containing: set[int] = set()
    if not target_rvas:
        return []
    for function in functions:
        for instruction in _iter_instructions(pe, function):
            if any(
                target in target_rvas
                for target in _rip_targets(pe, instruction)
            ):
                containing.add(function.begin_rva)
                break
    return sorted(containing)


def _wide_string_start(pe: pefile.PE, anchor_rva: int) -> int | None:
    section = _rva_section(pe, anchor_rva)
    if section is None:
        return None
    section_data = pe.get_data(section.VirtualAddress, section.SizeOfRawData)
    offset = anchor_rva - section.VirtualAddress
    lower = max(0, offset - 512)
    for candidate in range(offset - 2, lower - 1, -2):
        if section_data[candidate : candidate + 2] == b"\x00\x00":
            return section.VirtualAddress + candidate + 2
    return None


def _reference_targets(pe: pefile.PE, anchor_rvas: Iterable[int]) -> set[int]:
    targets: set[int] = set()
    for rva in anchor_rvas:
        targets.update(range(rva, rva + 32))
        wide_start = _wide_string_start(pe, rva)
        if wide_start is not None:
            targets.update(range(wide_start, wide_start + 32))
    return targets


def _masked_offsets(
    pe: pefile.PE,
    function: FunctionRecord,
    length: int,
) -> set[int]:
    offsets: set[int] = set()
    for instruction in _iter_instructions(pe, function):
        if instruction.id == 0:
            continue
        relative = _instruction_rva(pe, instruction) - function.begin_rva
        if relative >= length:
            break
        encoding = instruction.encoding
        if encoding.disp_size >= 4 and encoding.disp_offset:
            relative_disp = relative + encoding.disp_offset
            if relative_disp + encoding.disp_size <= length:
                offsets.update(
                    range(relative_disp, relative_disp + encoding.disp_size)
                )
        if encoding.imm_size >= 4 and encoding.imm_offset:
            relative_imm = relative + encoding.imm_offset
            if relative_imm + encoding.imm_size <= length:
                offsets.update(
                    range(relative_imm, relative_imm + encoding.imm_size)
                )
    return offsets


def _masked_matches(
    data: bytes,
    pattern: bytes,
    mask: bytes,
) -> list[int]:
    matches = []
    for offset in range(0, len(data) - len(pattern) + 1):
        if all(
            mask[index] == 0xCC
            or (data[offset + index] & mask[index])
            == (pattern[index] & mask[index])
            for index in range(len(pattern))
        ):
            matches.append(offset)
            if len(matches) > 1:
                break
    return matches


def _signature_for_function(
    pe: pefile.PE,
    text_data: bytes,
    function: FunctionRecord,
) -> dict:
    function_bytes = _read_rva(
        pe,
        function.begin_rva,
        min(function.end_rva - function.begin_rva, 128),
    )
    initial = min(64, len(function_bytes))
    attempts = [initial, min(96, len(function_bytes)), len(function_bytes)]
    for length in attempts:
        if length == 0:
            continue
        masked = mask_bytes(
            function_bytes,
            length,
            _masked_offsets(pe, function, length),
        )
        matches = _masked_matches(text_data, masked.pattern, masked.mask)
        if len(matches) == 1:
            return {
                "rva": function.begin_rva,
                "size": function.end_rva - function.begin_rva,
                "signature_length": length,
                "pattern": format_cpp_bytes(masked.pattern),
                "mask": format_cpp_bytes(masked.mask),
                "unique": True,
            }
    return {
        "rva": function.begin_rva,
        "size": function.end_rva - function.begin_rva,
        "signature_length": 0,
        "pattern": "",
        "mask": "",
        "unique": False,
    }


def _signature_for_window(
    pe: pefile.PE,
    text_data: bytes,
    begin_rva: int,
    max_length: int,
) -> dict:
    data = _read_rva(pe, begin_rva, max_length)
    for length in (min(32, len(data)), min(48, len(data)), len(data)):
        if length == 0:
            continue
        disassembler = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
        disassembler.detail = True
        disassembler.skipdata = True
        masked_offsets: set[int] = set()
        for instruction in disassembler.disasm(
            data[:length],
            pe.OPTIONAL_HEADER.ImageBase + begin_rva,
        ):
            if instruction.id == 0:
                continue
            relative = instruction.address - (
                pe.OPTIONAL_HEADER.ImageBase + begin_rva
            )
            encoding = instruction.encoding
            if encoding.disp_size >= 4 and encoding.disp_offset:
                offset = relative + encoding.disp_offset
                masked_offsets.update(
                    range(offset, offset + encoding.disp_size)
                )
            if encoding.imm_size >= 4 and encoding.imm_offset:
                offset = relative + encoding.imm_offset
                masked_offsets.update(
                    range(offset, offset + encoding.imm_size)
                )
        masked = mask_bytes(data, length, masked_offsets)
        matches = _masked_matches(text_data, masked.pattern, masked.mask)
        if len(matches) == 1:
            return {
                "rva": begin_rva,
                "signature_length": length,
                "pattern": format_cpp_bytes(masked.pattern),
                "mask": format_cpp_bytes(masked.mask),
                "unique": True,
            }
    return {
        "rva": begin_rva,
        "signature_length": 0,
        "pattern": "",
        "mask": "",
        "unique": False,
    }


def _writable_pointer_candidates(
    pe: pefile.PE,
    function: FunctionRecord,
    text_data: bytes,
) -> list[dict]:
    candidates = []
    image_base = pe.OPTIONAL_HEADER.ImageBase
    for instruction in _iter_instructions(pe, function):
        for target_rva in _rip_targets(pe, instruction):
            section = _rva_section(pe, target_rva)
            if section is None or not (
                section.Characteristics
                & pefile.SECTION_CHARACTERISTICS["IMAGE_SCN_MEM_WRITE"]
            ):
                continue
            raw = _read_rva(pe, target_rva, 8)
            if len(raw) != 8:
                continue
            value = struct.unpack("<Q", raw)[0]
            pointed_rva = value - image_base
            pointed_section = _rva_section(pe, pointed_rva)
            if pointed_section is None:
                continue
            if not (
                pointed_section.Characteristics
                & pefile.SECTION_CHARACTERISTICS["IMAGE_SCN_MEM_WRITE"]
            ):
                continue
            instruction_rva = _instruction_rva(pe, instruction)
            candidates.append({
                "instruction_rva": instruction_rva,
                "global_pointer_rva": target_rva,
                "initial_target_rva": pointed_rva,
                "initial_target_section": (
                    pointed_section.Name.decode(errors="replace").rstrip("\0")
                ),
                "instruction": f"{instruction.mnemonic} {instruction.op_str}",
                "signature": _signature_for_window(
                    pe,
                    text_data,
                    instruction_rva,
                    32,
                ),
            })
    return candidates


def analyze(path: Path) -> dict:
    pe = pefile.PE(str(path), fast_load=False)
    if pe.FILE_HEADER.Machine != pefile.MACHINE_TYPE["IMAGE_FILE_MACHINE_AMD64"]:
        raise ValueError("analysis requires an x64 PE image")
    functions = _function_ranges(pe)
    text_section = next(
        section for section in pe.sections
        if section.Name.rstrip(b"\0") == b".text"
    )
    text_data = pe.get_data(
        text_section.VirtualAddress,
        text_section.SizeOfRawData,
    )

    report = {
        "path": str(path),
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "timestamp": pe.FILE_HEADER.TimeDateStamp,
        "image_size": pe.OPTIONAL_HEADER.SizeOfImage,
        "function_count": len(functions),
        "targets": {},
    }

    for name, anchors in TARGETS.items():
        anchor_rvas = sorted(
            {
                rva
                for anchor in anchors
                for rva in _find_anchor_rvas(pe, anchor)
            }
        )
        function_rvas = _xref_functions(
            pe,
            functions,
            _reference_targets(pe, anchor_rvas),
        )
        details = []
        for function_rva in function_rvas:
            function = next(
                record for record in functions
                if record.begin_rva == function_rva
            )
            details.append(_signature_for_function(pe, text_data, function))
        report["targets"][name] = {
            "anchor_rvas": anchor_rvas,
            "functions": details,
        }

    reload_functions = report["targets"]["reload_config"]["functions"]
    if reload_functions:
        reload_function = next(
            record for record in functions
            if record.begin_rva == reload_functions[0]["rva"]
        )
        report["globals_pointer_candidates"] = _writable_pointer_candidates(
            pe,
            reload_function,
            text_data,
        )
    else:
        report["globals_pointer_candidates"] = []

    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("dll", type=Path)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()

    report = analyze(args.dll)
    output = json.dumps(report, indent=2)
    if args.json:
        args.json.parent.mkdir(parents=True, exist_ok=True)
        args.json.write_text(output + "\n", encoding="utf-8")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
