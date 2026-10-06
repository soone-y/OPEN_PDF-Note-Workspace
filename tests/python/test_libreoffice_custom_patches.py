from pathlib import Path
import re
import unittest


PATCH_DIR = Path(__file__).resolve().parents[2] / "third_party/libreoffice/custom_build/patches"


class LibreOfficeCustomPatchTests(unittest.TestCase):
    def test_generated_skia_patch_is_complete_and_keeps_official_declaration(self):
        outer = (PATCH_DIR / "0044-fix-skia-interface-macro-on-windows.patch").read_text(encoding="utf-8")
        marker = "+++ b/external/skia/windows-avoid-interface-macro.patch.1\n"
        creation = outer.split(marker, 1)[1].splitlines()
        count = re.fullmatch(r"@@ -0,0 \+1,(\d+) @@", creation[0])
        self.assertIsNotNone(count)
        additions = creation[1:]
        self.assertTrue(all(line.startswith("+") for line in additions))
        self.assertEqual(len(additions), int(count.group(1)), "Outer patch must not truncate its generated patch")
        nested = [line[1:] for line in additions]
        self.assertEqual(nested[:2], [
            "--- a/src/gpu/vk/vulkanmemoryallocator/VulkanAMDMemoryAllocator.cpp",
            "+++ b/src/gpu/vk/vulkanmemoryallocator/VulkanAMDMemoryAllocator.cpp",
        ])
        hunk = re.match(r"@@ -\d+,(\d+) \+\d+,(\d+) @@", nested[2])
        self.assertIsNotNone(hunk)
        body = nested[3:]
        self.assertTrue(all(line.startswith((" ", "+", "-")) for line in body))
        self.assertEqual(sum(line.startswith((" ", "-")) for line in body), int(hunk.group(1)))
        self.assertEqual(sum(line.startswith((" ", "+")) for line in body), int(hunk.group(2)))
        self.assertNotIn("VulkanAMDMemoryAllocator.h", "\n".join(nested))
        self.assertEqual(sum(line.startswith("+") and "vinterface" in line for line in body), 3)
        self.assertNotIn("vkInterface", "\n".join(nested))

    def test_native_windows_skia_binary_mode_preserves_msys_condition(self):
        patch = (PATCH_DIR / "0048-use-binary-skia-patches-on-native-windows.patch").read_text(encoding="utf-8")
        self.assertIn("+ifneq ($(strip $(MSYSTEM)$(filter WNT,$(OS))),)", patch)
        self.assertIn("gb_UnpackedTarball_set_patchflags,skia,--binary", patch)

    def test_non_gnu_jpeg_visibility_is_empty_not_exported(self):
        patch = (PATCH_DIR / "0049-define-libjpeg-hidden-on-non-gnu-compilers.patch").read_text(encoding="utf-8")
        self.assertIn('+#else\n+#define HIDDEN\n', patch)
        self.assertIn(' #define HIDDEN  __attribute__((visibility("hidden")))', patch)
        self.assertNotIn("dllexport", patch)

    def test_md4c_backport_is_complete_and_preserves_old_reference_ownership(self):
        outer = (PATCH_DIR / "0050-backport-md4c-allocation-failure-cleanup.patch").read_text(encoding="utf-8")
        self.assertIn("+$(eval $(call gb_UnpackedTarball_set_patchlevel,md4c,1))", outer)
        marker = "+++ b/external/md4c/allocation-failure-cleanup.patch.1\n"
        creation = outer.split(marker, 1)[1].splitlines()
        count = re.fullmatch(r"@@ -0,0 \+1,(\d+) @@", creation[0])
        self.assertIsNotNone(count)
        self.assertTrue(all(line.startswith("+") for line in creation[1:]))
        self.assertEqual(len(creation) - 1, int(count.group(1)))
        nested = [line[1:] for line in creation[1:]]
        hunks = [i for i, line in enumerate(nested) if line.startswith("@@ ")]
        self.assertEqual(len(hunks), 4)
        for index, start in enumerate(hunks):
            match = re.match(r"@@ -\d+,(\d+) \+\d+,(\d+) @@", nested[start])
            self.assertIsNotNone(match)
            end = hunks[index + 1] if index + 1 < len(hunks) else len(nested)
            body = nested[start + 1:end]
            self.assertTrue(all(line.startswith((" ", "+", "-")) for line in body))
            self.assertEqual(sum(line.startswith((" ", "-")) for line in body), int(match.group(1)))
            self.assertEqual(sum(line.startswith((" ", "+")) for line in body), int(match.group(2)))
        self.assertIn("+            ret = -1;", nested)
        self.assertIn("+    if(build->substr_types != build->trivial_types) {", nested)
        self.assertEqual(sum("+" == line[:1] and "MD_CHECK(md_end_current_block(ctx));" in line for line in nested), 2)
        self.assertFalse(any(line.startswith("-") and "free(def->" in line for line in nested))
        self.assertNotIn("MD4C_USE_UTF16", "\n".join(nested))


if __name__ == "__main__":
    unittest.main()
