import unittest

from MCPStandalone.Tools import analyze_migoto


class MaskTests(unittest.TestCase):
    def test_masks_requested_bytes(self):
        result = analyze_migoto.mask_bytes(
            b"\x48\x8b\x05\x78\x56\x34\x12",
            7,
            {3, 4, 5, 6},
        )
        self.assertEqual(result.pattern, b"\x48\x8b\x05\x00\x00\x00\x00")
        self.assertEqual(result.mask, b"\xff\xff\xff\xcc\xcc\xcc\xcc")

    def test_rejects_out_of_range_mask(self):
        with self.assertRaises(ValueError):
            analyze_migoto.mask_bytes(b"\x90", 1, {1})

    def test_formats_cpp_initializer(self):
        result = analyze_migoto.mask_bytes(
            b"\x48\x8b\x05\x78\x56\x34\x12",
            7,
            {3, 4, 5, 6},
        )
        self.assertEqual(
            analyze_migoto.format_cpp_bytes(result.pattern),
            "0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00",
        )
        self.assertEqual(
            analyze_migoto.format_cpp_bytes(result.mask),
            "0xFF, 0xFF, 0xFF, 0xCC, 0xCC, 0xCC, 0xCC",
        )


if __name__ == "__main__":
    unittest.main()
