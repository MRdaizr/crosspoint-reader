"""Host-only checks for the font generation hook (no PlatformIO required)."""

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("build_builtin_fonts", ROOT / "scripts" / "build_builtin_fonts.py")
fonts = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = fonts
SPEC.loader.exec_module(fonts)


def source(name, values, ctype="uint16_t", suffix="KernLeftCodepoints", count=""):
    symbol = name + suffix
    return f"static const {ctype} {symbol}[{count}] = {{{values}}};\nstatic const void* {name} = {symbol};\n"


class BuiltinFontGenerationTest(unittest.TestCase):
    def test_shares_only_equal_typed_values_and_rewrites_references(self):
        result, saved, shared = fonts.deduplicate([
            ("a.h", source("a", "0x41, 0x42,")),
            ("b.h", source("b", "65, /* same */ 66,", count="2")),
            ("c.h", source("c", "65, 67,")),
            ("d.h", source("d", "65, 66,", "uint8_t", "KernLeftClassIds")),
        ])
        self.assertEqual((saved, shared), (4, 1))
        self.assertNotIn("static const uint16_t bKernLeftCodepoints", result)
        self.assertIn("static const void* b = aKernLeftCodepoints;", result)
        self.assertIn("cKernLeftCodepoints[]", result)
        self.assertIn("dKernLeftClassIds[]", result)

    def test_signed_values_comments_and_octals(self):
        self.assertEqual(fonts.numeric_tokens("-2, 0x10u, // comment\n +010L,"), (-2, ",", 16, ",", 8, ","))

    def test_struct_shape_is_part_of_identity(self):
        result, saved, shared = fonts.deduplicate([
            ("a.h", source("a", "{1, 2, 3},", "EpdUnicodeInterval", "Intervals")),
            ("b.h", source("b", "{1, 2, 3},", "EpdUnicodeInterval", "Intervals")),
        ])
        self.assertEqual((saved, shared), (12, 1))
        self.assertIn("b = aIntervals", result)

    def test_empty_tables_and_bitmap_payloads_are_not_shared(self):
        result, saved, shared = fonts.deduplicate([
            ("a.h", source("a", "") + "static const uint8_t aBitmaps[] = {1};\n"),
            ("b.h", source("b", "") + "static const uint8_t bBitmaps[] = {1};\n"),
        ])
        self.assertEqual((saved, shared), (0, 0))
        self.assertIn("bKernLeftCodepoints[]", result)
        self.assertIn("bBitmaps[]", result)

    def test_rejects_unsupported_or_inconsistent_initializers(self):
        for text in (source("a", "symbol"), source("a", "1, 2", count="3"), source("a", "{1, 2}", "EpdUnicodeInterval", "Intervals"), source("a", "1", "uint8_t")):
            with self.subTest(text=text), self.assertRaises(ValueError):
                fonts.parse_tables(text)

    def test_profiles_follow_source_includes(self):
        header = "#include <builtinFonts/full.generated.h>\n#include <builtinFonts/a.h>\n#ifndef OMIT_FONTS\n#include <builtinFonts/b.h>\n#endif\n#include <builtinFonts/c.h>\n"
        self.assertEqual(fonts.selected_headers(header), ["a.h", "b.h", "c.h"])
        self.assertEqual(fonts.selected_headers(header, True), ["a.h", "c.h"])
        for slim in (False, True):
            names = fonts.selected_headers((ROOT / "lib/EpdFont/builtinFonts/all.h").read_text(), slim)
            self.assertEqual(len(names), 9 if slim else 37)

    def test_no_cross_profile_dependency_and_deterministic_output(self):
        a = ("a.h", source("a", "65, 66"))
        b = ("b.h", source("b", "65, 66"))
        full = fonts.deduplicate([a, b])
        slim = fonts.deduplicate([b])
        self.assertEqual(full, fonts.deduplicate([a, b]))
        self.assertEqual(slim[1:], (0, 0))
        self.assertNotIn("aKernLeftCodepoints", slim[0])

    def test_unchanged_output_does_not_touch_timestamp(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "font.generated.h"
            self.assertTrue(fonts.write_if_changed(path, "test\n"))
            before = path.stat().st_mtime_ns
            self.assertFalse(fonts.write_if_changed(path, "test\n"))
            self.assertEqual(before, path.stat().st_mtime_ns)

    def test_all_real_tables_are_preserved_or_proven_equal(self):
        directory = ROOT / "lib/EpdFont/builtinFonts"
        header = (directory / "all.h").read_text()
        for slim in (False, True):
            sources = [(name, (directory / name).read_text()) for name in fonts.selected_headers(header, slim)]
            result, saved, shared = fonts.deduplicate(sources)
            keys = {table.key for _, text in sources for table in fonts.parse_tables(text)}
            generated = fonts.parse_tables(result)
            self.assertEqual(keys, {table.key for table in generated})
            self.assertEqual(len(generated), len(keys))
            self.assertGreater(saved, 25000)
            self.assertGreater(shared, 0)


if __name__ == "__main__":
    unittest.main()
