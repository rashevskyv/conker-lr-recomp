"""Synthetic input-entry fixtures; no game binary required."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location(
    "compat", Path(__file__).parents[1] / "tools/windows_input_compat.py")
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


class CompatTests(unittest.TestCase):
    def fixture(self, root):
        (root / "recomp_funcs.h").write_text("void sub_0055BDF8(void);\n")
        (root / "recomp_a.c").write_text("void sub_0055BDF8(void)\n{\n}\n")
        (root / "recomp_b.c").write_text(
            "void other_function(void)\n{\n"
            "    SET_HI8(eax, g_fp_cmp); /* lahf */\n}\n")

    def snapshot(self, root):
        return {p.name: p.read_bytes() for p in root.iterdir()}

    def test_input_only_and_idempotent(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            other = (root / "recomp_b.c").read_bytes()
            compat.patch(root)
            self.assertIn("sub_0055BDF8_gen", (root / "recomp_a.c").read_text())
            self.assertEqual(other, (root / "recomp_b.c").read_bytes())
            before = self.snapshot(root)
            compat.patch(root)
            self.assertEqual(before, self.snapshot(root))

    def test_missing_input_rejected_without_writes(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            (root / "recomp_a.c").write_text("void unrelated(void) {}\n")
            before = self.snapshot(root)
            with self.assertRaises(ValueError):
                compat.patch(root)
            self.assertEqual(before, self.snapshot(root))

    def test_previous_graphics_patch_requires_clean_regeneration(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self.fixture(root)
            with (root / "recomp_b.c").open("a") as stream:
                stream.write("/* CLX_WINDOWS_INPUT_COMPAT_V1 */\n")
            before = self.snapshot(root)
            with self.assertRaises(ValueError):
                compat.patch(root)
            self.assertEqual(before, self.snapshot(root))


if __name__ == "__main__":
    unittest.main()
