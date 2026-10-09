"""Execute the production setup JSON helpers in an isolated Windows fixture."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

REPO_ROOT = Path(__file__).resolve().parents[2]


def production_sections(source: str) -> str:
    # Exact function boundaries: fail if production code moves, never test a copy.
    ranges = [
        ("static std::atomic<long> g_saveOperationCount{0};", "static std::atomic<uint64_t> g_editRevision{0};"),
        ("void EnterSaveOperation() {", "void NotifyEditRevisionChanged() {"),
        ("static bool IsSymlinkPath(", "static VerifiedThemeMeta* FindVerifiedMetaByFile("),
        ("static bool ReadTextFileUtf8Limited(", "static bool ReadThemeFile("),
        ("std::string WideToUTF8(", "namespace {\nconstexpr std::uint64_t kMaxBuildInfoManifestBytes"),
        ("static std::string ReadTextFileUtf8(const std::filesystem::path& p) {", "static std::optional<bool> QuerySystemTouchpadInvertVertical("),
        ("static bool ParseJsonStringToken(", "static bool WorkspaceJsonHasUnknownTopLevelFields("),
        ("static bool SetupJsonHasUnknownTopLevelFields(", "static std::wstring SetupJsonAutoUpdateBlockedReason("),
        ("[[nodiscard]] static std::filesystem::path QuarantineCorruptSetupJson(", "static std::wstring WorkspaceConfigRootKeyForCompare("),
        ("static setup_json_policy::AutoUpdateDecision ResolveSetupJsonAutoUpdateDecision(", "static bool ReadExistingSetupJsonForAutoUpdate("),
        ("// Locate only a top-level value,", "static const char* ToolModeKey("),
        ("static std::filesystem::path ResolveSetupJsonPath(", "static bool WriteSetupJsonFile("),
        ("std::vector<std::wstring> LoadSetupTempExternalLectureDirs() {", "bool PersistSetupTempExternalLectureDirs("),
        ("static bool ReplaceOrInsertJsonArrayField(std::string& json,", "static bool RemoveJsonScalarOrArrayField("),
        ("struct VerifiedThemeMeta {", "static std::wstring g_themeCurrentFile;"),
        ("static bool IsSafeThemeFileId(", "static std::filesystem::path ThemeFilePathFromId("),
        ("static std::optional<COLORREF> ParseJsonColorField(", "static bool ParseThemeBlock("),
        ("static void WriteThemeObject(std::ostream& os, const std::string& indent, const ThemeColors& theme) {", "static std::string ReadTextFileUtf8(const std::filesystem::path& p);"),
        ("static void WorkspaceSafeDirs(", "static void WriteThemeObject(std::ostream& os, const std::string& indent, const ThemeColors& theme) {"),
        ("constexpr int kScheduleMaxDays = 7;", "static std::optional<int> ParseJsonIntField("),
        ("static std::vector<VerifiedThemeMeta> g_themeVerified;", "static std::wstring g_themeLastDisplayEn;"),
        ("static void WriteThemeConfig(", "void SetPaletteCustomColor("),
    ]
    sections = []
    for start, end in ranges:
        assert source.count(start) == 1, start
        first = source.index(start)
        last = source.index(end, first)
        sections.append(source[first:last])
    return "\n".join(sections)


class SetupJsonRoundTripTests(unittest.TestCase):
    def test_save_restart_update_and_original_retention(self) -> None:
        compiler = shutil.which("g++")
        if os.name != "nt" or not compiler:
            self.fail("Windows and g++ are required for the setup persistence regression")
        root = REPO_ROOT / "out/tests/setup_json"
        root.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=root, prefix="roundtrip_") as temporary:
            fixture = Path(temporary)
            app = fixture / "app"
            app.mkdir()
            generated = fixture / "production_setup_helpers.cpp"
            preamble = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include "core/atomic_write.h"
#include "diagnostics/normal_operations.h"
#include "core/text_encoding.h"
#include "core/json_string.h"
#include "core/theme_types.h"
#include "core/workspace_config.h"
#include "core/setup_json_policy.h"
static constexpr std::uint64_t kMaxSetupJsonBytes = 256ull * 1024ull;
static std::optional<std::string> ParseJsonStringField(const std::string&, const std::string&);
static bool ReplaceOrInsertJsonStringField(std::string&, const std::string&, const std::string&);
void EnterSaveOperation();
void LeaveSaveOperation();
'''
            source = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8")
            header = (REPO_ROOT / "src/core/app_core.h").read_text(encoding="utf-8")
            guard = header[header.index("struct SaveOperationGuard {"):
                           header.index("void InstallGlobalBeepFilter();")]
            importer = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8")
            importer_helpers = importer[importer.index("static std::string TrimAsciiForSettingsBundle("):
                                        importer.index("static bool EquivalentSettingsPathText(")]
            viewer = (REPO_ROOT / "src/readonly_viewer/main.cpp").read_text(encoding="utf-8")
            viewer_helpers = viewer[viewer.index("std::string EscapeJsonString("):
                                    viewer.index("bool JsonBoolMember(")]
            checks = (REPO_ROOT / "tests/unit/setup_json_tests.cpp").read_text(encoding="utf-8")
            generated.write_text(preamble + guard + production_sections(source) + importer_helpers + viewer_helpers + checks, encoding="utf-8")
            executable = app / "setup_json_tests.exe"
            subprocess.run([
                compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-static-libgcc", "-static-libstdc++",
                "-Isrc", str(generated), "-o", str(executable),
            ], cwd=REPO_ROOT, check=True, timeout=90)
            environment = dict(os.environ)
            environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment["PATH"]
            for mode in ("write", "read", "update", "read", "strings", "reject", "remove", "empty"):
                subprocess.run([str(executable), mode], env=environment, check=True, timeout=20)
                saved = json.loads((app / "pdf_note_workspace_setup.json").read_text(encoding="utf-8"))
                self.assertEqual(saved["readonlyViewer"], {"sentinel": "keep $& ]"})
                self.assertEqual(saved["annotToolModeOrder"], ["select", "pan"])
                if mode == "empty":
                    self.assertEqual(saved["tempExternalLectureDirs"], [])
                if mode == "strings":
                    schedule = json.loads((app / "__pdf_note_workspace__/__settings__/schedule.json").read_text(encoding="utf-8"))
                    self.assertIn('"', schedule["scheduleCells"][0])
                    self.assertIn("\n", schedule["scheduleCells"][0])
                    theme = json.loads((app / "theme_fixture.json").read_text(encoding="utf-8"))
                    self.assertIn('"', theme["verified"][0]["display"])
            subprocess.run([str(executable), "recovery"], env=environment, check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
