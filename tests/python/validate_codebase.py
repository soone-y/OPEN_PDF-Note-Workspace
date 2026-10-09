from __future__ import annotations

import json
import py_compile
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
SOURCE_DIRS = ("src", "tests", "tools", "scripts")
CPP_SOURCE_EXTENSIONS = {
    ".c", ".cc", ".cpp", ".cxx", ".c++",
    ".h", ".hh", ".hpp", ".hxx", ".ipp", ".inl",
    ".inc", ".cppinc",
}
GENERATED_SOURCE_PREFIXES = (("tests", "bin"),)

RISKY_FS_CALL_RE = re.compile(
    r"std::filesystem::(?P<name>exists|is_directory|create_directories|rename|copy_file|remove|relative|weakly_canonical|last_write_time|current_path)\s*\("
)
SAFE_WITH_EC_RE = re.compile(r",\s*[A-Za-z_]\w*\s*\)")
SAFE_CURRENT_PATH_RE = re.compile(r"std::filesystem::current_path\(\s*[A-Za-z_]\w*\s*\)")
RAW_WIN32_TIMER_ID_RE = re.compile(
    r"\b(?:SetTimer|KillTimer)\s*\([^,\n]+,\s*(?:0x[0-9A-Fa-f]+|\d+)\b"
)
FIXED_MAX_PATH_BUFFER_RE = re.compile(
    r"\b(?:wchar_t|WCHAR)\s+\w+\s*\[\s*MAX_PATH(?:\s*[+\-]\s*\d+)?\s*\]"
)
COMMAND_ID_ENUM_RE = re.compile(r"enum\s+CommandId\s*:\s*int\s*\{(?P<body>.*?)\n\};", re.DOTALL)
INT_CONSTANT_RE_TEMPLATE = r"\b(?:inline\s+)?constexpr\s+int\s+{name}\s*=\s*(?P<value>-?(?:0x[0-9A-Fa-f]+|\d+))\s*;"

BASELINE_ALLOWED_THROWING_FS = {
    "src/core/app_core.cpp:if (std::filesystem::exists(setup)) return true;",
    "src/core/app_core.cpp:if (std::filesystem::exists(wsjson)) {",
    "src/core/app_core.cpp:if (!std::filesystem::exists(setup)) {",
    "src/core/app_core.cpp:if (!std::filesystem::exists(setup)) return result;",
    "src/core/app_core.cpp:if (std::filesystem::exists(setup)) {",
    "src/core/app_core.cpp:if (!std::filesystem::exists(wsjson)) return;",
    "src/help/help.cpp:if (len == 0 || len == MAX_PATH) return std::filesystem::current_path();",
    "src/help/help.cpp:candidates.push_back(std::filesystem::current_path() / L\"docs\" / fileName);",
    "src/help/help.cpp:candidates.push_back(std::filesystem::current_path() / L\"開発ドキュメント\" / L\"ヘルプについて.txt\");",
    "src/pdf_view/navigation.cpp:if (!std::filesystem::exists(path)) return true;",
    "src/search/search.cpp:if (!TryGetCurrentSessionRoot(sessionPath) || !std::filesystem::exists(sessionPath)) {",
    "src/search/search.cpp:if (!TryGetCurrentLectureRoot(lecturePath) || !std::filesystem::exists(lecturePath)) {",
}


def compile_python_files() -> list[str]:
    errors: list[str] = []
    for source_dir in SOURCE_DIRS:
        root = REPO_ROOT / source_dir
        if not root.exists():
            continue
        for path in sorted(root.rglob("*.py")):
            rel = path.relative_to(REPO_ROOT).as_posix()
            if "__pycache__" in rel:
                continue
            try:
                py_compile.compile(str(path), doraise=True)
            except py_compile.PyCompileError as exc:
                errors.append(f"{rel}: {exc.msg}")
    return errors


def find_windows_powershell_encoding_violations() -> list[str]:
    """Require a BOM for PowerShell 5.1 scripts containing non-ASCII source."""
    errors: list[str] = []
    scripts_root = REPO_ROOT / "tests" / "scripts"
    if not scripts_root.exists():
        return errors

    for path in sorted(scripts_root.rglob("*.ps1")):
        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8-sig")
        except UnicodeDecodeError:
            continue
        if any(ord(character) > 0x7F for character in text) and not raw.startswith(b"\xef\xbb\xbf"):
            errors.append(
                f"{path.relative_to(REPO_ROOT).as_posix()}: non-ASCII PowerShell source must use a UTF-8 BOM for Windows PowerShell 5.1"
            )
    return errors


def iter_source_files() -> list[Path]:
    files: list[Path] = []
    for source_dir in SOURCE_DIRS:
        root = REPO_ROOT / source_dir
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() not in CPP_SOURCE_EXTENSIONS:
                continue
            relative_parts = path.relative_to(REPO_ROOT).parts
            if any(relative_parts[: len(prefix)] == prefix for prefix in GENERATED_SOURCE_PREFIXES):
                continue
            if "third_party/pdfium" in path.as_posix():
                continue
            files.append(path)
    return files


def iter_win32_id_source_files() -> list[Path]:
    files: list[Path] = []
    for source_dir in SOURCE_DIRS:
        root = REPO_ROOT / source_dir
        if not root.exists():
            continue
        for path in root.rglob("*"):
            if not path.is_file():
                continue
            if path.suffix.lower() not in CPP_SOURCE_EXTENSIONS:
                continue
            relative_parts = path.relative_to(REPO_ROOT).parts
            if any(relative_parts[: len(prefix)] == prefix for prefix in GENERATED_SOURCE_PREFIXES):
                continue
            if "third_party/pdfium" in path.as_posix():
                continue
            files.append(path)
    return files


def find_new_throwing_filesystem_calls() -> list[str]:
    errors: list[str] = []
    for path in iter_source_files():
        rel = path.relative_to(REPO_ROOT).as_posix()
        lines = path.read_text(encoding="utf-8", errors="ignore").splitlines()
        for index, raw_line in enumerate(lines):
            line = raw_line.strip()
            if not line or line.startswith("//") or not RISKY_FS_CALL_RE.search(line):
                continue

            # Filesystem calls are frequently formatted across several lines. Inspect
            # the complete call so an explicit std::error_code on the final line is
            # not misreported as a throwing overload.
            call_text = line
            cursor = index
            while ");" not in call_text and cursor + 1 < len(lines) and cursor - index < 8:
                cursor += 1
                call_text += " " + lines[cursor].strip()

            if SAFE_WITH_EC_RE.search(call_text) or SAFE_CURRENT_PATH_RE.search(call_text):
                continue
            key = f"{rel}:{line}"
            if key in BASELINE_ALLOWED_THROWING_FS:
                continue
            errors.append(f"{rel}:{index + 1}: {line}")
    return errors


def find_raw_win32_timer_ids() -> list[str]:
    errors: list[str] = []
    for path in iter_win32_id_source_files():
        rel = path.relative_to(REPO_ROOT).as_posix()
        with path.open("r", encoding="utf-8", errors="ignore") as f:
            for lineno, raw_line in enumerate(f, start=1):
                line = raw_line.strip()
                if not line or line.startswith("//"):
                    continue
                if RAW_WIN32_TIMER_ID_RE.search(line):
                    errors.append(f"{rel}:{lineno}: {line}")
    return errors


def find_long_path_contract_violations() -> list[str]:
    """Keep the executable and its build/test paths long-path aware."""
    errors: list[str] = []
    manifest = REPO_ROOT / "src" / "resources" / "app.manifest"
    resource = REPO_ROOT / "src" / "resources" / "app.rc"
    workspace_build = REPO_ROOT / "scripts" / "build" / "build_workspace.ps1"
    viewer_build = REPO_ROOT / "scripts" / "build" / "build_readonly_viewer.ps1"
    atomic_test = REPO_ROOT / "tests" / "scripts" / "run_atomic_write_tests.ps1"

    required_files = (manifest, resource, workspace_build, viewer_build, atomic_test)
    if any(not path.is_file() for path in required_files):
        return ["long-path contract: required manifest, build, or test file is missing"]

    manifest_text = manifest.read_text(encoding="utf-8", errors="ignore")
    if "longPathAware" not in manifest_text or ">true<" not in manifest_text:
        errors.append("src/resources/app.manifest: longPathAware=true is required")
    if '1 24 "src/resources/app.manifest"' not in resource.read_text(encoding="utf-8", errors="ignore"):
        errors.append("src/resources/app.rc: application manifest must be embedded as resource 1/24")
    for path in (workspace_build, viewer_build):
        if "src/resources/app.manifest" not in path.read_text(encoding="utf-8", errors="ignore"):
            errors.append(f"{path.relative_to(REPO_ROOT).as_posix()}: manifest must be a tracked resource input")
    test_text = atomic_test.read_text(encoding="utf-8", errors="ignore")
    if "atomic_write_tests_resource.o" not in test_text or "windres" not in test_text:
        errors.append("tests/scripts/run_atomic_write_tests.ps1: atomic-write test must link the long-path manifest")

    for path in iter_source_files():
        rel = path.relative_to(REPO_ROOT).as_posix()
        for line_number, raw in enumerate(path.read_text(encoding="utf-8", errors="ignore").splitlines(), start=1):
            if FIXED_MAX_PATH_BUFFER_RE.search(raw):
                errors.append(f"{rel}:{line_number}: fixed MAX_PATH path buffer is forbidden; use a growing buffer")
    return errors


def parse_int_constant(text: str, name: str) -> int | None:
    pattern = re.compile(INT_CONSTANT_RE_TEMPLATE.format(name=re.escape(name)))
    match = pattern.search(text)
    if not match:
        return None
    return int(match.group("value"), 0)


def parse_command_ids() -> tuple[list[tuple[str, int]], list[str]]:
    path = REPO_ROOT / "src" / "core" / "command_ids.h"
    text = path.read_text(encoding="utf-8", errors="ignore")
    match = COMMAND_ID_ENUM_RE.search(text)
    if not match:
        return [], ["src/core/command_ids.h: CommandId enum not found"]

    values: list[tuple[str, int]] = []
    errors: list[str] = []
    current: int | None = None
    for raw_line in match.group("body").splitlines():
        line = raw_line.split("//", 1)[0].strip().rstrip(",")
        if not line:
            continue
        if "=" in line:
            name, expr = [part.strip() for part in line.split("=", 1)]
            try:
                current = int(expr, 0)
            except ValueError:
                errors.append(f"src/core/command_ids.h: unsupported CommandId expression for {name}: {expr}")
                continue
        else:
            name = line
            if current is None:
                errors.append(f"src/core/command_ids.h: first CommandId has no explicit value: {name}")
                continue
            current += 1
        values.append((name, current))
    return values, errors


def find_forbidden_ui_strings() -> list[str]:
    forbidden = {
        "キーボードソフト操作": "ソフトキーボード操作",
        "Windows選択を使う": "Windowsファイル選択を使う",
        "マーカー: テキスト": "テキストマーカー",
        "マーカー(テキスト)": "テキストマーカー",
        "マーカー（テキスト）": "テキストマーカー",
        "マーカー(フリー)": "フリーハンドマーカー",
        "ペン設定": "線設定",
        "補正ペン": "線補正",
        "一時ペン": "一時線",
        "Correction Pen": "Line correction",
        "Pen style": "Line style",
    }
    errors: list[str] = []
    src_root = REPO_ROOT / "src"
    for path in sorted(src_root.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix.lower() not in CPP_SOURCE_EXTENSIONS:
            continue
        rel = path.relative_to(REPO_ROOT).as_posix()
        text = path.read_text(encoding="utf-8", errors="ignore")
        for old, new in forbidden.items():
            if old in text:
                errors.append(f"{rel}: forbidden UI string {old!r}; use {new!r}")
    return errors

def find_command_id_collisions() -> list[str]:
    errors: list[str] = []
    command_ids, parse_errors = parse_command_ids()
    errors.extend(parse_errors)
    if not command_ids:
        return errors

    by_value: dict[int, list[str]] = {}
    for name, value in command_ids:
        by_value.setdefault(value, []).append(name)
    for value, names in sorted(by_value.items()):
        if len(names) > 1:
            errors.append(f"duplicate CommandId value {value}: {', '.join(names)}")

    header = (REPO_ROOT / "src" / "core" / "command_ids.h").read_text(encoding="utf-8", errors="ignore")
    palette_capacity = parse_int_constant(header, "kToolPaletteCommandSlotCapacity")
    if palette_capacity is None:
        errors.append("src/core/command_ids.h: kToolPaletteCommandSlotCapacity not found")
        return errors
    if palette_capacity <= 0:
        errors.append(f"src/core/command_ids.h: invalid kToolPaletteCommandSlotCapacity: {palette_capacity}")
        return errors

    id_lookup = dict(command_ids)
    palette_base = id_lookup.get("ID_TOOL_COLOR_BASE")
    if palette_base is None:
        errors.append("src/core/app_core.h: ID_TOOL_COLOR_BASE not found")
        return errors

    palette_end = palette_base + palette_capacity - 1
    for name, value in command_ids:
        if name == "ID_TOOL_COLOR_BASE":
            continue
        if palette_base <= value <= palette_end:
            errors.append(
                f"CommandId {name}={value} collides with dynamic ID_TOOL_COLOR_BASE "
                f"range {palette_base}..{palette_end}"
            )
    return errors

def find_font_combo_display_regressions() -> list[str]:
    errors: list[str] = []
    path = REPO_ROOT / "src" / "core" / "font_list.h"
    text = path.read_text(encoding="utf-8", errors="ignore")
    required = {
        'kFontPreviewSample = L"日本語名: 日本語サンプル / 表示例: あア漢 Aa123"':
            "font combo preview must expose a Japanese font-name slot and Japanese sample text",
        'kFontNoJapanesePreviewLabel = L"日本語なし"':
            "font combo must explicitly mark fonts without Japanese glyph coverage",
        'case FontStyle::Latin:':
            "font combo must label Latin fonts instead of treating them as unknown",
        'case FontStyle::Symbol:':
            "font combo must label symbol fonts instead of treating them as unknown",
        'FontStyleHasJapaneseGlyphs':
            "font combo must distinguish Japanese-capable fonts from Latin/symbol fonts",
        'preview.replace(pos, target.size(), jpName);':
            "font combo must replace the generic Japanese sample marker with the concrete Japanese label",
    }
    for needle, message in required.items():
        if needle not in text:
            errors.append(f"src/core/font_list.h: {message}")
    return errors

def find_textbox_position_regressions() -> list[str]:
    errors: list[str] = []
    path = REPO_ROOT / "src" / "pdf_view" / "navigation.cppinc"
    text = path.read_text(encoding="utf-8", errors="ignore")
    expected = "static constexpr int kTextClickLeftOffsetPx = 2;"
    if expected not in text:
        errors.append(
            "src/pdf_view/navigation.cppinc: kTextClickLeftOffsetPx must stay at 2px "
            "so new text annotations are not placed too far left of the click point"
        )
    return errors


def find_rotated_ellipse_rotation_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/core/annot_types.h": {
            "kDefaultRotatedEllipseAngleRad": "legacy rotated_ellipse data needs a stable fallback angle",
            "double shapeRotation": "Annotation must persist a per-shape rotation angle",
        },
        "src/pdf_view/pdf_view.cpp": {
            "ShapeRotationFromClientEndpoints": "interactive rotated ellipse drawing must derive angle from drag endpoints",
            "BuildRotatedEllipseOutlinePointsPx(const RECT& r, double angleRad)": "viewer rendering must accept a stored angle",
            "EffectiveRotatedEllipseAngleRad": "viewer rendering must sanitize missing or invalid rotated ellipse angles",
        },
        "src/pdf_view/input.cppinc": {
            "ann.shapeRotation = ShapeRotationFromClientEndpoints": "committed rotated ellipse annotations must store the drag angle",
        },
        "src/pdf_view/annotation_store.cppinc": {
            '"shape_rotation"': "annotation history JSON must persist rotated ellipse angle",
            "kDefaultRotatedEllipseAngleRad": "legacy annotation history without shape_rotation must remain readable",
        },
        "src/clrop/types.h": {
            "shapeRotation": "CLROP item model must carry rotated ellipse angle",
        },
        "src/clrop/json.cpp": {
            'shape_rotation': "CLROP serializer must write rotated ellipse angle",
        },
        "src/clrop/json_direct_parser.inc": {
            'key == "shape_rotation"': "CLROP parser must read rotated ellipse angle",
        },
        "src/file_output/file_output.cpp": {
            "BuildRotatedEllipseOutlinePointsPx(const RECT& rc, double angleRad)": "bitmap export must render stored rotated ellipse angle",
            "pdfAngle = rotated ? -visualAngle : 0.0": "PDF export must account for PDF/client Y-axis direction",
        },
        "src/readonly_viewer/pdf_preview_panel.cpp": {
            "item.shapeRotation": "readonly viewer must render CLROP rotated ellipse angle",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors


def find_textbox_vertical_writing_mode_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/core/annot_types.h": {
            "enum class TextWritingMode { Horizontal, VerticalRl };": "Annotation must define a stable TextBox writing-mode enum",
            "TextWritingMode writingMode = TextWritingMode::Horizontal": "Annotation must default TextBox writing mode to horizontal for legacy data",
        },
        "src/pdf_view/annotation_store.cppinc": {
            "TextWritingModeToString": "annotation history JSON must serialize TextBox writing mode",
            "TextWritingModeFromString": "annotation history JSON must parse TextBox writing mode",
            "\"writing_mode\"": "annotation history JSON must use the writing_mode key",
            "if (a.type == Annotation::Type::TextBox)": "annotation history JSON must scope writing_mode to TextBox",
            "out.writingMode = TextWritingMode::Horizontal": "legacy annotation history without writing_mode must read as horizontal",
            "if (out.type != Annotation::Type::TextBox)": "non-TextBox annotations must not inherit writing_mode",
            "if (a.writingMode != b.writingMode || a.backgroundAssistMode != b.backgroundAssistMode) return false;": "annotation merge comparison must not ignore writing-mode changes",
        },
        "src/clrop/types.h": {
            "std::string writingMode": "CLROP item model must carry TextBox writing mode",
        },
        "src/clrop/bridge.cpp": {
            "TextWritingModeToClrop": "CLROP bridge must serialize TextBox writing mode",
            "TextWritingModeFromClrop": "CLROP bridge must parse TextBox writing mode",
            "item.writingMode = TextWritingModeToClrop(a.writingMode);": "TextBox-to-CLROP conversion must preserve writing mode",
            "a.writingMode = TextWritingModeFromClrop(item.writingMode);": "CLROP-to-TextBox conversion must restore writing mode",
            "item.writingMode.clear();": "MathBox CLROP conversion must not leak TextBox writing mode",
        },
        "src/clrop/json.cpp": {
            "writing_mode": "CLROP serializer must write TextBox writing mode",
            "it.writingMode": "CLROP serializer must read writing mode from the item model",
        },
        "src/clrop/json_direct_parser.inc": {
            "key == \"writing_mode\"": "CLROP parser must read TextBox writing mode",
            "item.writingMode": "CLROP parser must store writing mode in the item model",
        },
        "src/file_output/file_output_stage.cpp": {
            "if (a.writingMode != b.writingMode)": "stage comparison must not ignore writing-mode changes",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors


def find_annotation_summary_regressions() -> list[str]:
    errors: list[str] = []
    panel = REPO_ROOT / "src" / "ui" / "dialogs" / "annot_math_panel.cpp"
    panel_text = panel.read_text(encoding="utf-8", errors="ignore")
    forbidden = {
        'L"Total: "': "annotation panel summary must not show aggregate total text",
        'L"合計: "': "annotation panel summary must not show aggregate total text",
        'L"\\r\\nSelected: "': "annotation panel summary must not show selected-item duplicate text",
        'L"\\r\\n選択: "': "annotation panel summary must not show selected-item duplicate text",
        'L"\\r\\nSelected: none"': "annotation panel summary must not show selected-none text",
        'L"\\r\\n選択: なし"': "annotation panel summary must not show selected-none text",
    }
    for needle, message in forbidden.items():
        if needle in panel_text:
            errors.append(f"src/ui/dialogs/annot_math_panel.cpp: {message}")

    layout = REPO_ROOT / "src" / "ui" / "layout.cpp"
    layout_text = layout.read_text(encoding="utf-8", errors="ignore")
    required_layout = {
        "const int panelSummaryH = 20;":
            "annotation panel layout must reserve a compact save-status row",
        "const int panelListMinH = 48;":
            "annotation panel layout must reserve visible annotation-list rows",
        "status and list are the":
            "annotation panel layout must prioritize recovery controls over optional actions",
        "addPos(g_hAnnotSummary, x, y, w, panelSummaryH);":
            "annotation panel layout must place the save-status row",
        "addPos(g_hAnnotList, x, y, w, listH);":
            "annotation panel list must consume the remaining panel height",
    }
    for needle, message in required_layout.items():
        if needle not in layout_text:
            errors.append(f"src/ui/layout.cpp: {message}")
    return errors


def find_debug_resource_monitor_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/core/command_ids.h": {
            "ID_DEBUG_RESOURCE_MONITOR": "developer-only resource monitor command ID must exist",
        },
        "src/ui/menus/menu_build.cpp": {
            "if (menuState.developerMode)": "debug menu must remain developer-mode gated",
            "ID_DEBUG_RESOURCE_MONITOR": "debug menu must expose the resource monitor only inside the debug menu",
            "DebugResourceMonitorMenuLabel()": "debug menu must use the resource monitor label helper",
        },
        "src/app/command_dispatch.cppinc": {
            "case ID_DEBUG_RESOURCE_MONITOR:": "resource monitor command must be handled",
            "if (!g_config.developerMode) break;": "resource monitor command must remain developer-mode gated",
            "BuildDebugResourceMonitorText()": "resource monitor command must build a local snapshot text",
        },
        "src/ui/menus/main_debug_menu.cpp": {
            "DebugResourceMonitorMenuLabel": "resource monitor label helper must exist",
            "BuildDebugResourceMonitorText": "resource monitor snapshot builder must exist",
            "No data is sent or written.": "resource monitor must state that data is not sent or written",
            "GlobalMemoryStatusEx": "resource monitor must use local Win32 memory status only",
            "GetGuiResources": "resource monitor must expose local GDI/USER object counts",
            "GetProcessTimes": "resource monitor must expose local process timing",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors

def find_annotation_shortcut_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/core/app_core.cpp": {
            'token == "UP" || token == "ARROWUP"': "shortcut parser must accept Ctrl+Up for color cycling/config validation",
            'token == "DOWN" || token == "ARROWDOWN"': "shortcut parser must accept Ctrl+Down for color cycling/config validation",
            'token.rfind("NUMPAD", 0) == 0': "shortcut parser must accept numpad keys explicitly",
            "ShortcutKeyAllowsNoModifier": "plain shortcuts must be limited to the approved no-modifier key class",
            "IsFixedAnnotToolNavigationShortcut": "fixed annotation navigation keys must be reserved from user bindings",
            "shapeDetail": "canonical Shape detail state must persist",
            "ShapeDetailForLegacyState": "legacy Shape selection must migrate to canonical detail state",
            '{ "Numpad7", AnnotToolShortcutTargetKind::Detail, AnnotToolFamily::Pen, ToolMode::Freehand }': "default annotation shortcuts must include standalone numpad tool switching",
            "std::set<UINT> chordSeen": "shortcut loading must reject duplicate chords",
        },
        "src/pdf_view/document_io.cppinc": {
            'ShouldPromptForPdfPassword()': "startup-driven PDF opens must not enter a password dialog loop",
            'password_prompt_suppressed_for_startup_open': "suppressed automatic password opens must leave trace evidence",
            'ShouldPromptForPdfPassword() && reloadPromptCount++ < 3': "startup-driven PDF failures must not open retry dialogs",
        },
        "src/note_view/note_view_note_ops.cppinc": {
            'ShouldPromptForPdfPassword()': "startup-driven note recovery must not open a modal dialog",
            'ShouldNotifyDocumentOpenLockFailure()': "startup-driven note errors must remain silent",
        },
        "src/main.cpp": {
            "HandleAnnotColorCycleShortcutInLoop": "main loop must handle Ctrl+Up/Ctrl+Down annotation color cycling",
            "HandleFixedAnnotToolNavigationShortcutInLoop": "fixed annotation category/detail navigation shortcut must remain available",
            "ShouldSuppressAnnotToolShortcutForFocus(msg)": "annotation shortcuts must be suppressed for text/IME focus",
            "MainLoopShortcutMessageBelongsToOwner": "main-loop shortcuts must ignore other root windows",
            "OrderedShapeDetails": "Shape detail selector must follow configured detail order",
        },
        "src/app/bootstrap.cppinc": {
            "HandleAnnotColorCycleShortcutInLoop(hWnd, msg)": "color cycling shortcut must be called before generic annotation tool shortcut handling",
            "HandleFixedAnnotToolNavigationShortcutInLoop(hWnd, msg)": "fixed annotation navigation shortcut must be called from the main loop",
        },
        "src/app/command_dispatch.cppinc": {
            "ToolOptionCycleReverseRequested": "Shift+click reverse cycling policy must remain centralized for toolbar commands",
            "OrderedShapeDetails": "Shape navigation must follow configured detail order",
        },
        "src/pdf_view/input.cppinc": {
            "BeginTextBoxEditClickSelection(hwnd, pt, shiftDown)": "PDF-surface Shift+click must remain available for text-box selection",
        },
        "src/help/help.cpp": {
            "Ctrl+↑ / Ctrl+↓": "help must document the color cycling shortcut",
            "テンキー単独": "help must document standalone numpad annotation shortcuts",
            "IME変換中": "help must document that IME composition suppresses shortcuts",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors

def find_palette_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/core/app_core.cpp": {
            "user_palette.json": "palette colors must remain backed by the local user palette file",
            "SaveUserPaletteColorsForSettings": "settings palette save path must remain implemented",
            "ApplyActivePaletteColorStep": "toolbar/PDF color cycling must use the shared runtime palette stepper",
            "ToolbarHasColorOptions(g_toolMode)": "palette cycling must only run for tools that expose color options",
            "StoreToolColorForMode(g_toolMode, g_activeColor)": "palette selection must persist the selected color to the active tool",
            "PersistConfig()": "palette/tool color changes must be persisted",
            "RedrawWindow(g_hPdfToolbar": "palette changes must redraw the toolbar buttons",
        },
        "src/settings/settings_palette.cppinc": {
            "DrawPaletteSlotButton": "settings dialog must draw visible palette slots",
            "SetPaletteSettingsColor": "settings dialog must allow directly editing a palette slot color",
            "ShowPaletteColorEditorDialog": "palette editing must use the application-owned dialog",
        },
        "src/settings/settings_annot.cppinc": {
            "SaveUserPaletteColorsForSettings": "settings dialog must save changed palette colors",
        },
        "src/features/automation/main_ui_automation.cppinc": {
            "RunUiAutomationPaletteScenario": "UI automation must cover palette display, selection, persistence, and cycling",
            "#0C2238": "UI automation must verify the saved palette file content",
            "ID_TOOL_COLOR_BASE + 2": "UI automation must verify toolbar command selection for a palette slot",
            "automation:palette_ok": "UI automation trace must record successful palette coverage",
        },
        "src/app/command_dispatch.cppinc": {
            "ID_TOOL_COLOR_BASE": "toolbar palette command dispatch must remain wired",
            "StoreToolColorForMode(g_toolMode, g_activeColor)": "toolbar palette selection must store the color for the active tool",
            "PersistConfig()": "toolbar palette selection must persist config",
        },
        "src/pdf_view/input.cppinc": {
            "ApplyActivePaletteColorStep": "PDF-view Ctrl+Up/Ctrl+Down must use the shared palette stepper",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors


def find_search_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/search/search.h": {
            "ToggleSearchWindow": "Ctrl+F/menu search command must be able to close an open search window",
            "ShowSearchWindowWithPreset": "preset search must continue to open/focus the search window without toggling it closed",
        },
        "src/search/search.cpp": {
            "IDC_SEARCH_OPTION_NORMALIZE": "search UI must expose width/kana/separator normalization as an option",
            "IDC_SEARCH_OPTION_IGNORE_CASE": "search UI must expose case-insensitive matching as an option",
            "CurrentSearchOptions": "search execution must read the current search option checkboxes",
            "note::SemanticSearchOptions options": "search matching must pass explicit normalization options through note/PDF/annotation paths",
            "ToggleSearchWindow": "search window must implement Ctrl+F/menu toggling",
            "IsCtrlFKeyDown": "search child controls must recognize Ctrl+F as close",
            "HideSearchResultAt": "search results must keep the temporary hide-from-results path",
            "VK_DELETE": "Delete key must remain wired to hide a search result",
            "WS_EX_LAYERED": "search window must support translucent display",
            "SetLayeredWindowAttributes": "search translucency must be applied without sound or external services",
        },
        "src/app/command_dispatch.cppinc": {
            "ToggleSearchWindow(hWnd)": "ID_SEARCH must toggle rather than only opening the search window",
        },
        "src/note/note_semantic_index.h": {
            "SemanticSearchOptions": "semantic search normalization must be configurable",
        },
        "src/note/note_semantic_index.cpp": {
            "options.normalizeWidthKana": "semantic search must honor width/kana normalization option",
            "options.ignoreCase": "semantic search must honor case sensitivity option",
            "options.ignoreSeparators": "semantic search must honor separator normalization option",
        },
        "tests/unit/note_parser_tests.cpp": {
            "semantic search options can disable width, case, kana, and separator normalization": "unit tests must cover disabling search normalization",
            "semantic search options can require case-sensitive matches": "unit tests must cover case-sensitive search",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors


def find_textbox_font_sync_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/ui/dialogs/annot_math_panel.cpp": {
            "SyncTextBoxToolFontFromAnnotationIndex": "selected TextBox annotation may sync font family but must not overwrite font-size slots",
            "Selecting a TextBox must not overwrite the annotation toolbar A/B size slots": "TextBox selection must not treat the annotation font size as the source of truth",
            "SyncToolbarFontSizeCombo()": "TextBox selection must still refresh toolbar display after allowed font-family changes",
            "PersistConfig()": "TextBox selection must persist only allowed text-tool setting changes",
        },
        "src/ui/core/main_window_proc.cppinc": {
            "SyncTextBoxToolFontFromAnnotationIndex(sel)": "annotation-list selection must sync TextBox font settings",
        },
        "src/pdf_view/input.cppinc": {
            "GetSelectedTextBoxAnnotationIndex": "Ctrl+/- must target the selected TextBox annotation outside inline editing",
            "ApplyActiveTextToolFontToSelectedTextBox": "toolbar font-size changes must update the selected TextBox, not the toolbar slots",
            "after.fontPt = ResolveAdaptiveTextFontPtForPage(after.pageIndex, g_textFontPt);": "selected TextBox must receive the active tool font size with page scaling",
            "ChangeSelectedTextBoxFont": "selected TextBox font size must be adjustable without entering edit mode",
            "RecordAnnotUpdate(idx, idx, before, after)": "selected TextBox font changes must be undoable as annotation updates",
            "ReflowMovedTextBox(after)": "selected TextBox font changes must remeasure the box geometry",
            "SyncTextBoxToolFontFromAnnotationIndex(hitIdx)": "clicking an existing TextBox must not overwrite toolbar font-size slots",
            "ChangeSelectedTextBoxFont(hwnd, true)": "Ctrl+Plus must adjust a selected TextBox before falling back to PDF zoom",
            "ChangeSelectedTextBoxFont(hwnd, false)": "Ctrl+Minus must adjust a selected TextBox before falling back to PDF zoom",
        },
        "src/pdf_view/interaction_overlay.cppinc": {
            "const double editFontPt = ResolveAdaptiveTextFontPtForPage(ann.pageIndex, g_textFontPt);": "editing an existing TextBox must update it from the active tool font size instead of changing toolbar options",
            "BeginTextEdit(hwnd, ann.pageIndex": "existing TextBox edit start must still enter the normal edit path",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors


def find_operation_feedback_regressions() -> list[str]:
    errors: list[str] = []
    
    # 1. Check RefreshNoteOverlayNow checks s_noteOverlayRefreshTargetRevision != CurrentEditRevision
    main_window_proc = REPO_ROOT / "src/ui/core/main_window_proc.cppinc"
    if main_window_proc.exists():
        content = main_window_proc.read_text(encoding="utf-8", errors="ignore")
        if "s_noteOverlayRefreshTargetRevision != CurrentEditRevision()" not in content:
            errors.append(f"{main_window_proc.name}: RefreshNoteOverlayNow must check s_noteOverlayRefreshTargetRevision != CurrentEditRevision()")
            
    # 2. WM_ENDSESSION must preserve recovery data without integrating into an
    # original. Windows can terminate the process immediately after it returns.
    if main_window_proc.exists():
        content = main_window_proc.read_text(encoding="utf-8", errors="ignore")
        end_session_start = content.find("case WM_ENDSESSION:")
        end_session_end = content.find("case WM_DESTROY:", end_session_start)
        end_session_block = content[end_session_start:end_session_end]
        if (end_session_start >= 0 and
                "PreserveUnsavedChangesForSystemEndSession" not in end_session_block):
            errors.append(f"{main_window_proc.name}: WM_ENDSESSION must preserve recoverable staged changes")
        if "RunSaveAndIntegrateTransaction" in end_session_block:
            errors.append(f"{main_window_proc.name}: WM_ENDSESSION must not integrate staged data into originals")
        if "CancelOfficeConversionJobsForExit" not in end_session_block:
            errors.append(f"{main_window_proc.name}: WM_ENDSESSION must terminate Office conversion jobs")

    # 3. The read-only viewer must cancel an in-flight load and atomically
    # snapshot its persistent session when Windows ends the session.
    readonly_main = REPO_ROOT / "src/readonly_viewer/main.cpp"
    if readonly_main.exists():
        content = readonly_main.read_text(encoding="utf-8", errors="ignore")
        end_session_start = content.find("case WM_ENDSESSION:")
        end_session_end = content.find("case WM_CLOSE:", end_session_start)
        end_session_block = content[end_session_start:end_session_end]
        if (end_session_start < 0 or
                "g_loadCancelRequested = true" not in end_session_block or
                "SaveReadonlySession" not in end_session_block):
            errors.append(f"{readonly_main.name}: WM_ENDSESSION must cancel loading and preserve the persistent session")
        if ("void EndCancellableLoad()" not in content or
                "PostMessageW(g_hwndMain, WM_CLOSE" not in content):
            errors.append(f"{readonly_main.name}: a user close during loading must resume after cancellation")
            
    # 4. Check CommitActiveNoteEditBoundary validates g_currentNotePath
    note_ops = REPO_ROOT / "src/note_view/note_view_note_ops.cppinc"
    if note_ops.exists():
        content = note_ops.read_text(encoding="utf-8", errors="ignore")
        if "void CommitActiveNoteEditBoundary" in content and "g_currentNotePath.empty()" not in content:
            errors.append(f"{note_ops.name}: CommitActiveNoteEditBoundary must validate g_currentNotePath.empty()")

    return errors


def find_runtime_safety_regressions() -> list[str]:
    errors: list[str] = []
    office_path = REPO_ROOT / "src/workspace/workspace_actions.cpp"
    office_text = office_path.read_text(encoding="utf-8", errors="ignore")
    required_office = {
        "ValidateOfficePackageForOfflineConversion": "Office conversion must reject unsafe package relationships before launch",
        "RunLibreOfficePdfConversion": "Office conversion must use the verified LibreOffice runtime",
    }
    for needle, message in required_office.items():
        if needle not in office_text:
            errors.append(f"src/workspace/workspace_actions.cpp: {message}")
    handoff_contract = (
        "bool ValidateLibreOfficeHandoffPathBudget",
        "ValidateLibreOfficeHandoffPathBudget(soffice, sourceOfficeCopy, outDir, profileDir, outErr)",
        "{L\"LibreOffice\", soffice}",
        "{L\"profile cache\", profileProbe}",
    )
    if any(needle not in office_text for needle in handoff_contract):
        errors.append(
            "src/workspace/workspace_actions.cpp: LibreOffice handoff paths must be validated before process launch"
        )
    if office_text.count("ValidateLibreOfficeHandoffPathBudget(soffice, sourceOfficeCopy, outDir, profileDir, outErr)") < 2:
        errors.append(
            "src/workspace/workspace_actions.cpp: both foreground and background LibreOffice launches must validate handoff paths"
        )
    background_conversion_contract = (
        "JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE",
        "AssignProcessToJobObject",
        "kMaxParallelOfficeConversions = 2",
        "std::thread(RunOfficeBackgroundJob, raw).detach()",
        "kMsgOfficeConversionWorkerComplete",
    )
    if any(needle not in office_text for needle in background_conversion_contract):
        errors.append("src/workspace/workspace_actions.cpp: Office conversion must use bounded background workers and a kill-on-close Job Object")
    for forbidden in ("Word.Application", "PowerPoint.Application", "Excel.Application", "Workbooks.Open",
                      '$ext -match "xls"', '$ext -eq ".xlsx"', '$ext -eq ".xls"'):
        if forbidden in office_text:
            errors.append(f"src/workspace/workspace_actions.cpp: MS Office automation must not be implemented ({forbidden})")
    if 'return ext == L".docx" || ext == L".pptx";' not in office_text:
        errors.append("src/workspace/workspace_actions.cpp: Office import policy must allow only .docx and .pptx")

    atomic_text = (REPO_ROOT / "src/core/atomic_write.h").read_text(encoding="utf-8", errors="ignore")
    atomic_quarantine_contract = {
        "MoveFileExW(sourcePath.c_str(), destinationPath.c_str(), MOVEFILE_WRITE_THROUGH)":
            "quarantine moves must never replace an existing destination",
        "CopyFileW(sourcePath.c_str(), destinationPath.c_str(), TRUE)":
            "quarantine copy fallback must fail if the destination appeared concurrently",
    }
    for needle, message in atomic_quarantine_contract.items():
        if needle not in atomic_text:
            errors.append(f"src/core/atomic_write.h: {message}")

    readonly_text = (REPO_ROOT / "src/readonly_viewer/pdf_preview_panel.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    readonly_atomic_contract = {
        '#include "core/atomic_write.h"': "read-only viewer writes must use the shared atomic-write implementation",
        "atomic_write::AtomicWriteBytes(": "read-only viewer settings must use collision-safe atomic writes",
        "atomic_write::CreateUniqueTempFile(": "read-only viewer PDF export must reserve a unique temp file",
        "atomic_write::AtomicReplaceFile(": "read-only viewer PDF export must use the shared replacement policy",
    }
    for needle, message in readonly_atomic_contract.items():
        if needle not in readonly_text:
            errors.append(f"src/readonly_viewer/pdf_preview_panel.cpp: {message}")
    for forbidden in ('temp += L".tmp";', 'temp += std::to_wstring(GetCurrentProcessId())'):
        if forbidden in readonly_text:
            errors.append("src/readonly_viewer/pdf_preview_panel.cpp: predictable CREATE_ALWAYS temp paths are forbidden")

    image_text = (REPO_ROOT / "src/pdf_view/view_state.cppinc").read_text(encoding="utf-8", errors="ignore")
    image_limit_contract = {
        "kMaxDecodedImageBytes": "decoded image memory must have a fixed upper bound",
        "TryComputeBgraLayout": "WIC image dimensions and buffer arithmetic must be checked before allocation",
        "sourceBytes > kMaxDecodedImageBytes": "WIC scaling must reject oversized source buffers",
        "bytes > static_cast<uint64_t>(std::numeric_limits<UINT>::max())":
            "WIC buffer sizes must fit the API's UINT byte-count parameter",
    }
    for needle, message in image_limit_contract.items():
        if needle not in image_text:
            errors.append(f"src/pdf_view/view_state.cppinc: {message}")

    integrity_gate_text = (REPO_ROOT / "tools/release_checks/release_set_integrity_gate.py").read_text(
        encoding="utf-8", errors="ignore"
    )
    startup_gate_text = (REPO_ROOT / "tools/release_checks/release_startup_smoke_gate.py").read_text(
        encoding="utf-8", errors="ignore"
    )
    release_zip_bounds = {
        "release_set_integrity_gate.py": (
            integrity_gate_text,
            ("zip_path.stat().st_size > expected_bytes + archive_overhead_limit", "info.file_size != expected_size", "extracted_size > expected_size", "with archive.open(info) as member"),
        ),
        "release_startup_smoke_gate.py": (
            startup_gate_text,
            ("archive_path.stat().st_size > expected_bytes + archive_overhead_limit", "len(infos) > len(expected) + len(allowed_directories)", "written > expected_size", "unexpected ZIP file entry"),
        ),
    }
    for relative_path, (text, needles) in release_zip_bounds.items():
        if any(needle not in text for needle in needles):
            errors.append(f"tools/release_checks/{relative_path}: release ZIP processing must remain bounded by unpacked release contents")

    annotation_history_text = (REPO_ROOT / "src/pdf_view/annotation_store.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    annotation_history_contract = (
        "kMaxAnnotHistoryJsonBytes",
        "kMaxAnnotHistoryJsonDepth",
        "ReadFileBytesNoFollowLimitedWin32(path, kMaxAnnotHistoryJsonBytes, json)",
        "MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS",
        "std::from_chars(number.data(), number.data() + number.size(), val)",
        "parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size()",
        "!std::isfinite(val)",
        "if (!closed) return false;",
    )
    if any(needle not in annotation_history_text for needle in annotation_history_contract):
        errors.append("src/pdf_view/annotation_store.cppinc: annotation-history JSON must remain bounded and strictly parsed")
    path_safety_text = (REPO_ROOT / "src/core/path_safety.h").read_text(encoding="utf-8", errors="ignore")
    no_follow_read_contract = (
        "ReadFileBytesNoFollowLimitedWin32",
        "FILE_FLAG_OPEN_REPARSE_POINT",
        "FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT",
    )
    if any(needle not in path_safety_text for needle in no_follow_read_contract):
        errors.append("src/core/path_safety.h: bounded sensitive-file reads must reject reparse points on the opened handle")

    proc_path = REPO_ROOT / "src/ui/core/main_window_proc.cppinc"
    proc_text = proc_path.read_text(encoding="utf-8", errors="ignore")
    case_start = proc_text.find("case kMsgRunUiAutomation:")
    case_end = proc_text.find("case kMsgReloadLectureList:", case_start)
    automation_case = proc_text[case_start:case_end] if case_start >= 0 and case_end > case_start else ""
    if not automation_case or "RunUiAutomationScenarios" not in automation_case:
        errors.append("src/ui/core/main_window_proc.cppinc: UI automation message must execute its scenarios")
    if "#ifndef NDEBUG" in automation_case or "#ifdef _DEBUG" in automation_case:
        errors.append("src/ui/core/main_window_proc.cppinc: explicitly enabled UI automation must also run in test Release builds")
    main_text = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8", errors="ignore")
    automation_include = '#include "features/automation/main_ui_automation.cppinc"'
    include_pos = main_text.find(automation_include)
    if include_pos < 0:
        errors.append("src/main.cpp: UI automation implementation must be included in test Release builds")
    else:
        guard_window = main_text[max(0, include_pos - 80):include_pos]
        if "#ifndef NDEBUG" in guard_window or "#ifdef _DEBUG" in guard_window:
            errors.append("src/main.cpp: UI automation implementation must not be debug-build-only")
    return errors


def find_textbox_commit_layout_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/pdf_view/navigation.cppinc": {
            "kTextBoxGlyphOverhangPadPx = 2": "TextBox measurement must keep a right-edge glyph overhang allowance",
        },
        "src/pdf_view/pdf_view.cpp": {
            "NormalizeTextBoxTextForCommit": "TextBox commit/save must normalize trailing CR/LF before persisting text",
            "const std::wstring committedText = NormalizeTextBoxTextForCommit(g_pdf.editText);": "active edit save snapshots must use normalized TextBox text",
            "g_pdf.editText = NormalizeTextBoxTextForCommit(g_pdf.editText);": "TextBox commit must trim trailing CR/LF from the editing buffer",
            "g_pdf.editCaret = std::min(g_pdf.editCaret, g_pdf.editText.size());": "TextBox commit must clamp caret after trimming trailing CR/LF",
            "ann.text = committedText;": "active edit save snapshots must persist normalized TextBox text",
            "FinalizeTextBoxAnnotationLayout(ann);": "TextBox commit/save must finalize layout through one shared reflow path",
            "ann.textLines.clear();": "TextBox finalization must discard stale line caches before reflowing",
            "ReflowMovedTextBox(ann);": "TextBox finalization must remeasure width and height after text/font changes",
            "maxLineW + kTextBoxGlyphOverhangPadPx + padPx * 2": "TextBox edit and saved layout measurement must include right-edge glyph overhang padding",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    pdf_text = (REPO_ROOT / "src/pdf_view/pdf_view.cpp").read_text(encoding="utf-8", errors="ignore")
    if pdf_text.count("FinalizeTextBoxAnnotationLayout(ann);") < 2:
        errors.append("src/pdf_view/pdf_view.cpp: TextBox commit and active edit save snapshot must both use shared layout finalization")
    if pdf_text.count("maxLineW + kTextBoxGlyphOverhangPadPx + padPx * 2") < 2:
        errors.append("src/pdf_view/pdf_view.cpp: both edit-time and saved TextBox measurement must include glyph overhang padding")
    if "LayoutTextLinesWithFont(innerWidth, committedText" in pdf_text or "LayoutTextLinesWithFont(innerWidth, g_pdf.editText" in pdf_text:
        errors.append("src/pdf_view/pdf_view.cpp: TextBox commit paths must not precompute separate line caches before final reflow")
    return errors
def find_annotation_text_quality_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/pdf_view/navigation.cppinc": {
            "P3-6: annotation TextBox glyphs are rendered in their own high-DPI raster cache": "TextBox quality intent must stay documented next to the DPI constants",
            "kTextBoxStableRasterDpi = 576.0": "TextBox annotation glyphs must keep a high-DPI raster baseline independent of page cache resolution",
            "kTextBoxLayoutDpi = kTextBoxStableRasterDpi": "TextBox layout must use the same stable DPI as the quality raster cache",
            "kPdfRenderMinScale = 0.75": "PDF page rendering may use its own low-resolution cache policy without lowering annotation text DPI",
        },
        "src/pdf_view/pdf_view.cpp": {
            "BuildTextBoxRaster": "TextBox annotations must have an annotation-owned raster path",
            "CreateDIBSection": "TextBox annotation text must render into a separate 32bpp bitmap before compositing",
            "fontPt * kTextBoxStableRasterDpi / 72.0": "TextBox font pixels must be derived from the stable annotation DPI, not the current PDF page cache scale",
            "lf.lfQuality = ANTIALIASED_QUALITY": "TextBox glyph rasterization must keep antialiased quality in the high-DPI cache",
            "AlphaBlend(hdc, r.left, r.top, w, h, scratchDc": "TextBox glyphs must composite as an overlay instead of being baked into the PDF page bitmap",
            "TextBoxLowZoomBoostAlpha": "Low zoom TextBox rendering must preserve legibility rather than relying only on downscaled page pixels",
        },
        "src/pdf_view/interaction_overlay.cppinc": {
            "HDC scratchDc = CreateCompatibleDC(hdc);": "annotation drawing must allocate a separate scratch DC for overlay text composition",
            "DrawTextBoxStable(hdc, scratchDc, r, ann);": "TextBox annotations must use the stable overlay renderer from DrawAnnotations",
        },
        "src/pdf_view/render.cppinc": {
            "DrawAnnotations(memDC, paint);": "annotations must be drawn after PDF page bitmaps so annotation text stays separate from page cache pixels",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    render_text = (REPO_ROOT / "src/pdf_view/render.cppinc").read_text(encoding="utf-8", errors="ignore")
    overlay_pos = render_text.find("DrawAnnotations(memDC, paint);")
    page_overlay_pos = render_text.find("DrawReadableTextOverlay(memDC, paint);")
    if overlay_pos < 0 or page_overlay_pos < 0 or overlay_pos < page_overlay_pos:
        errors.append("src/pdf_view/render.cppinc: annotation overlay must be drawn after readable text/page overlays")
    return errors

def find_annotation_layer_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/pdf_view/interaction_overlay.cppinc": {
            "static void DrawAnnotations(HDC hdc, const RECT& client)": "annotation viewer drawing must stay centralized in DrawAnnotations",
            "if (ann.type == Annotation::Type::TextColor)": "TextColor annotations must have a distinct viewer drawing branch",
            "TintRectTextPixels(hdc, qr, ann.color": "TextColor selection quads must recolor text pixels instead of filling an opaque rectangle",
            "TintRectTextPixels(hdc, r, ann.color": "TextColor fallback rectangles must still use text-pixel tinting",
            "} else if (ann.type == Annotation::Type::MarkerText)": "MarkerText annotations must have a distinct viewer drawing branch",
            "FillRectAlphaMaskedByText(hdc, qr, dark": "MarkerText selection quads must be masked by PDF text pixels",
            "kMarkerTextAlphaScaleOnText": "MarkerText must keep reduced alpha on glyph pixels so text remains readable",
            "ann.type == Annotation::Type::MarkerFree || ann.type == Annotation::Type::Freehand": "free marker drawing must stay separate from text marker drawing",
            "DarkenColor(ann.color, 0.85)": "free/text markers must keep the darkened viewer color policy",
            "DrawDashedArrowLinePx": "arrow annotations must preserve dash-aware antialiased viewer drawing",
            "DrawDashedLinePx": "line annotations must preserve dash-aware antialiased viewer drawing",
            "if (std::isfinite(partPt) && partPt > 0.0)": "viewer dash normalization must reject nonfinite/nonpositive render segments",
            "if (pattern.size() == 1) pattern.push_back(pattern.front());": "viewer must normalize a single dash in its temporary rendering pattern",
        },
        "src/file_output/file_output.cpp": {
            "enum class AnnotationRenderMode": "export rendering must keep viewer-like and PDF-like annotation render modes explicit",
            "static void DrawAnnotationsToBuffer": "PNG export must draw annotations through the shared raster export path",
            "case Annotation::Type::TextColor:": "export raster path must handle TextColor annotations explicitly",
            "TintRectTextPixels(hdc, rc, ann.color": "PNG TextColor export must tint text pixels, not opaque rectangles",
            "case Annotation::Type::MarkerText:": "export raster path must handle MarkerText annotations explicitly",
            "FillRectAlphaMaskedByText(hdc, rc, col": "PNG MarkerText export must preserve text-masked alpha fill",
            "AppendDashedPdfSegment": "PDF export for line/arrow annotations must preserve dash segments",
            'localization::Text(L"file_output.text_color_export_unsupported")': "PDF export must use the localized TextColor rejection notice",
            "static bool RejectUnsupportedTextColorPdfExport": "PDF export must centralize its unsupported TextColor preflight",
        },
        "src/pdf_view/annotation_store.cppinc": {
            "case Annotation::Type::MarkerText: return \"marker_text\";": "MarkerText must remain serializable to .clrop",
            "case Annotation::Type::TextColor: return \"text_color\";": "TextColor must remain serializable to .clrop",
            "case Annotation::Type::MarkerFree: return \"marker_free\";": "MarkerFree must remain serializable to .clrop",
            "if (s == \"marker_text\") { out = Annotation::Type::MarkerText; return true; }": "MarkerText must remain loadable from .clrop",
            "if (s == \"text_color\") { out = Annotation::Type::TextColor; return true; }": "TextColor must remain loadable from .clrop",
            "if (s == \"marker_free\") { out = Annotation::Type::MarkerFree; return true; }": "MarkerFree must remain loadable from .clrop",
            "oss << \",\\\"dash\\\":[\";": "dash pattern must be persisted to .clrop",
            'GetNumberArrayField(v, "dash", out.dash)': "journal loading must preserve dash entries and their count",
            "return AnnotationJsonNumbersFinite(out);": "journal loading must reject nonfinite dash data without normalizing saved values",
        },
        "src/file_output/file_output_stage.cpp": {
            "if (a.dash.size() != b.dash.size())": "staged annotation comparison must include dash pattern length",
            "if (!AnnotationCompareNearlyEqual(a.dash[i], b.dash[i]))": "staged annotation comparison must include dash pattern values",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")

    file_output = (REPO_ROOT / "src/file_output/file_output.cpp").read_text(encoding="utf-8", errors="ignore")
    export_impl_pos = file_output.find("static bool ExportPdfPagesImpl")
    reject_pos = file_output.find("if (RejectUnsupportedTextColorPdfExport(owner, pages)) return false;", export_impl_pos)
    write_pos = file_output.find("ExportPdfDocumentWithSpecs(doc", export_impl_pos)
    if export_impl_pos < 0 or reject_pos < 0 or write_pos < 0 or not (reject_pos < write_pos):
        errors.append("src/file_output/file_output.cpp: TextColor PDF export must be rejected before destination PDF writing starts")
    if file_output.count("if (RejectUnsupportedTextColorPdfExport(owner, pages)) return false;") < 3:
        errors.append("src/file_output/file_output.cpp: TextColor PDF export must be rejected before selecting a destination and in the write path")
    return errors

def find_pdf_textbox_visibility_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/pdf_view/navigation.cppinc": {
            "EnsureEditingTextBoxVisible": "PDF TextBox editing must have a dedicated visibility helper",
            "kEditVisibleMarginPx = 24": "PDF TextBox editing visibility must keep a viewport margin",
            "GetEditingTextBoxClientRect(&r)": "visibility helper must measure the active edit box in client coordinates",
            "ClampPdfScroll(viewH, viewW)": "visibility helper must clamp scroll and update scrollbars after moving the viewport",
            "PdfTextBoxEditVisibility": "visibility helper must be traceable for field diagnosis",
            "InvalidateRect(hwnd, nullptr, FALSE);": "visibility helper must repaint after automatic scrolling",
        },
        "src/pdf_view/annotation_edit.cppinc": {
            "EnsureEditingTextBoxVisible(hwnd, L\"begin_text_edit\")": "editing an existing TextBox must reveal the box after initial measurement",
            "EnsureEditingTextBoxVisible(hwnd, L\"undo_text_edit\")": "TextBox undo must keep the resized edit box visible",
            "EnsureEditingTextBoxVisible(hwnd, L\"redo_text_edit\")": "TextBox redo must keep the resized edit box visible",
        },
        "src/pdf_view/input.cppinc": {
            "EnsureEditingTextBoxVisible(hwnd, L\"new_text_box\")": "new TextBox creation must reveal the edit area",
            "EnsureEditingTextBoxVisible(hwnd, L\"text_input\")": "ordinary character input must reveal a growing edit box",
            "EnsureEditingTextBoxVisible(hwnd, L\"ime_composition\")": "IME composition must reveal a growing edit box before candidate positioning",
            "EnsureEditingTextBoxVisible(hwnd, L\"ime_endcomposition\")": "IME end composition must keep the final edit box visible",
            "EnsureEditingTextBoxVisible(hwnd, L\"editing_font_enlarge\")": "editing font enlargement must reveal the resized edit box",
            "EnsureEditingTextBoxVisible(hwnd, L\"editing_font_shrink\")": "editing font shrink must keep the edit box visible",
            "EnsureEditingTextBoxVisible(hwnd, L\"textbox_font_update\")": "toolbar font update must reveal the resized edit box",
            "EnsureEditingTextBoxVisible(hwnd, L\"paste_text\")": "pasted text must reveal a growing edit box",
            "EnsureEditingTextBoxVisible(hwnd, L\"cut_text\")": "cut text must keep the edit box repaint/IME path consistent",
            "EnsureEditingTextBoxVisible(hwnd, L\"delete_key\")": "delete key edits must keep the edit box repaint/IME path consistent",
            "EnsureEditingTextBoxVisible(hwnd, L\"caret_move\")": "keyboard caret moves must keep IME candidate positioning in visible view coordinates",
        },
        "src/pdf_view/interaction_overlay.cppinc": {
            "EnsureEditingTextBoxVisible(hwnd, L\"ctx_cut_text\")": "context-menu cut must keep TextBox visibility handling consistent",
            "EnsureEditingTextBoxVisible(hwnd, L\"ctx_paste_text\")": "context-menu paste must reveal a growing edit box",
        },
        "src/pdf_view/zoom_flow.cppinc": {
            "EnsureEditingTextBoxVisible(pdfWnd, L\"zoom_edit_box\")": "zoom-time TextBox remeasure must keep the active edit box visible",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    input_text = (REPO_ROOT / "src/pdf_view/input.cppinc").read_text(encoding="utf-8", errors="ignore")
    ime_pos = input_text.find('EnsureEditingTextBoxVisible(hwnd, L"ime_composition")')
    update_pos = input_text.find("UpdateImeWindowPosition(hwnd, true);", ime_pos)
    if ime_pos < 0 or update_pos < 0 or ime_pos > update_pos:
        errors.append("src/pdf_view/input.cppinc: IME composition visibility must run before IME candidate window positioning")
    return errors

def find_annotation_save_timing_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/file_output/file_output_stage.cpp": {
            "TraceAnnotStageTiming": "annotation stage save must keep a dedicated per-step timing helper",
            "StageSaveAnnotTiming": "annotation stage save timing must use a stable preview_trace area",
            "after_strong_validation": "annotation stage save must time strong validation separately",
            "after_journal_append": "annotation stage save must time journal append separately",
            "after_prepare_snapshot": "annotation stage save must time snapshot reuse/preparation separately",
            "after_match_persisted": "annotation stage save must time persisted-state comparison separately",
            "after_checkpoint": "annotation stage save must time clrop checkpoint writing separately",
            "after_discard_journal": "annotation stage save must time journal cleanup separately",
            "prepared->annotations.size()": "annotation save timing must record the prepared annotation count",
            "CurrentEditRevision()": "annotation save timing must record the edit revision for slow-save correlation",
            "TryAppendPendingAnnotJournal": "annotation stage save must keep the journal fast path visible to timing checks",
            "StageAnnotationCheckpointWithData": "annotation stage save must keep the checkpoint path visible to timing checks",
        },
        "src/pdf_view/annotation_store.cppinc": {
            "PdfSaveAnnotationsIfDirty": "outer annotation save wrapper must remain traceable",
            "SaveAnnotHistoryIfNeeded()": "annotation history save must remain separable from stage annotation save timing",
            "file_output::SaveAnnotationsIfDirty(owner)": "outer annotation save wrapper must delegate to staged annotation save",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    stage_text = (REPO_ROOT / "src/file_output/file_output_stage.cpp").read_text(encoding="utf-8", errors="ignore")
    ordered_steps = [
        "after_strong_validation",
        "after_journal_append",
        "after_prepare_snapshot",
        "after_match_persisted",
        "after_checkpoint",
    ]
    previous = -1
    for step in ordered_steps:
        pos = stage_text.find(step)
        if pos < 0:
            continue
        if pos < previous:
            errors.append("src/file_output/file_output_stage.cpp: annotation save timing steps must stay in save-order sequence")
            break
        previous = pos
    return errors

def find_existing_pdf_annotation_policy_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/pdf_view/annotation_store.cppinc": {
            "LoadAnnotationsForCurrentPdf": "application annotations must be loaded through the app-side annotation store, not from native PDF annotations",
            "clrop_bridge::ClropPathForPdf(g_pdf.path)": "application annotations must remain stored beside the PDF as .clrop data",
            "clrop_bridge::LoadAnnotations(clropPath, g_pdf.path": "loading application annotations must read .clrop data for the current PDF",
            "ApplyLoadedAnnotationsForCurrentPdf": "loaded application annotations must be applied to g_annots only",
            "TryRecoverStagedAnnotationsForCurrentPdf": "unintegrated annotation changes must be recoverable from staged .clrop files",
            "file_output::SaveAnnotationsIfDirty(owner)": "application annotation saves must go through staged file_output save logic",
        },
        "src/file_output/file_output_stage.cpp": {
            "EnsureFastLoadedAnnotationsStrongValidatedBeforeSave": "annotation stage save must strongly validate fast-loaded .clrop state before writing",
            "StageAnnotationCheckpointWithData": "dirty application annotations must be checkpointed to staged .clrop files",
            "meta.destPath = std::filesystem::path(clrop_bridge::ClropPathForPdf(pdfPath));": "annotation stage target must be .clrop, not the original PDF",
            "MatchAnnotationSnapshotToPersistedState": "annotation save must compare against persisted .clrop/stage state before writing",
        },
        "src/file_output/file_output.cpp": {
            "CurrentLogicalPdfAnnotations()": "PDF export must read application annotations from the logical app annotation list",
            "FPDF_CreateNewDocument()": "PDF export must create a new destination document",
            "dest = pdf_annotation_export::CreateDocument()": "annotated PDF export must create an empty output copy with valid default font resources",
            "FPDF_ImportPages(dest, srcDoc": "PDF export must copy pages into the destination document before adding app annotations",
            "AddAnnotationsToPage(annots": "PDF export must add only application annotations to the destination copy",
            "pdf_annotation_export::Finalize(page)": "PDF output must finalize annotation references before saving, without depending on preview rendering",
            "pdf_annotation_export::CanScaleExisting(page, exportScale)": "native annotated input must reject unsupported non-unit scaling before saving",
            "PickSavePath(owner, GetUiText().menuExportPdf.c_str()": "PDF export must require an explicit output path",
            "IsSamePath(std::filesystem::path(outPath), std::filesystem::path(CurrentLogicalPdfPath()))": "PDF export must reject overwriting the currently opened original PDF path",
            "WarnExportOverwriteOriginal(owner, /*isPdf=*/true);": "attempted original overwrite must produce a quiet warning instead of replacing the source PDF",
            "atomic_write::AtomicReplaceFile(dest, tmp": "PDF export output must be written via atomic replace at the chosen destination",
        },
        "src/core/app_core.cpp": {
            "const std::vector<Annotation>* CurrentLogicalPdfAnnotations()": "logical PDF annotations must expose the app-side annotation list",
            "return &g_annots;": "logical PDF annotations must not alias native PDF annotation objects",
        },
        "src/file_output/pdf_annotation_export.cpp": {
            "FPDFPage_CreateAnnot(destination, subtype)": "native annotations must be created only on the output destination",
            "destination == scratch": "the output and scratch page must not alias",
            "Annotation::Type::TextColor) return false": "the annotation writer must reject original-text recoloring",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")

    forbidden_native_pdf_annotation_writes = [
        "FPDFPage_RemoveAnnot",
        "FPDFPage_CreateAnnot",
        "FPDFPage_TransformAnnots",
        "FPDFAnnot_Set",
        "FPDFAnnot_Append",
        "FPDFAnnot_UpdateObject",
        "FPDFAnnot_AddInkStroke",
        "FPDFAnnot_RemoveInkList",
    ]
    source_roots = [REPO_ROOT / "src"]
    for root in source_roots:
        for path in root.rglob("*"):
            if path.suffix.lower() not in {".cpp", ".h", ".cppinc", ".inc"}:
                continue
            rel = path.relative_to(REPO_ROOT).as_posix()
            text = path.read_text(encoding="utf-8", errors="ignore")
            for needle in forbidden_native_pdf_annotation_writes:
                if needle in text:
                    # Explicit output-copy policy: this isolated writer is used
                    # only with a newly created destination, never the source PDF.
                    if rel == "src/file_output/pdf_annotation_export.cpp":
                        continue
                    errors.append(f"{rel}: native PDF annotation mutation API must not be used without an explicit read-only/import policy: {needle}")
    return errors


def find_png_export_quality_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/file_output/file_output.h": {
            "kPdfPngDefaultDpi = 144": "PNG export must retain an explicit standard-DPI contract",
            "kPdfPngMaxDimensionPx = 16384": "PNG export must bound a single raster dimension",
            "kPdfPngMaxPixels = 32'000'000": "PNG export must bound total raster allocation",
        },
        "src/file_output/file_output.cpp": {
            "pixelCount > file_output::kPdfPngMaxPixels": "PNG export API must reject oversized rasters before allocation",
            "kPdfPngMaxDimensionPx": "PNG export API must reject oversized dimensions before allocation",
            "SavePngWic(outPath, pixels.data(), outWidthPx, outHeightPx, stride, dpi": "PNG export must preserve the calculated DPI in output metadata",
            "0, FPDF_ANNOT | FPDF_LCD_TEXT)": "PNG export must render native PDF annotations as well as the separate app layer",
        },
        "src/ui/dialogs/export_dialog.cpp": {
            "288 DPI": "PNG export UI must present a high-detail preset",
            "可逆PNG / 約 ": "PNG export UI must show format, DPI, and pixel-count guidance",
            "file_output::kPdfPngMaxPixels": "PNG export UI must use the same safe pixel limit as the export API",
        },
        "src/features/automation/main_ui_automation.cppinc": {
            "ReadPngDimensions": "PNG automation must inspect the encoded image dimensions",
            "InspectPngRasterWithWic": "PNG automation must decode the image rather than trusting its signature",
            "frame->GetResolution(&dpiX, &dpiY)": "PNG automation must verify exported DPI metadata",
            "visiblePixelCount >= 64": "PNG automation must reject an all-white raster",
            "kExportPngWidth = 1200": "PNG automation must render a meaningful high-resolution artifact",
            "exportedPngWidth != kExportPngWidth": "PNG automation must reject silently resized output",
        },
    }
    for rel, needles in required_by_file.items():
        text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors


def find_slide_crop_export_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/file_output/file_output.h": {
            "struct PdfCropRectPt": "slide crop export must carry an explicit PDF-point crop rectangle",
            "std::optional<PdfCropRectPt> cropPt": "page export specs must preserve optional crop rectangles",
        },
        "src/file_output/file_output.cpp": {
            "IsValidPdfCropRectPt": "slide crop export must validate crop rectangles against the source page",
            "ApplyPdfPageCropBoxes": "slide crop export must apply the crop to PDF page boxes",
            "FPDF_GetPageSizeByIndex(srcDoc, spec.pageIndex": "slide crop export must validate against source page dimensions before writing",
            "FPDFPage_SetMediaBox(page, left, bottom, right, top)": "slide crop export must make the exported page use the crop bounds",
            "FPDFPage_SetCropBox(page, left, bottom, right, top)": "slide crop export must set the visible CropBox",
            "ExtractPdfCropRectFromPageToken": "page-spec parsing must accept crop rectangles for manual correction/export",
            "page[left:bottom:right:top]": "invalid crop syntax must fail with a clear local message",
            "IsSamePath(std::filesystem::path(outPath), std::filesystem::path(CurrentLogicalPdfPath()))": "slide crop PDF export must keep rejecting original-path overwrite",
            "atomic_write::AtomicReplaceFile(dest, tmp": "slide crop PDF export must keep atomic destination replacement",
        },
        "src/ui/dialogs/export_dialog.cpp": {
            "1[0:0:360:540]": "export dialog must expose the manual crop-page syntax in its example text",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors

def find_note_placement_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/core/workspace_config.h": {
            "std::wstring notePlacement = L\"bottom\"": "workspace config must persist the primary note pane placement",
        },
        "src/core/app_core.h": {
            "enum class NotePlacement { Bottom, Top };": "note placement must be represented by an explicit enum",
            "ParseNotePlacement": "note placement strings must be normalized on load",
            "NotePlacementToString": "note placement must be serialized through a normalized string helper",
            "extern NotePlacement g_notePlacement;": "runtime note placement state must be declared for layout use",
        },
        "src/core/app_core.cpp": {
            "NotePlacement g_notePlacement = NotePlacement::Bottom;": "runtime note placement must default to the existing bottom layout",
            "bottomNoteMode|notePlacement|colorTone": "workspace JSON detection must recognize notePlacement",
            "\"bottomNoteMode\", \"notePlacement\"": "workspace JSON known-field validation must accept notePlacement",
            "ParseJsonStringField(json, \"notePlacement\")": "workspace loading must read notePlacement",
            "cfg.notePlacement = NotePlacementToString(ParseNotePlacement": "workspace loading/saving must normalize notePlacement",
            "g_config.notePlacement = NotePlacementToString(g_notePlacement);": "config persistence must capture runtime note placement",
        },
        "src/workspace/workspace_config_io.cpp": {
            "g_notePlacement = ParseNotePlacement(g_config.notePlacement);": "UI config application must apply notePlacement to runtime state",
            "g_config.notePlacement = NotePlacementToString(g_notePlacement);": "UI config application must normalize notePlacement after parsing",
        },
        "src/ui/layout.cpp": {
            "const bool noteAtTop = (g_notePlacement == NotePlacement::Top);": "layout must branch on top note placement",
            "addPos(g_hPdfView, c.centerX, pdfY, c.centerW, pdfH);": "PDF view must use placement-derived coordinates",
            "!noteAtTop &&\n                      g_bottomPanePin == BottomPanePin::Note": "top note placement must not merge the primary note into the bottom-right pane",
            "const bool returnHiddenBottomPaneToNote = !noteAtTop &&": "top note placement must not steal bottom-right width for the primary note",
            "addPos(g_hNoteEdit, c.centerX, noteY, noteW, noteH);": "note edit window must use placement-derived coordinates",
            "addPos(g_hNoteRender, c.centerX, noteY, noteW, noteH);": "note render window must use placement-derived coordinates",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors
def find_link_workflow_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/main.cpp": {
            "static std::wstring GenerateLinkId()": "link creation must generate stable unique link IDs",
            "LinkIdAlreadyUsed(candidate)": "link ID generation must avoid collisions with current note/PDF and persisted files",
            "static int PendingLinkPointCount()": "pending link completion must count note and PDF endpoints together",
            "RememberPendingLinkPdfPath(CurrentLogicalPdfPath())": "cross-PDF pending links must remember the PDF path containing a marker",
            "DiscardPendingLinkMarkersFromPdfPath": "canceling an incomplete cross-PDF link must remove pending markers from remembered PDF paths",
            "UpdatePendingLinkMarkerNotePathInPdfPath": "finalizing a cross-PDF link must update stored PDF marker note paths",
            "FinalizePendingLinkModeIfReady": "pending link mode must finalize only after enough endpoints exist",
            "CancelPendingLinkMode": "pending link mode must have a single cancel path",
            "PreparePendingLinkForPdfSwitch": "PDF switching while link mode is active must persist/track pending PDF markers",
            "g_linkPending.id = GenerateLinkId();": "starting link mode must allocate a link ID before placing endpoints",
        },
        "src/app/bootstrap.cppinc": {
            "if (isEscKey && g_linkPending.active)": "Esc handling must explicitly cancel pending link mode",
            "CancelPendingLinkMode(hWnd);": "Esc link cancellation must use the shared cancel path",
        },
        "src/pdf_view/annotation_store.cppinc": {
            "case Annotation::Type::LinkMarker: return \"link_marker\";": "LinkMarker annotations must remain serializable to .clrop",
            "\"link_id\"": "LinkMarker serialization must persist the link ID",
            "\"note_path\"": "LinkMarker serialization must persist the target note path",
            "if (s == \"link_marker\") { out = Annotation::Type::LinkMarker; return true; }": "LinkMarker annotations must remain loadable from .clrop",
        },
        "src/note_view/note_view_shared.cppinc": {
            "static bool HandleLinkIdJump": "note link activation must use a shared jump resolver",
            "FindPdfLinkHit(linkId, targetPdf, hit)": "link jump must be able to resolve PDF markers outside the current PDF",
            "OpenPdfWithAnnotations(owner, targetPdf)": "cross-PDF link jump must open the target PDF with annotations",
            "JumpToPdfPoint(g_hPdfView, ann.pageIndex, ann.x1, ann.y1)": "PDF link jump must navigate to the marker point",
            "JumpToNoteLinkId(linkId": "link jump must fall back to note targets when no PDF marker is found",
            "LinkIdAtCharIndex(g_hNoteEdit, caret)": "keyboard link activation must resolve the link ID under the note caret",
            "if (link_render_grace_active && line == caret_line) return false;": "a clicked link must remain rendered until its double-click is resolved",
        },
        "src/note_view/note_view_input_proc.cppinc": {
            "const bool ctrlAltClick =": "note links must support an explicit Ctrl+Alt click activation gesture",
            "if (ctrlAltClick && !g_linkPending.active)": "Ctrl+Alt link activation must not interfere with link creation mode",
            "HandleLinkIdJump(*linkId, GetParent(hWnd), g_currentNotePath, idx)": "Ctrl+Alt click must use the shared internal-link jump resolver",
        },
        "src/pdf_view/input.cppinc": {
            "RecordAnnotAdd(static_cast<int>(g_annots.size() - 1), g_annots.back());": "PDF link endpoints must enter annotation undo history",
            "MarkAnnotsDirty(GetParent(hwnd));": "PDF link endpoints must enter the recoverable annotation stage path",
            "RememberPendingLinkPdfPath(CurrentLogicalPdfPath());": "PDF link endpoints must remain tracked across a PDF switch",
            "FinalizePendingLinkModeIfReady(owner);": "PDF link completion must use the shared cross-view finalizer",
        },
        "src/pdf_view/annotation_edit.cppinc": {
            "DiscardPendingLinkAnnotHistory(linkId);": "canceling a pending PDF link must not leave undo history that resurrects an endpoint",
            "InvalidatePendingAnnotStageCommands(CurrentLogicalPdfPath());": "canceling a pending PDF link must checkpoint its removed endpoint rather than stale add commands",
            "SynchronizePendingPdfLinkEndpointAfterHistoryReplay(cmd, /*undo=*/true);": "undoing a pending PDF endpoint must reconcile the pending-link count",
            "SynchronizePendingPdfLinkEndpointAfterHistoryReplay(cmd, /*undo=*/false);": "redoing a pending PDF endpoint must reconcile the pending-link count",
        },
        "src/note_view/note_view_note_ops.cppinc": {
            "SynchronizePendingNoteLinkPointAfterUndoRedo(replayedText);": "note undo/redo must reconcile a pending link endpoint from post-replay text",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")

    main_text = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8", errors="ignore")
    start_pos = main_text.find("g_linkPending.id = GenerateLinkId();")
    finalize_pos = main_text.find("void FinalizePendingLinkModeIfReady")
    cancel_pos = main_text.find("void CancelPendingLinkMode")
    switch_pos = main_text.find("static void PreparePendingLinkForPdfSwitch")
    remember_pdf_pos = main_text.find("void RememberPendingLinkPdfPath")
    if (start_pos < 0 or finalize_pos < 0 or cancel_pos < 0 or switch_pos < 0 or
            remember_pdf_pos < 0):
        errors.append("src/main.cpp: pending link workflow must keep explicit start, finalize, cancel, and PDF-switch paths")
    pdf_input_text = (REPO_ROOT / "src/pdf_view/input.cppinc").read_text(encoding="utf-8", errors="ignore")
    pdf_link_create_pos = pdf_input_text.find("if (g_linkPending.active && !g_linkPending.id.empty()")
    pdf_link_add_pos = pdf_input_text.find("RecordAnnotAdd(", pdf_link_create_pos)
    pdf_link_dirty_pos = pdf_input_text.find("MarkAnnotsDirty(GetParent(hwnd));", pdf_link_add_pos)
    pdf_link_track_pos = pdf_input_text.find("RememberPendingLinkPdfPath(CurrentLogicalPdfPath());", pdf_link_dirty_pos)
    pdf_link_finalize_pos = pdf_input_text.find("FinalizePendingLinkModeIfReady(owner);", pdf_link_track_pos)
    if (pdf_link_create_pos < 0 or pdf_link_add_pos < pdf_link_create_pos or
            pdf_link_dirty_pos < pdf_link_add_pos or pdf_link_track_pos < pdf_link_dirty_pos or
            pdf_link_finalize_pos < pdf_link_track_pos):
        errors.append("src/pdf_view/input.cppinc: every PDF link endpoint must enter history, dirty stage tracking, and the shared finalizer in that order")
    input_text = (REPO_ROOT / "src/note_view/note_view_input_proc.cppinc").read_text(encoding="utf-8", errors="ignore")
    dblclk_pos = input_text.find("case WM_LBUTTONDBLCLK")
    pointer_hit_pos = input_text.find("ResolveNotePointerPresentationHit(", dblclk_pos)
    activation_pos = input_text.find("if (pointerHit.raw_index.has_value())", dblclk_pos)
    jump_pos = input_text.find("HandleLinkIdJump(*linkId", activation_pos)
    clear_grace_pos = input_text.find("ClearNoteLinkRenderGrace(hWnd);", jump_pos)
    resolver_pos = input_text.find("static NotePointerPresentationHit ResolveNotePointerPresentationHit")
    final_hit_pos = input_text.find("TryGetFinalNoteRenderHit", resolver_pos)
    final_only_guard_pos = input_text.find("if (FinalFrameCanUseCustomPaint(hWnd))", final_hit_pos)
    native_hit_pos = input_text.find("HitTestNativeNoteRawPosition", resolver_pos)
    native_index_conversion_pos = input_text.find(
        "MapEditorTransportToCanonicalNoteOffsetForInput(hWnd, *nativeHit)", native_hit_pos
    )
    if (dblclk_pos < 0 or pointer_hit_pos < 0 or activation_pos < 0 or jump_pos < 0 or
            clear_grace_pos < 0 or clear_grace_pos < jump_pos or resolver_pos < 0 or
            final_hit_pos < resolver_pos or final_only_guard_pos < final_hit_pos or
            native_hit_pos < final_only_guard_pos or
            native_index_conversion_pos < native_hit_pos):
        errors.append("src/note_view/note_view_input_proc.cppinc: link double-click must preserve final-frame coordinates and use whole-native fallback without legacy structured hit testing")
    resolver_end = input_text.find("\n}\n\nLRESULT CALLBACK NoteEditProc", resolver_pos)
    if resolver_end < 0:
        errors.append("src/note_view/note_view_input_proc.cppinc: final/native pointer resolver must remain a bounded helper")
    elif "HitTestMarkupPosition" in input_text[resolver_pos:resolver_end]:
        errors.append("src/note_view/note_view_input_proc.cppinc: final/native pointer resolver must not revive the retired markup hit-test path")
    elif "RichEditIndexToRawTextIndex" in input_text[resolver_pos:resolver_end]:
        errors.append("src/note_view/note_view_input_proc.cppinc: final/native pointer resolver must not linearly scan canonical text for a RichEdit coordinate")
    return errors
def find_assist_pane_visibility_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/note_view/note_view.h": {
            "bool ShouldShowBottomNotePane();": "bottom note assist visibility must be exposed to layout without duplicating projection logic",
        },
        "src/note_view/note_view_note_ops.cppinc": {
            "bool ShouldShowBottomNotePane()": "bottom note assist visibility must have a single projection-based gate",
            "if (g_bottomPanePin == BottomPanePin::Math) return false;": "math pane mode must not report the bottom note assist pane as visible",
            "if (g_bottomNoteMode == BottomNoteMode::Legacy) return false;": "legacy note extension mode must not report the separate assist pane as visible",
            "const BottomPaneProjection projection = BuildCurrentBottomPaneProjection(g_bottomNoteMode);": "assist pane visibility must use the current bottom pane projection",
            "if (projection.mode == BottomNoteMode::Headings) {": "headings pane visibility must allow placeholder text while headings are empty or updating",
            "if (!projection.valid || projection.empty) return false;": "empty or invalid assist pane content must be hidden",
            "return !projection.text.empty();": "assist pane visibility must require actual content text",
        },
        "src/ui/layout.cpp": {
            "bool showBottomNotePane = !showMathPane && ShouldShowBottomNotePane();": "layout must consult bottom note assist visibility before reserving the right-bottom pane",
            "const bool shortcutRight = false;": "note input shortcut panel must stay fixed in the bottom-left column",
            "const bool returnHiddenBottomPaneToNote = !noteAtTop && !showMathPane && !showBottomNotePane && !shortcutRight;": "hidden empty assist pane space must be returned to note editing when not used by right-side shortcuts",
            "int noteW = c.centerW + ((mergeNote || returnHiddenBottomPaneToNote) ? (c.rightW + c.split) : 0);": "note editor must reclaim the right-bottom width when assist pane is hidden and shortcuts are not placed there",
            "addPos(g_hBottomNote, bottomRightX, bottomRightY, bottomRightW, bottomNoteH, showBottomNotePane, true);": "empty bottom note assist pane must get zero height and be hidden by layout",
            "g_hBottomRight = showMathPane ? g_hBottomMath : (showBottomNotePane ? g_hBottomNote : nullptr);": "bottom-right alias must not point at a hidden empty assist pane",
        },
        "src/note_view/note_view_shared.cppinc": {
            "if (!ShouldShowBottomNotePane())": "refresh must not re-show an empty bottom note assist pane after layout hid it",
            "ShowWindow(g_hBottomNote, SW_HIDE);": "refresh must hide the bottom note assist pane when it has no content",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors
def find_startup_last_open_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/main.cpp": {
            "TraceStartupLastOpenRestore": "startup last-open restore must keep step-by-step trace evidence",
            "StartupLastOpenRestore": "startup last-open restore must use a stable preview_trace area",
            "ShowStartupLastOpenPartialNotice": "startup last-open restore must report partial restore failures via a quiet visual notice",
            "FindFileEntryIndexByPath": "startup last-open restore must validate PDF/Note targets separately against loaded file lists",
            "SelectStartupLastOpenFileListItem": "startup last-open restore must select last-open PDF/Note entries when auto-open is off",
            "end=selected_files_only": "startup last-open restore must have an explicit selection-only path for sessionAutoOpenMode=off",
            "end=open_files": "startup last-open restore must keep the explicit open-files path for view mode",
            "OpenPdfIfDifferent(hWnd, preferredPdf)": "startup last-open restore must open the preferred PDF independently",
            "OpenNoteIfDifferent(hWnd, preferredNote)": "startup last-open restore must open the preferred note independently",
        },
        "src/features/automation/main_ui_automation.cppinc": {
            "selectedOnlyRestored": "UI automation must cover startup last-open selection-only restore",
            "Startup restore did not select the last-open PDF/Note when auto-open was off.": "UI automation must fail if selection-only restore does not select the last-open files",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    main_text = (REPO_ROOT / "src/main.cpp").read_text(encoding="utf-8", errors="ignore")
    automation_text = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(encoding="utf-8", errors="ignore")
    select_pos = automation_text.find('g_config.sessionAutoOpenMode = L"off";')
    view_pos = automation_text.find('g_config.sessionAutoOpenMode = L"view";', select_pos)
    if select_pos < 0 or view_pos < 0 or select_pos > view_pos:
        errors.append("src/main.cpp: UI automation must verify selection-only restore before view/open restore")
    restore_pos = main_text.find("static bool RestoreStartupLastOpenSelection")
    load_pos = main_text.find("LoadFiles(targetSession, preferredPdf, preferredNote);", restore_pos)
    select_file_pos = main_text.find("SelectStartupLastOpenFileListItem", load_pos)
    if restore_pos < 0 or load_pos < 0 or select_file_pos < 0 or select_file_pos < load_pos:
        errors.append("src/main.cpp: startup last-open file list selection must happen after session files are loaded")
    return errors

def find_note_input_latency_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/note_view/note_view_input_proc.cppinc": {
            "NoteInputLatencyTrace": "note delete/newline latency must remain measurable under preview_trace",
            "NoteInputLatencyAction": "note input latency tracing must classify delete/backspace/enter actions",
            "backspace-char": "Backspace WM_CHAR latency must be traced",
            "delete-key": "Delete WM_KEYDOWN latency must be traced",
            "newline-char": "Enter/newline WM_CHAR latency must be traced",
            "tab-key": "Tab WM_KEYDOWN latency must be traced",
            "NoteInputLatencyTrace inputLatencyTrace(hWnd, msg, wParam);": "NoteEditProc must instantiate latency tracing before early returns",
            "saveRunning=": "note input latency logs must record whether a save transaction was running",
            "revisionChanged=": "note input latency logs must record whether the edit revision changed",
            "selBefore=": "note input latency logs must include selection before/after for delete/newline diagnosis",
        },
        "src/note_view/note_view_input_helpers.cppinc": {
            "case WM_MOUSEWHEEL:": "save transaction guard must not block note vertical wheel scrolling",
            "case WM_MOUSEHWHEEL:": "save transaction guard must not block note horizontal wheel scrolling",
            "case WM_VSCROLL:": "save transaction guard must not block note vertical scrollbar messages",
            "case WM_HSCROLL:": "save transaction guard must not block note horizontal scrollbar messages",
            "return false;": "scroll/paint messages must pass through while a save transaction is running",
        },
        "src/ui/core/main_window_proc.cppinc": {
            "file_output::NotifyNoteStageEdit(hWnd);": "note changes must stage-save through the deferred notification path instead of synchronous input-thread file writes",
            "ScheduleNoteOverlayRefresh(hWnd, false": "note rendering refresh should remain scheduled/deferred for ordinary input",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    helper_text = (REPO_ROOT / "src/note_view/note_view_input_helpers.cppinc").read_text(encoding="utf-8", errors="ignore")
    guard_pos = helper_text.find("static bool BlockNoteEditInputDuringSave")
    wheel_pos = helper_text.find("case WM_MOUSEWHEEL:", guard_pos)
    key_pos = helper_text.find("case WM_KEYDOWN:", guard_pos)
    if guard_pos < 0 or wheel_pos < 0 or key_pos < 0 or wheel_pos > key_pos:
        errors.append("src/note_view/note_view_input_helpers.cppinc: save guard must allow scroll messages before blocking key input")
    return errors


def find_note_indent_delete_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/note_view/note_view_note_ops.cppinc": {
            "bool structuralPrefix = false": "plain indentation must not be treated as a structural Markdown prefix",
            "ctx.structuralPrefix = hasQuotePrefix || hasListMarker || hasChecklist": "auto-prefix delete must be limited to quote/list/checklist structure",
            "!ctx.structuralPrefix": "auto-prefix delete must not remove an ordinary leading-space run",
            "TryHandleLeadingTabStopBackspace": "Backspace must have a dedicated tab-stop indentation path",
            "constexpr size_t kTabStopSpaces = 4": "tab-stop indentation deletion must remain explicit and bounded",
            "constexpr size_t kMaxLeadingIndentProbeChars = 4096": "tab-stop deletion must bound its current-line probe",
            "(column % kTabStopSpaces) != 0": "tab-stop deletion must fire only at tab-stop boundaries",
            "GetEditTextRangeForIndexing(hWnd, probeStart, caret)": "tab-stop deletion must inspect only a bounded current-line probe, not the whole note",
            "if (ch != L' ') return false;": "tab-stop deletion must require a pure leading-space run",
        },
        "src/note_view/note_view_input_proc.cppinc": {
            "TryHandleLeadingTabStopBackspace(hWnd)": "note Backspace must call the tab-stop deletion handler after structural-prefix checks",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    return errors



def find_note_auto_pair_bracket_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/note_view/note_view_note_ops.cppinc": {
            "TryResolveNoteAutoPairBracket": "note bracket pairs must be resolved by a dedicated helper",
            "TryHandleNoteAutoPairBracket": "note bracket input must be handled before the default WM_CHAR insertion path",
            "g_config.autoPairBrackets": "note bracket pairing must honor the user setting",
            "replacement.push_back(ch);": "note bracket pairing must insert the opening bracket in the same edit transaction",
            "replacement.push_back(close);": "note bracket pairing must insert the closing bracket in the same edit transaction",
            "ReplaceNoteSelectionAndSetSelection(hWnd, selStart, selEnd, replacement": "note bracket pairing must use the single replacement + selection helper",
            "const size_t nextSelStart = 1;": "empty note bracket pairing must place the caret inside the inserted pair",
            "const size_t nextSelEnd = 1 + selected.size();": "selected note text must remain selected inside the inserted bracket pair",
        },
        "src/note_view/note_view_input_proc.cppinc": {
            "TryHandleNoteAutoPairBracket(hWnd, static_cast<wchar_t>(wParam))": "note WM_CHAR must route bracket input to the auto-pair handler",
            "if (ignoreUntil != 0)": "full-width parenthesis left-navigation cancellation must remain armed across unrelated keys",
            "g_config.fullWidthParenCancelNextLeft": "full-width parenthesis left-navigation cancellation must honor its setting",
            "GetTickCount64() >= ignoreUntil": "expired parenthesis left-navigation cancellation must be cleared",
        },
        "src/note_view/note_view_input_helpers.cppinc": {
            "g_config.fullWidthParenCaretInside": "full-width parenthesis caret movement must honor its setting",
            "g_config.fullWidthParenCancelNextLeft": "full-width parenthesis left-navigation cancellation must honor its setting",
        },
        "src/pdf_view/input.cppinc": {
            "g_config.fullWidthParenCaretInside": "PDF text-box full-width parenthesis caret movement must honor its setting",
            "g_config.fullWidthParenCancelNextLeft": "PDF text-box left-navigation cancellation must honor its setting",
            "g_pdfIgnoreNextLeftKeyUntilTick": "PDF text-box must cancel one left navigation after a full-width parenthesis pair move",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    input_text = (REPO_ROOT / "src/note_view/note_view_input_proc.cppinc").read_text(encoding="utf-8", errors="ignore")
    hook_pos = input_text.find("TryHandleNoteAutoPairBracket(hWnd, static_cast<wchar_t>(wParam))")
    default_pos = input_text.find("RegisterExpectedNoteTextEdit(hWnd, selStart, selEnd, inserted", hook_pos)
    if hook_pos < 0 or default_pos < 0 or hook_pos > default_pos:
        errors.append("src/note_view/note_view_input_proc.cppinc: bracket auto-pairing must run before default character edit registration")
    ops_text = (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(encoding="utf-8", errors="ignore")
    if "case L'(':" in ops_text:
        errors.append("src/note_view/note_view_note_ops.cppinc: half-width '(' must not trigger note auto-pairing")
    if "case L'（':" in ops_text:
        errors.append("src/note_view/note_view_note_ops.cppinc: direct full-width '(' input must not trigger note auto-pairing")
    return errors


def find_workspace_config_compatibility_regressions() -> list[str]:
    errors: list[str] = []
    text = (REPO_ROOT / "src/core/app_core.cpp").read_text(encoding="utf-8", errors="ignore")
    config_text = (REPO_ROOT / "src/core/workspace_config.h").read_text(encoding="utf-8", errors="ignore")
    office_actions_text = (REPO_ROOT / "src/workspace/workspace_actions.cpp").read_text(encoding="utf-8", errors="ignore")
    required = {
        '"debugLogOfficeConversion",\n        "leftWidth"':
            "workspace.json writer compatibility key must remain in the known-field allowlist",
        'ParseJsonBoolField(json, "debugLogOfficeConversion")':
            "workspace.json office-conversion debug key must remain readable",
        'ofs << "  \\"debugLogOfficeConversion\\": "':
            "workspace.json office-conversion debug key must remain serializable",
    }
    for needle, message in required.items():
        if needle not in text:
            errors.append(f"src/core/app_core.cpp: {message}")
    if "bool officeConversion = false;" not in config_text:
        errors.append("src/core/workspace_config.h: Office conversion diagnostics must be disabled by default")
    prohibited_office_log_fragments = (
        'L"start source="',
        'L" command="',
        'L"output_dir="',
        'L" expected="',
        'L"output_identification_failed detail="',
    )
    for fragment in prohibited_office_log_fragments:
        if fragment in office_actions_text:
            errors.append(
                "src/workspace/workspace_actions.cpp: Office conversion diagnostics must not retain paths or command lines"
            )
            break
    import_text = office_actions_text
    required_import_no_follow = (
        "FILE_FLAG_OPEN_REPARSE_POINT",
        "GetFileInformationByHandle(srcHandle, &sourceInfo)",
        "FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY",
    )
    for needle in required_import_no_follow:
        if needle not in import_text:
            errors.append(
                "src/workspace/workspace_actions.cpp: import source handles must reject reparse points after opening"
            )
            break

    system_dll_load_contracts = {
        "src/core/app_core.cpp": ("uxtheme.dll",),
        "src/readonly_viewer/main.cpp": ("Msftedit.dll",),
        "src/ui/core/main_view_layout.cppinc": ("Msftedit.dll", "Riched20.dll"),
    }
    for relative_path, dll_names in system_dll_load_contracts.items():
        dll_load_text = (REPO_ROOT / relative_path).read_text(encoding="utf-8", errors="ignore")
        for dll_name in dll_names:
            required_load = f'LoadLibraryExW(L"{dll_name}", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)'
            bare_load = f'LoadLibraryW(L"{dll_name}")'
            if required_load not in dll_load_text or bare_load in dll_load_text:
                errors.append(
                    f"{relative_path}: {dll_name} must be loaded only from System32 to prevent DLL search-path hijacking"
                )

    publish_text = (REPO_ROOT / "publish.ps1").read_text(encoding="utf-8", errors="ignore")
    repo_checks_text = (REPO_ROOT / "tests/scripts/run_repo_checks.ps1").read_text(
        encoding="utf-8", errors="ignore"
    )
    if "ZIP placeholder marker" in repo_checks_text or "Assert-VersionTrackedDocumentation" in repo_checks_text:
        errors.append(
            "run_repo_checks.ps1: obsolete ZIP placeholder markers must not gate language-specific release confirmation"
        )
    expected_urllib_request_pattern = "urllib" + "\\.request"
    if 'from|import)\\s+(?:urllib|' in repo_checks_text or expected_urllib_request_pattern not in repo_checks_text:
        errors.append(
            "run_repo_checks.ps1: Python network scan must target request APIs without rejecting local URL decoding"
        )
    verify_read_only_contract = (
        'if ($Mode -eq "Verify") {',
        'Verify mode is read-only; no checklist was created or updated.',
        'exit 0',
    )
    for needle in verify_read_only_contract:
        if needle not in publish_text:
            errors.append("publish.ps1: Verify mode must validate without creating or updating a checklist")
            break
    submit_checklist_contract = 'JA/EN pair の公開確認文書がありません。'
    if submit_checklist_contract not in publish_text:
        errors.append("publish.ps1: Submit and Resubmit must require an existing checklist")
    locale_pair_contract = (
        'function Get-ReleaseSetPair',
        'function Get-RequestedReleaseSetPair',
        'function Get-LatestReleaseSetPair',
        'function Get-AllowlistDiffLines',
        'Allowlist diff placeholder was repaired in the existing unconfirmed checklist:',
        '$(foreach ($line in $allowlistDiffSection) { $line })',
        '公開snapshot allowlist 差分（コメント・空行を除いた有効エントリ）:',
        '--- ALLOWLIST DIFF BEGIN ---',
        'Confirmed checklist has no allowlist diff section; it is retained unchanged:',
        '$allowlistDiffLines = Get-AllowlistDiffLines',
        '$candidateDirectories += @(Get-ChildItem -LiteralPath $parentDirectory.FullName -Directory -Force)',
        'ParentPath = $directory.Parent.FullName',
        '$_.Version + "`n" + $_.ParentPath',
        'Specify exactly two release sets: one ja set and one en set.',
        'if ($ReleaseSetPath.Count -eq 0 -and $Mode -in @("ReleaseNotes", "Confirm", "Submit")) {',
        'return Get-LatestReleaseSetPair',
        'function Invoke-CreateReleaseSetPair',
        'exactly two created ja/en release set paths',
        'foreach ($item in $items)',
    )
    for needle in locale_pair_contract:
        if needle not in publish_text:
            errors.append("publish.ps1: normal publish must require and process the ja/en release-set pair")
            break
    unified_release_notes_contract = (
        'function Get-PairChecklistPath',
        '$pairName = Split-Path -Leaf (Split-Path -Parent $item.SetRoot)',
        '$setName = "${pairName}_${locale}"',
        '$names = @($byLocale["ja"]; $byLocale["en"])',
        'function New-PairChecklist',
        'function Get-PairConfirmationWord',
        'function New-ReleaseNotesText',
        '"## English",',
        '"## 日本語",',
        '"### System requirements",',
        '"### 動作環境",',
        'Get-ReleaseChecksumAssetName -Locale "ja"',
        'Get-ReleaseChecksumAssetName -Locale "en"',
        'function Invoke-UnifiedPublicSubmission',
        'function Invoke-UnifiedReleaseResubmission',
        '"v$($Manifest.app_version)"',
    )
    for needle in unified_release_notes_contract:
        if needle not in publish_text:
            errors.append("publish.ps1: a unified release must provide bilingual notes, four named checksums, and one version tag")
            break
    if publish_text.index('"## 日本語",') > publish_text.index('"## English",'):
        errors.append("publish.ps1: unified release notes must place the Japanese block before the English block")
    release_set_text = (REPO_ROOT / "scripts/release/make_release_set.ps1").read_text(encoding="utf-8", errors="ignore")
    distribution_name_contract = (
        'function Get-DistributionZipName',
        'pdf_note_workspace_${Version}_${Locale}_${Edition}.zip',
        'function Get-DistributionArchiveRootName',
        'PDF-Note-Workspace-$Version',
        '-Locale $Locale -Edition "full"',
        '-Locale $Locale -Edition "lite"',
    )
    if any(needle not in release_set_text for needle in distribution_name_contract):
        errors.append("make_release_set.ps1: distributable ZIP names must include product, version, locale, and edition")
    release_entry_text = (REPO_ROOT / "release.ps1").read_text(encoding="utf-8", errors="ignore")
    release_pair_layout_contract = (
        'function Get-ReleasePairDirectory',
        'pdf_note_workspace_release_${safeVersion}_${stamp}',
        '$targetLocales = @(',
        'if ($AllLocales) { "ja"; "en" } else { $Locale }',
        '"-ReleaseSetName", $targetLocale',
        'RELEASE_PAIR_PATH=$pairDirectory',
    )
    if any(needle not in release_entry_text for needle in release_pair_layout_contract):
        errors.append("release.ps1: -AllLocales must place ja/en sets in one named parent directory")
    release_snapshot_reuse_contract = (
        '$frozenPublicSnapshot = ""',
        '"-PublicSnapshotSource", $frozenPublicSnapshot',
        '"public_snapshot"',
    )
    if any(needle not in release_entry_text for needle in release_snapshot_reuse_contract):
        errors.append("release.ps1: -AllLocales must reuse the first frozen public snapshot for the paired locale")
    snapshot_reuse_gate_contract = (
        '[string]$PublicSnapshotSource = ""',
        'function Copy-DirectoryStrict',
        'Copy-DirectoryStrict -Source $PublicSnapshotSource -Destination $publicSnapshotDir',
        'if ([string]::IsNullOrWhiteSpace($PublicSnapshotSource)) {',
        'if (-not $DeferPostCreationValidation) {',
    )
    if any(needle not in release_set_text for needle in snapshot_reuse_gate_contract):
        errors.append("make_release_set.ps1: reused snapshots must be copied safely and still pass the release gates")
    release_set_logging_contract = (
        'function Initialize-ReleaseDetailLog',
        'out\\logs\\release_set',
        'function Invoke-ReleasePythonGate',
        'function Write-ReleaseFailureLogTail',
        'release set の詳細ログ:',
    )
    if any(needle not in release_set_text for needle in release_set_logging_contract):
        errors.append("make_release_set.ps1: failed release gates must retain and display a dedicated detail log")
    primary_run_log_contracts = {
        "build.ps1": ('out\\logs\\build\\build_{0}.log', 'Start-Transcript -LiteralPath $script:runLogPath', 'Stop-RunLog'),
        "release.ps1": ('out\\logs\\release\\release_{0}.log', 'Start-Transcript -LiteralPath $script:runLogPath', 'Stop-RunLog'),
        "publish.ps1": ('out\\logs\\publish\\publish_{0}.log', 'Start-Transcript -LiteralPath $script:runLogPath', 'Stop-RunLog'),
    }
    for relative_path, contract in primary_run_log_contracts.items():
        entry_text = (REPO_ROOT / relative_path).read_text(encoding="utf-8", errors="ignore")
        if any(needle not in entry_text for needle in contract):
            errors.append(f"{relative_path}: every invocation must create and close one primary run log")
    package_text = (REPO_ROOT / "scripts/release/pack_release.ps1").read_text(encoding="utf-8", errors="ignore")
    package_link_rewrite_contract = (
        '$content.Replace("](DOCUMENTATION.md)", "](docs/README.md)")',
        '$content.Replace("](docs/ja/README.md)", "](docs/README.md)")',
        '$content.Replace("](docs/en/README.md)", "](docs/README.md)")',
    )
    if any(needle not in package_text for needle in package_link_rewrite_contract):
        errors.append("pack_release.ps1: top-level README documentation links must resolve inside a locale package")
    libjpeg_notice = (REPO_ROOT / "third_party/pdfium/licenses/libjpeg_turbo.md").read_text(
        encoding="utf-8", errors="ignore"
    )
    if "[README.ijg](libjpeg_turbo.ijg)" not in libjpeg_notice:
        errors.append("libjpeg-turbo notice: IJG license link must point to the shipped libjpeg_turbo.ijg file")
    if "function Get-LatestReleaseSet {" in publish_text:
        errors.append("publish.ps1: obsolete single-locale latest-release selection must not remain")
    if "function Get-LatestChecklistReleaseSet" in publish_text:
        errors.append("publish.ps1: obsolete single-locale latest-checklist selection must not remain")
    integrity_contract = ('release_set_integrity_gate.py', 'SnapshotTreeHash', 'SnapshotAllowlistHash')
    for needle in integrity_contract:
        if needle not in publish_text:
            errors.append("publish.ps1: confirmation must bind the public snapshot manifest and allowlist hash")
            break
    pages_snapshot_gate_contract = (
        'function Test-FrozenReleaseSnapshotPages',
        'Test-FrozenReleaseSnapshotPages -SnapshotDirectory $snapshotDirectory',
        'function Test-CopiedSnapshotMatchesManifest',
        'Test-CopiedSnapshotMatchesManifest -SetRoot $SetRoot -SnapshotDirectory $worktreeRoot',
        'verifies the entire copied tree',
    )
    for needle in pages_snapshot_gate_contract:
        if needle not in publish_text:
            errors.append("publish.ps1: release-set validation must validate frozen Pages and the complete copied submission snapshot")
            break
    public_snapshot_content_contract = (
        'function Test-PublicSnapshotContent',
        'public_snapshot_content_gate.py',
        'Test-PublicSnapshotContent -SnapshotDirectory $snapshotDirectory',
        'artifact_manifest_sha256',
    )
    for needle in public_snapshot_content_contract:
        if needle not in publish_text:
            errors.append("publish.ps1: Verify must enforce frozen allowlist, artifact manifest, and snapshot content policy")
            break
    exporter_text = (REPO_ROOT / "tools/dev/export_public_snapshot.py").read_text(encoding="utf-8", errors="ignore")
    for needle in ("git_tracked_files(root)", "parse_artifact_manifest", "allowlisted directories contain files that are not tracked by Git"):
        if needle not in exporter_text:
            errors.append("tools/dev/export_public_snapshot.py: public snapshots must reject unreviewed Git-untracked files")
            break
    public_allowlist_text = (
        REPO_ROOT / "docs/internal/public_repo_release_allowlist_2026-08-24.txt"
    ).read_text(encoding="utf-8-sig", errors="ignore")
    for required in ("locales/", "tools/localization/", "docs/ja/", "docs/en/"):
        if required not in public_allowlist_text:
            errors.append(
                "public release allowlist: language build inputs and both locale document trees must be published together"
            )
            break
    creation_validation_contract = (
        '"-DeferPostCreationValidation"',
        'standalone release.ps1 keeps its self-validation by default',
    )
    for needle in creation_validation_contract:
        if needle not in publish_text:
            errors.append("publish.ps1: creation-time duplicate gates may only be deferred to immediate caller validation")
            break
    release_text = (REPO_ROOT / "release.ps1").read_text(encoding="utf-8", errors="ignore")
    make_release_text = (REPO_ROOT / "scripts/release/make_release_set.ps1").read_text(
        encoding="utf-8", errors="ignore"
    )
    if 'if ($DeferPostCreationValidation) { $releaseSetArgs += "-DeferPostCreationValidation" }' not in release_text:
        errors.append("release.ps1: deferred validation must be passed explicitly to release-set creation")
    build_text = (REPO_ROOT / "build.ps1").read_text(encoding="utf-8", errors="ignore")
    release_entrypoint_contract = (
        'release.ps1 は通常版と Lite版を一組で作成するため、-Lite は指定できません。',
        'release set は作成せず、ビルド成果物だけを削除します',
        'Invoke-RequiredScript -ScriptPath $buildScript -Arguments $buildArgs -Description "ビルド成果物の削除"',
    )
    for needle in release_entrypoint_contract:
        if needle not in release_text:
            errors.append("release.ps1: Lite-only release sets must be rejected and -Clean must not create a release set")
            break
    release_test_contract = (
        '[switch]$Test,',
        '$testOutputBase = Join-Path (Get-ReleaseSetBaseDirectory) "TEST_ONLY_DO_NOT_PUBLISH"',
        'pdf_note_workspace_TEST_ONLY_DO_NOT_PUBLISH_ja_full',
        '"-NoChecksums",',
        '"-TestOnly"',
        'TEST ONLY 出力が完了しました。',
    )
    for needle in release_test_contract:
        if needle not in release_text:
            errors.append("release.ps1: -Test must create only a clearly marked non-ZIP JA Full local output")
            break
    package_test_contract = (
        '[switch]$TestOnly,',
        '[switch]$NoChecksums,',
        'if ($TestOnly -and $Zip)',
        'if ($NoChecksums -and -not $TestOnly)',
        'TEST_ONLY_DO_NOT_PUBLISH.txt',
        'NOT A RELEASE SET OR DISTRIBUTABLE',
    )
    for needle in package_test_contract:
        if needle not in make_release_text and needle not in package_text:
            errors.append("pack_release.ps1: test-only output must be marked and must reject ZIP creation")
            break
    if 'if ($Clean -and $Rebuild) {' not in build_text:
        errors.append("build.ps1: mutually exclusive -Clean and -Rebuild must be rejected before invoking a build")
    deferred_gate_contract = (
        'if (-not $DeferPostCreationValidation)',
        'if ($DeferPostCreationValidation) { $integrityArgs += "--skip-validation-after-write" }',
        'if (-not $DeferPostCreationValidation -and -not $SnapshotOnly -and -not $Lite)',
        'RELEASE_SET_PATH=$setRoot',
    )
    for needle in deferred_gate_contract:
        if needle not in make_release_text:
            errors.append("make_release_set.ps1: deferred gates and the exact created path must remain explicit")
            break
    if 'release set は通常版と Lite版を一組で作成するため、-Lite は指定できません。' not in make_release_text:
        errors.append("make_release_set.ps1: Lite-only release sets must be rejected")
    return errors


def find_pdf_textbox_tap_commit_regressions() -> list[str]:
    errors: list[str] = []
    text = (REPO_ROOT / "src/pdf_view/input.cppinc").read_text(encoding="utf-8", errors="ignore")
    required = {
        "A PDF-surface click outside the active box is the explicit":
            "outside TextBox clicks must be documented as confirmation gestures",
        "CommitTextEditing(hwnd, true);":
            "outside TextBox clicks must commit active text editing",
        "if (g_pdf.pendingTextBoxHit && !g_pdf.movingAnnot)":
            "lost capture during a pending TextBox tap must have a fallback",
        "CommitPendingTextBoxEdit(hwnd);":
            "lost capture during a pending TextBox tap must complete the normal tap path",
    }
    for needle, message in required.items():
        if needle not in text:
            errors.append(f"src/pdf_view/input.cppinc: {message}")
    return errors


def find_note_tex_route_regressions() -> list[str]:
    errors: list[str] = []
    actions_path = REPO_ROOT / "src/workspace/workspace_actions.cpp"
    actions_text = actions_path.read_text(encoding="utf-8", errors="ignore")

    def creation_section(start_marker: str, end_marker: str, description: str) -> str:
        start = actions_text.find(start_marker)
        end = actions_text.find(end_marker, start)
        if start < 0 or end < 0:
            errors.append(f"src/workspace/workspace_actions.cpp: {description} must remain isolated for extension validation")
            return ""
        return actions_text[start:end]

    supported_extensions = creation_section(
        "bool IsSupportedNewNoteExtension",
        "std::wstring DefaultNewNoteStem",
        "new-note extension allowlist",
    )
    save_dialog = creation_section(
        "std::optional<std::wstring> PromptNoteFileNameWithSaveDialog",
        "std::optional<std::wstring> PromptCurrentNoteFileNameWithSaveDialog",
        "new-note save-dialog filters",
    )
    extension_menu = creation_section(
        "bool ShowNewNoteButtonContextMenu",
        "enum class ImportOneResult",
        "new-note extension menu",
    )
    for section, description in (
        (supported_extensions, "new-note extension allowlist"),
        (save_dialog, "new-note save-dialog filters"),
        (extension_menu, "new-note extension menu"),
    ):
        if 'L".tex"' in section:
            errors.append(f"src/workspace/workspace_actions.cpp: {description} must not offer .tex creation")

    route_text = (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    required_route_contract = {
        'if (ext == L".tex") return NoteContentRoute::TeX;': ".tex must remain openable as TeX source",
        "return note::NoteContentKind::TeXSource;": ".tex must keep a dedicated math-only source kind",
        "return CurrentNoteContentRoute() != NoteContentRoute::PlainText;": ".tex must support the render overlay",
        "BuildTeXMathRenderCacheLine": ".tex must render only its TeX math spans",
        "CurrentNoteUsesTeXMathOnlyRender": ".tex render cache must not apply Markdown presentation",
        "HasOnlyTeXLineWhitespaceAfter": ".tex block math must retain trailing source text rather than hiding it",
        "active_display_math_first_line": "rendered display math must switch as one raw surface while it is edited",
    }
    for needle, description in required_route_contract.items():
        if needle not in route_text:
            errors.append(f"src/note_view/note_view_shared.cppinc: {description}")
    parser_text = (REPO_ROOT / "src/note/note_parser.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    if "NoteDocument ParseTeXMathDocument" not in parser_text or "CollectTeXSourceCommentSpans" not in parser_text:
        errors.append("src/note/note_parser.cpp: TeX math parsing must keep source comments and non-math text raw")
    parser_tests = (REPO_ROOT / "tests/unit/note_parser_tests.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    if "TeX source parser extracts completed TeX math but ignores comments" not in parser_tests:
        errors.append("tests/unit/note_parser_tests.cpp: TeX math-only parsing must be covered")
    return errors


def find_final_render_publication_boundary_regressions() -> list[str]:
    """Keep the complete final route together in the shipped application."""
    errors: list[str] = []
    final_modules = (
        "src/note/note_parser_checkpoint.cpp",
        "src/note/note_source_coordinate_transform.cpp",
        "src/note/note_syntax_snapshot.cpp",
        "src/note/note_syntax_document.cpp",
        "src/note/note_render_layout_snapshot.cpp",
        "src/note/note_render_source_plan.cpp",
        "src/note/note_render_placement_snapshot.cpp",
        "src/note/note_presentation_owner_plan.cpp",
        "src/note/note_render_final_publication.cpp",
        "src/note/note_render_final_transaction.cpp",
        "src/note/note_render_final_interaction.cpp",
        "src/note/note_render_final_display_run.cpp",
        "src/note/note_render_final_presentation_snapshot.cpp",
        "src/note/note_render_final_presentation_interaction.cpp",
        "src/note/note_render_final_ime_preedit_presentation.cpp",
        "src/note/note_render_final_win32_adapter.cpp",
        "src/note/note_render_final_style.cpp",
        "src/note/note_render_final_gdi_measurement_provider.cpp",
        "src/note/note_render_final_gdi_painter.cpp",
    )
    manifest_path = REPO_ROOT / "scripts/build/build_sources.json"
    try:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"scripts/build/build_sources.json: cannot verify final-render runtime boundary: {exc}"]

    shipped_sources = set(manifest.get("BuildSourceFiles", ()))
    for module in final_modules:
        if module not in shipped_sources:
            errors.append(
                f"scripts/build/build_sources.json: final-render runtime module is missing from the application build ({module})"
            )

    note_view_fragments = set(
        manifest.get("TranslationUnitDesign", {})
        .get("UnityBuildFragments", {})
        .get("src/note_view/note_view.cpp", ())
    )
    if "note_view/note_view_final_render.cppinc" not in note_view_fragments:
        errors.append(
            "scripts/build/build_sources.json: note_view final-render bridge must remain in the note_view unity root"
        )

    parser_test_script = (REPO_ROOT / "tests/scripts/run_note_parser_tests.ps1").read_text(
        encoding="utf-8", errors="ignore"
    )
    for module in final_modules:
        if module not in parser_test_script:
            errors.append(
                f"tests/scripts/run_note_parser_tests.ps1: final-render differential test must compile {module}"
            )

    note_view_root = (REPO_ROOT / "src/note_view/note_view.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    note_view_final = (REPO_ROOT / "src/note_view/note_view_final_render.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    if '#include "note/note_render_final_win32_adapter.h"' not in note_view_root:
        errors.append("src/note_view/note_view.cpp: final adapter must be owned by the note view")
    for needle in (
        "NoteRenderFinalWin32AdapterState",
        "RefreshFinalNoteRenderFrame",
        "TryPaintFinalNoteRenderFrame",
        "TryGetFinalNoteRenderHit",
        "TryGetFinalNoteRenderCaretRect",
        "ResolveFinalLegacyLinkIdAtCanonicalOffset",
        "HandleFinalNoteRenderScrollCommand",
    ):
        if needle not in note_view_final:
            errors.append(
                f"src/note_view/note_view_final_render.cppinc: final runtime bridge must retain {needle}"
            )

    # Input and hit-testing are on the interactive path.  They may use the
    # immutable source-line aggregates, but must not materialize the entire
    # canonical model merely to translate RichEdit's CRLF coordinate system.
    source_line_map_header = (REPO_ROOT / "src/note/note_source_line_map.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    source_line_map_impl = (REPO_ROOT / "src/note/note_source_line_map.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "rich_edit_text_length() const noexcept",
        "CanonicalOffsetToRichEditIndex",
        "RichEditIndexToCanonicalOffset",
    ):
        if needle not in source_line_map_header:
            errors.append(
                f"src/note/note_source_line_map.h: final input coordinate aggregate must retain {needle}"
            )
    for needle in (
        "CanonicalOffsetToRichEditIndex",
        "RichEditIndexToCanonicalOffset",
        "SubtreeRichEditTextLength",
    ):
        if needle not in source_line_map_impl:
            errors.append(
                f"src/note/note_source_line_map.cpp: final input coordinate aggregate must implement {needle}"
            )
    if ("textCore.model()" in note_view_final or "core->model()" in note_view_final or
            "RichEditIndexToRawTextIndex(" in note_view_final):
        errors.append(
            "src/note_view/note_view_final_render.cppinc: final selection and hit mapping must not materialize or linearly scan complete canonical text"
        )
    if ("NoteSourceLineMap::CanonicalOffsetToRichEditIndex" not in note_view_final or
            "NoteSourceLineMap::RichEditIndexToCanonicalOffset" not in note_view_final):
        errors.append(
            "src/note_view/note_view_final_render.cppinc: final selection and hit mapping must use the persistent source-line coordinate aggregate"
        )

    adapter_header = (REPO_ROOT / "src/note/note_render_final_win32_adapter.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    adapter_impl = (REPO_ROOT / "src/note/note_render_final_win32_adapter.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "NoteRenderFinalWin32AdapterStateTestWork",
        "local_transaction_attempts",
        "complete_transaction_attempts",
        "test_work() const noexcept",
    ):
        if needle not in adapter_header:
            errors.append(
                f"src/note/note_render_final_win32_adapter.h: final publication decision accounting must retain {needle}"
            )
    for needle in (
        "++test_work_.local_transaction_attempts;",
        "++test_work_.local_transaction_builds;",
        "++test_work_.complete_transaction_attempts;",
        "++test_work_.complete_transaction_builds;",
    ):
        if needle not in adapter_impl:
            errors.append(
                f"src/note/note_render_final_win32_adapter.cpp: final publication decision accounting must retain {needle}"
            )
    parser_test_source = (REPO_ROOT / "tests/unit/note_parser_tests.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    if "a 4096-row final adapter edit uses one local transaction" not in parser_test_source:
        errors.append(
            "tests/unit/note_parser_tests.cpp: long final-adapter local transaction work contract must be covered"
        )
    if "converts canonical and RichEdit CRLF coordinates without text access" not in parser_test_source:
        errors.append(
            "tests/unit/note_parser_tests.cpp: persistent canonical/RichEdit coordinate conversion must be covered"
        )

    # The final modules are now part of the shipped, atomic route. Leaving a
    # pre-cutover "test-only" contract in their public comments invites a
    # future caller to treat them as optional migration artifacts again.
    for rel in (
        "src/note/note_presentation_owner_plan.h",
        "src/note/note_render_placement_snapshot.h",
        "src/note/note_render_final_publication.h",
        "src/note/note_render_final_transaction.h",
    ):
        runtime_contract_text = (REPO_ROOT / rel).read_text(
            encoding="utf-8", errors="ignore"
        )
        for stale_contract in (
            "remains differential-test-only",
            "remains test-only until",
            "remains parser-test-only",
        ):
            if stale_contract in runtime_contract_text:
                errors.append(
                    f"{rel}: shipped final-render contracts must not describe the runtime route as {stale_contract}"
                )

    final_paint_pos = note_view_final.find("static bool TryPaintFinalNoteRenderFrame")
    final_paint_end = note_view_final.find("static bool TryGetFinalNoteRenderHit", final_paint_pos)
    if (final_paint_pos < 0 or final_paint_end < final_paint_pos or
            "if (!painted)" not in note_view_final[final_paint_pos:final_paint_end] or
            "WithdrawFinalNoteRenderFrame(hWnd);" not in note_view_final[final_paint_pos:final_paint_end]):
        errors.append(
            "src/note_view/note_view_final_render.cppinc: a final paint failure must withdraw its publication before native fallback"
        )

    input_proc = (REPO_ROOT / "src/note_view/note_view_input_proc.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    paint_pos = input_proc.find("case WM_PAINT")
    final_paint_pos = input_proc.find("TryPaintFinalNoteRenderFrame", paint_pos)
    native_paint_pos = input_proc.find("PaintNativeNoteClient", final_paint_pos)
    if paint_pos < 0 or final_paint_pos < paint_pos or native_paint_pos < final_paint_pos:
        errors.append(
            "src/note_view/note_view_input_proc.cppinc: paint must choose final-frame paint or one whole-native fallback"
        )
    else:
        fallback_pos = input_proc.find("if (!finalPainted)", final_paint_pos)
        fallback_native_pos = input_proc.find("PaintNativeNoteClient", fallback_pos)
        fallback_reset_pos = input_proc.find("paintNoteBackground(paintDC);", fallback_pos)
        if (fallback_pos < 0 or fallback_native_pos < fallback_pos or
                fallback_reset_pos < fallback_pos or fallback_reset_pos > fallback_native_pos):
            errors.append(
                "src/note_view/note_view_input_proc.cppinc: native fallback after final paint failure must erase partial final pixels first"
            )
    if "DrawHighlightOverlay(" in input_proc[paint_pos:]:
        errors.append(
            "src/note_view/note_view_input_proc.cppinc: final paint must not reintroduce the retired clipped overlay"
        )

    # WM_SIZE is an invalidating input event: the pre-dispatch guard withdraws
    # the immutable frame before RichEdit consumes the resize. A note can be
    # opened while another pane owns focus, so the synchronizer must make
    # this refresh exception to its focus rule. Final-scrollbar acknowledgement
    # must not reformat native raw extents; external resizes still dispatch
    # natively before the normal post-default publication.
    resize_pos = input_proc.find("case WM_SIZE:", input_proc.find("case WM_TIMER:"))
    resize_end = input_proc.find("case WM_LBUTTONDBLCLK", resize_pos)
    if resize_pos < 0 or resize_end < resize_pos:
        errors.append(
            "src/note_view/note_view_input_proc.cppinc: live WM_SIZE handling must remain a bounded input branch"
        )
    else:
        resize_text = input_proc[resize_pos:resize_end]
        format_rect_pos = resize_text.find("UpdateNoteFormatRect(hWnd);")
        invalidate_pos = resize_text.find("InvalidateRect(hWnd, nullptr, FALSE);", max(0, format_rect_pos))
        if (format_rect_pos < 0 or invalidate_pos < format_rect_pos or
                "RefreshFinalNoteRenderFrame(hWnd);" in resize_text):
            errors.append(
                "src/note_view/note_view_input_proc.cppinc: WM_SIZE must update geometry and defer exactly one final/native publication until after the native procedure"
            )
        scrollbar_ack = resize_text[:max(0, format_rect_pos)]
        if ("FinalRenderRouteEnabled(hWnd)" not in scrollbar_ack or
                "finalRenderScrollSyncInProgress" not in scrollbar_ack or
                "SyncRenderDrivenNoteInputUi(hWnd, msg);" not in scrollbar_ack or
                "return 0;" not in scrollbar_ack or "CallWindowProc" in scrollbar_ack):
            errors.append(
                "src/note_view/note_view_input_proc.cppinc: final-scrollbar WM_SIZE must close/coalesce final publication without native raw reformatting"
            )

    input_helpers = (REPO_ROOT / "src/note_view/note_view_input_helpers.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    sync_input_pos = input_helpers.find("static void SyncRenderDrivenNoteInputUi")
    sync_input_end = input_helpers.find("static void RequestNoteContentRefreshAfterEdit", sync_input_pos)
    if sync_input_pos < 0 or sync_input_end < sync_input_pos:
        errors.append(
            "src/note_view/note_view_input_helpers.cppinc: post-default final-frame input synchronizer must remain bounded"
        )
    else:
        sync_input_text = input_helpers[sync_input_pos:sync_input_end]
        resize_exception_pos = sync_input_text.find("msg == WM_SIZE && refreshFinalFrame")
        refresh_pos = sync_input_text.find("RefreshFinalNoteRenderFrame(hWnd);")
        unfocused_return_pos = sync_input_text.find("if (!focused) return;", refresh_pos)
        if (resize_exception_pos < 0 or refresh_pos < resize_exception_pos or
                unfocused_return_pos < refresh_pos):
            errors.append(
                "src/note_view/note_view_input_helpers.cppinc: post-default WM_SIZE must republish final/native ownership while unfocused, without running focused-only input work"
            )
    ime_caret_pos = input_helpers.find("static bool TryGetNoteImeCaretRect")
    final_caret_pos = input_helpers.find("TryGetFinalNoteRenderCaretRect", ime_caret_pos)
    native_caret_pos = input_helpers.find("TryGetNativeNoteCaretRect", final_caret_pos)
    if (ime_caret_pos < 0 or final_caret_pos < ime_caret_pos or
            native_caret_pos < final_caret_pos or
            "BuildNoteRenderPresentationFrame" in input_helpers[ime_caret_pos:native_caret_pos]):
        errors.append(
            "src/note_view/note_view_input_helpers.cppinc: IME caret must use final geometry or whole-native fallback, never a legacy presentation frame"
        )

    note_ops = (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    recompute_pos = note_ops.find("void RecomputeMathFromNote()")
    final_semantic_pos = note_ops.find("RefreshNoteSyntaxStageFromCanonicalCore", recompute_pos)
    editor_read_pos = note_ops.find("GetEditTextForIndexing(g_hNoteEdit)", recompute_pos)
    if (recompute_pos < 0 or final_semantic_pos < recompute_pos or
            editor_read_pos < final_semantic_pos or
            "editor_full_read=false" not in note_ops[final_semantic_pos:editor_read_pos]):
        errors.append(
            "src/note_view/note_view_note_ops.cppinc: final-frame semantic refresh must consume the canonical root before any whole-editor recovery read"
        )
    final_fallback_pos = note_ops.find("stage=final_native_fallback", editor_read_pos)
    legacy_layout_pos = note_ops.find("RefreshNoteRenderAndLayoutStage(", editor_read_pos)
    if (final_fallback_pos < editor_read_pos or legacy_layout_pos < final_fallback_pos or
            "withdrawn_no_legacy_rebuild" not in note_ops[final_fallback_pos:legacy_layout_pos]):
        errors.append(
            "src/note_view/note_view_note_ops.cppinc: a failed final publication must choose whole-native fallback without rebuilding the retired LineCache"
        )
    link_lookup_pos = note_ops.find("static std::optional<std::wstring> LinkIdAtCharIndex")
    final_link_lookup_pos = note_ops.find("ResolveFinalLegacyLinkIdAtCanonicalOffset", link_lookup_pos)
    line_cache_link_lookup_pos = note_ops.find("const LineCache& cache", link_lookup_pos)
    if (link_lookup_pos < 0 or final_link_lookup_pos < link_lookup_pos or
            line_cache_link_lookup_pos < final_link_lookup_pos):
        errors.append(
            "src/note_view/note_view_note_ops.cppinc: final-frame link lookup must resolve the immutable source plan before any legacy cache fallback"
        )

    note_ui_header = (REPO_ROOT / "src/note_view/note_view.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    if ("bool finalFrameActive = false;" not in note_ui_header or
            "size_t finalStructuredLineCount = 0;" not in note_ui_header or
            "size_t finalRawLineCount = 0;" not in note_ui_header):
        errors.append(
            "src/note_view/note_view.h: UI automation must expose only read-only final-frame ownership facts"
        )
    final_snapshot_pos = note_view_final.find("static bool PopulateNoteUiSnapshotFromFinalFrame")
    final_snapshot_end = note_view_final.find("static void WithdrawFinalNoteRenderFrame", final_snapshot_pos)
    if (final_snapshot_pos < 0 or final_snapshot_end < final_snapshot_pos):
        errors.append(
            "src/note_view/note_view_final_render.cppinc: final-frame UI observation must remain isolated"
        )
    else:
        final_snapshot_text = note_view_final[final_snapshot_pos:final_snapshot_end]
        for required in (
            "snapshot->finalFrameActive = true;",
            "++snapshot->finalStructuredLineCount;",
            "++snapshot->finalRawLineCount;",
        ):
            if required not in final_snapshot_text:
                errors.append(
                    "src/note_view/note_view_final_render.cppinc: final-frame UI observation must report immutable surface ownership"
                )
                break
    final_scroll_sync_pos = note_view_final.find("static void SyncFinalNoteScrollBars")
    final_scroll_sync_end = note_view_final.find(
        "static void EnsureFinalNoteRenderCaretVisible", final_scroll_sync_pos
    )
    if (final_scroll_sync_pos < 0 or final_scroll_sync_end < final_scroll_sync_pos or
            "if (context.finalRenderScrollSyncInProgress) return;" not in
            note_view_final[final_scroll_sync_pos:final_scroll_sync_end] or
            "ScopedScrollSyncReset" not in
            note_view_final[final_scroll_sync_pos:final_scroll_sync_end] or
            "const auto frame = context.finalRenderState.frame();" not in
            note_view_final[final_scroll_sync_pos:final_scroll_sync_end] or
            "bool finalRenderScrollSyncInProgress = false;" not in
            (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
                encoding="utf-8", errors="ignore"
            )):
        errors.append(
            "src/note_view: final scrollbar synchronization must reject synchronous Win32 reentry and retain its immutable frame"
        )
    final_refresh_pos = note_view_final.find("static bool RefreshFinalNoteRenderFrame")
    final_refresh_end = note_view_final.find("static void BeginFinalNoteImePreedit", final_refresh_pos)
    final_refresh_text = note_view_final[final_refresh_pos:final_refresh_end]
    if (final_refresh_pos < 0 or final_refresh_end < final_refresh_pos or
            "if (context.finalRenderPublicationInProgress)" not in final_refresh_text or
            "ScopedFinalPublicationReset" not in final_refresh_text or
            "bool finalRenderPublicationInProgress = false;" not in
            (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
                encoding="utf-8", errors="ignore"
            ) or
            "bool finalRenderPublicationPending = false;" not in
            (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
                encoding="utf-8", errors="ignore"
            ) or
            "constexpr int kMaxPublicationPasses = 3;" not in final_refresh_text):
        errors.append(
            "src/note_view: final publication must reject synchronous Win32 reentry before it can recursively measure or publish"
        )
    final_format_pos = note_view_final.find("static bool ApplyFinalNoteNativeFallbackFormatRect")
    final_format_end = note_view_final.find("[[nodiscard]] bool BuildFinalLayoutKey", final_format_pos)
    shared_format_pos = (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    ).find("static void UpdateNoteFormatRect(HWND hWnd)")
    shared_format_end = (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    ).find("static std::wstring ToLowerAscii", shared_format_pos)
    final_format_text = note_view_final[final_format_pos:final_format_end]
    shared_format_text = (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )[shared_format_pos:shared_format_end]
    final_branch_pos = shared_format_text.find("ApplyFinalNoteNativeFallbackFormatRect(hWnd)")
    legacy_canvas_pos = shared_format_text.find("const bool nextRenderActive")
    if (final_format_pos < 0 or final_format_end < final_format_pos or
            shared_format_pos < 0 or shared_format_end < shared_format_pos or
            final_branch_pos < 0 or legacy_canvas_pos < final_branch_pos or
            "finalNativeFallbackFormatSyncInProgress" not in final_format_text or
            "EM_SETTARGETDEVICE" not in final_format_text or
            "EM_SETRECTNP" not in final_format_text or
            "SetScrollInfo" in final_format_text):
        errors.append(
            "src/note_view: final route must give RichEdit only a guarded transport rectangle and must not derive legacy scrollbars"
        )
    shared_all = (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    line_spacing_pos = shared_all.find("void RefreshNoteLineSpacingForPresentationSurface")
    line_spacing_text = shared_all[line_spacing_pos:]
    if (line_spacing_pos < 0 or
            "FinalRenderRouteEnabled(hWnd)" not in line_spacing_text or
            line_spacing_text.find("FinalRenderRouteEnabled(hWnd)") >
            line_spacing_text.find("CollectNoteNativeRawMetricLines")):
        errors.append(
            "src/note_view/note_view_shared.cppinc: final native fallback must not revive legacy line-spacing/cache ownership"
        )
    input_proc = (REPO_ROOT / "src/note_view/note_view_input_proc.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    wheel_pos = input_proc.find("case WM_MOUSEHWHEEL:")
    wheel_end = input_proc.find("case WM_RBUTTONDOWN:", wheel_pos)
    hscroll_pos = input_proc.find("case WM_HSCROLL:")
    hscroll_end = input_proc.find("case WM_TIMER:", hscroll_pos)
    final_input_guard = "if (FinalRenderRouteEnabled(hWnd)) break;"
    legacy_hscroll_guard = "if (IsNoteRenderActive() && !FinalRenderRouteEnabled(hWnd))"
    if (wheel_pos < 0 or wheel_end < wheel_pos or
            final_input_guard not in input_proc[wheel_pos:wheel_end] or
            hscroll_pos < 0 or hscroll_end < hscroll_pos or
            legacy_hscroll_guard not in input_proc[hscroll_pos:hscroll_end]):
        errors.append(
            "src/note_view/note_view_input_proc.cppinc: final native fallback must pass horizontal scroll input to RichEdit instead of the retired canvas"
        )
    final_input_raw_uses = input_proc.count("CurrentNoteSyntaxSource().raw")
    if (final_input_raw_uses != 2 or
            "TryMapFinalRouteCanonicalOffsetToRichEdit" not in input_proc or
            "TryMapFinalRouteRichEditIndexToCanonical" not in input_proc):
        errors.append(
            "src/note_view/note_view_input_proc.cppinc: final input must confine legacy raw-string coordinate scans to the non-final bridge branches"
        )
    if ("TryMapFinalRouteCanonicalOffsetToRichEdit" not in note_view_final or
            "TryMapFinalRouteRichEditIndexToCanonical" not in note_view_final or
            "NoteSourceLineMap::CanonicalOffsetToRichEditIndex" not in note_view_final or
            "NoteSourceLineMap::RichEditIndexToCanonicalOffset" not in note_view_final or
            "editor_replacement_span" not in note_view_final):
        errors.append(
            "src/note_view/note_view_final_render.cppinc: final input transport must resolve both directions through the persistent source-line map, including proven IME preedit boundaries"
        )
    expand_pos = (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    ).find("void ExpandNoteRenderCanvasForPendingEdit")
    expand_end = (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    ).find("static int MeasureRenderedContentBottomPx", expand_pos)
    if (expand_pos < 0 or expand_end < expand_pos or
            "if (FinalRenderRouteEnabled(hWnd)) return;" not in
            (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(
                encoding="utf-8", errors="ignore"
            )[expand_pos:expand_end]):
        errors.append(
            "src/note_view/note_view_note_ops.cppinc: final route must not expand the retired helper canvas during an edit"
        )
    ui_automation = (REPO_ROOT / "src/features/automation/main_ui_automation.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    for required in (
        "A freshly opened rendered note did not publish one fully structured final frame.",
        "Final note rendering did not survive a narrow resize/scrollbar round trip.",
        "constexpr int kResizeRoundTrips = 4;",
        "A table caret did not make exactly its atomic table surface raw inside the final frame.",
    ):
        if required not in ui_automation:
            errors.append(
                "src/features/automation/main_ui_automation.cppinc: UI automation must exercise final-frame structured/raw ownership"
            )
            break

    shared = (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(
        encoding="utf-8", errors="ignore"
    )
    switch_begin = shared.find("bool BeginNoteRenderSwitch()")
    switch_end = shared.find("void EndNoteRenderSwitch()")
    switch_end_finish = shared.find("bool IsNoteRenderSwitchInProgress()", switch_end)
    if (switch_begin < 0 or switch_end < switch_begin or switch_end_finish < switch_end):
        errors.append(
            "src/note_view/note_view_shared.cppinc: render mode transition must retain bounded begin/end ownership"
        )
    else:
        begin_text = shared[switch_begin:switch_end]
        end_text = shared[switch_end:switch_end_finish]
        withdraw_pos = begin_text.find("WithdrawFinalNoteRenderFrame(g_hNoteEdit);")
        freeze_pos = begin_text.find("SendMessageW(g_hNoteEdit, WM_SETREDRAW, FALSE, 0);")
        unblocked_pos = end_text.find("g_noteRenderSwitchBlocked = false;")
        publish_pos = end_text.find("RecomputeMathFromNote();", unblocked_pos)
        expose_pos = end_text.find("SendMessageW(hWnd, WM_SETREDRAW, TRUE, 0);")
        if (withdraw_pos < 0 or freeze_pos < withdraw_pos or
                unblocked_pos < 0 or publish_pos < unblocked_pos or
                expose_pos < publish_pos):
            errors.append(
                "src/note_view/note_view_shared.cppinc: a mode transition must withdraw the old frame, publish final/native ownership while frozen, then expose it"
            )

    toggle_pos = shared.find("static void ToggleNoteRenderSetting()")
    toggle_end = shared.find("static void CompletePendingBottomNoteRenderToggleAfterIme", toggle_pos)
    if toggle_pos < 0 or toggle_end < toggle_pos:
        errors.append(
            "src/note_view/note_view_shared.cppinc: render toggle must remain a bounded state transition"
        )
    else:
        toggle_text = shared[toggle_pos:toggle_end]
        if ("RefreshMarkdownRenderCache(" in toggle_text or
                "UpdateNoteLineSpacing(" in toggle_text):
            errors.append(
                "src/note_view/note_view_shared.cppinc: render toggle must not rebuild the retired LineCache or line-spacing route before final publication"
            )

    load_pos = note_ops.find("void LoadNoteFile(")
    clear_pos = note_ops.find("void ClearNoteEditorSilently(")
    save_pos = note_ops.find("bool SaveNoteFile(", clear_pos)
    if load_pos < 0 or clear_pos < load_pos or save_pos < clear_pos:
        errors.append(
            "src/note_view/note_view_note_ops.cppinc: note load/clear must retain bounded switch transitions"
        )
    else:
        load_text = note_ops[load_pos:clear_pos]
        clear_text = note_ops[clear_pos:save_pos]
        if "RecomputeMathFromNote();" in load_text or "RecomputeMathFromNote();" in clear_text:
            errors.append(
                "src/note_view/note_view_note_ops.cppinc: load/clear must defer publication to EndNoteRenderSwitch rather than taking a legacy switch-time route"
            )

    for rel in (
        "src/note/note_render_final_publication.h",
        "src/note/note_render_final_publication.cpp",
        "src/note/note_render_final_transaction.h",
        "src/note/note_render_final_transaction.cpp",
    ):
        text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="ignore")
        for forbidden in (
            "LineCache",
            "RenderLineStore",
            "ActiveNoteRenderSnapshot",
            "windows.h",
            "gdi32",
        ):
            if forbidden in text:
                errors.append(
                    f"{rel}: final publication must stay independent from legacy or Win32 rendering state ({forbidden})"
                )

    transaction_header = (REPO_ROOT / "src/note/note_render_final_transaction.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    transaction_impl = (REPO_ROOT / "src/note/note_render_final_transaction.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "BuildComplete",
        "NoteRenderFinalMeasurementProvider",
        "NoteRenderFinalMeasuredFrame",
        "NoteSyntaxSnapshot::BuildFromTextCore",
        "NoteRenderSourcePlan::Build",
        "measurementProvider.Measure",
        "NoteRenderLineLayoutMap::Build",
        "NoteRenderLayoutSnapshot::Build",
        "NoteRenderPlacementSnapshot::Build",
        "NoteRenderFinalPublication::Build",
        "NoteRenderFinalPublication::BuildWithCheckpoint",
        "BuildLocalPlainTextPatch",
        "MeasureReplacement",
        "RequiresNativeFallback",
    ):
        if needle not in transaction_header and needle not in transaction_impl:
            errors.append(
                f"src/note/note_render_final_transaction.*: complete final transaction must retain {needle}"
            )
    parser_test_source = (REPO_ROOT / "tests/unit/note_parser_tests.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    if "NoteRenderFinalTransaction::BuildComplete" not in parser_test_source:
        errors.append(
            "tests/unit/note_parser_tests.cpp: complete final transaction must be covered by an atomic-publication test"
        )
    if "NoteRenderFinalTransaction::BuildLocalPlainTextPatch" not in parser_test_source:
        errors.append(
            "tests/unit/note_parser_tests.cpp: local final transaction must be covered by an atomic-publication test"
        )
    publication_header = (REPO_ROOT / "src/note/note_render_final_publication.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    publication_impl = (REPO_ROOT / "src/note/note_render_final_publication.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "BuildWithCheckpoint",
        "continuity_checkpoint_",
        "has_continuity_checkpoint",
        "HasExactCheckpoint",
    ):
        if needle not in publication_header and needle not in publication_impl:
            errors.append(
                f"src/note/note_render_final_publication.*: complete transaction continuity must retain {needle}"
            )

    source_plan_header = (REPO_ROOT / "src/note/note_render_source_plan.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    source_plan_impl = (REPO_ROOT / "src/note/note_render_source_plan.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "SourcePlanRelativeBoundary",
        "NoteRenderSourcePlanStorage",
        "NoteRenderSourcePlanLocalPatchWork",
        "tail_line_payload_rewrites",
        "SharesLinePayloadForDifferentialTest",
        "ProvesUnchangedRowsFrom",
        "NoteRenderAtomicGroupQueryWork",
        "BuildIntervalIndex",
        "AppendIntersecting",
    ):
        if needle not in source_plan_header and needle not in source_plan_impl:
            errors.append(
                f"src/note/note_render_source_plan.*: persistent local source-plan splice must retain {needle}"
            )
    if "std::vector<NoteRenderSourceLinePlan> lines_;" in source_plan_header:
        errors.append(
            "src/note/note_render_source_plan.h: source-plan rows must not regress to a fully copied vector member"
        )
    if "std::any_of(storage->groups_.begin(), storage->groups_.end()" in source_plan_impl:
        errors.append(
            "src/note/note_render_source_plan.cpp: atomic-group lookup must not regress to a linear full-group scan"
        )

    checkpoint_header = (REPO_ROOT / "src/note/note_parser_checkpoint.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    checkpoint_impl = (REPO_ROOT / "src/note/note_parser_checkpoint.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "NoteParserCheckpointStorage",
        "BuildNoteParserCheckpointLocalPlainTextPatch",
        "NoteParserCheckpointLocalPatchWork",
        "SharesLinePayloadForDifferentialTest",
        "NoteSourceLineMap::LineAt",
    ):
        if needle not in checkpoint_header and needle not in checkpoint_impl:
            errors.append(
                f"src/note/note_parser_checkpoint.*: persistent checkpoint state must retain {needle}"
            )
    if "std::vector<NoteParserLineCheckpoint> lines;" in checkpoint_header:
        errors.append(
            "src/note/note_parser_checkpoint.h: checkpoint rows must not regress to a fully copied vector member"
        )
    syntax_snapshot_impl = (REPO_ROOT / "src/note/note_syntax_snapshot.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    syntax_snapshot_header = (REPO_ROOT / "src/note/note_syntax_snapshot.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    if "NoteSourceLineMap::SameSnapshotIdentity" not in syntax_snapshot_impl:
        errors.append(
            "src/note/note_syntax_snapshot.cpp: local snapshot must bind its checkpoint to the exact source-line-map identity"
        )
    for needle in (
        "NoteSyntaxDocument::Build(",
        "NoteSyntaxDocument::BuildLocalPlainTextPatch",
        "NoteTextPieceSequence::CopyRange",
        "CopyDocumentForCompleteBuild",
    ):
        if needle not in syntax_snapshot_impl:
            errors.append(
                f"src/note/note_syntax_snapshot.cpp: final persistent syntax snapshot must retain {needle}"
            )
    for forbidden in (
        "NoteTextModel source_;",
        "NoteDocument document_;",
        "NoteInfluenceIndex influence_index_;",
        "SemanticIndexSnapshot semantic_index_;",
    ):
        if forbidden in syntax_snapshot_header:
            errors.append(
                f"src/note/note_syntax_snapshot.h: final snapshot must not retain a legacy full-sized member ({forbidden})"
            )

    if "syntax->source()" in source_plan_impl or "previous->syntax_->source()" in source_plan_impl:
        errors.append(
            "src/note/note_render_source_plan.cpp: final source plan must read only explicit snapshot ranges, never a legacy source model"
        )
    if "syntax->document()" in source_plan_impl:
        errors.append(
            "src/note/note_render_source_plan.cpp: final source plan must not retain or consume a legacy snapshot document"
        )
    if "CopyDocumentForCompleteBuild" in source_plan_impl:
        errors.append(
            "src/note/note_render_source_plan.cpp: final source-plan construction must resolve NoteSyntaxDocument directly, never materialize a legacy NoteDocument"
        )
    if "NoteDocument" in source_plan_impl:
        errors.append(
            "src/note/note_render_source_plan.cpp: final source-plan construction must not carry any legacy NoteDocument representation"
        )
    for needle in (
        "syntax->syntax_document()",
        "CopySourceRange",
        "document.ResolveBlock",
        "document.ResolveInline",
        "document.ResolveStyle",
        "document.ResolveMath",
    ):
        if needle not in source_plan_impl:
            errors.append(
                f"src/note/note_render_source_plan.cpp: final source plan must retain direct persistent syntax access ({needle})"
            )

    syntax_document_header = (REPO_ROOT / "src/note/note_syntax_document.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    syntax_document_impl = (REPO_ROOT / "src/note/note_syntax_document.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "NoteSyntaxDocumentStorage",
        "NoteSyntaxDocumentSpanIndex",
        "RelativeSpan",
        "BuildLocalPlainTextPatch",
        "PermitsPlainTextLeafEdit",
        "NoteSyntaxDocumentQueryWork",
        "SharesNodePayloadForDifferentialTest",
        "CopyDocumentForCompleteBuild",
        "NoteSourceLineMap::SameSnapshotIdentity",
        "tail_node_payload_rewrites",
    ):
        if needle not in syntax_document_header and needle not in syntax_document_impl:
            errors.append(
                f"src/note/note_syntax_document.*: persistent final syntax document must retain {needle}"
            )
    if "std::vector<StoredNode> nodes_;" in syntax_document_impl:
        errors.append(
            "src/note/note_syntax_document.cpp: final syntax nodes must not regress to a fully copied vector member"
        )

    placement_header = (REPO_ROOT / "src/note/note_render_placement_snapshot.h").read_text(
        encoding="utf-8", errors="ignore"
    )
    placement_impl = (REPO_ROOT / "src/note/note_render_placement_snapshot.cpp").read_text(
        encoding="utf-8", errors="ignore"
    )
    for needle in (
        "PlacementRelativeBoundary",
        "NoteRenderPlacementStorage",
        "NoteRenderPlacementSnapshotLocalSpliceWork",
        "tail_line_payload_rewrites",
        "SharesLinePayloadForDifferentialTest",
        "ProvesUnchangedRowsFrom",
    ):
        if needle not in placement_header and needle not in placement_impl:
            errors.append(
                f"src/note/note_render_placement_snapshot.*: persistent local placement splice must retain {needle}"
            )
    if "std::vector<NoteRenderLinePlacement> line_placements_;" in placement_header:
        errors.append(
            "src/note/note_render_placement_snapshot.h: placement rows must not regress to a fully copied vector member"
        )
    return errors


def find_note_render_incremental_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/note/note_dirty_graph.cpp": {
            "NoteDirtyGraphAllowsRenderEarlyStop": "note render dirty graph must expose an early-stop predicate",
            "graph.edit_kind == NoteEditKind::PlainText": "plain text edits must be distinguishable for bounded render refresh",
            "graph.syntax_features == NoteDirtySyntaxFeature::None": "bounded render refresh must be limited to syntax-neutral edits",
        },
        "tests/unit/note_parser_tests.cpp": {
            "dirty graph enables bounded render and spacing fast paths": "dirty graph fast paths must be covered by parser tests",
            "dirty graph expands edits inside a code fence to its closing boundary": "dirty graph must not truncate structural fence invalidation",
            "dirty graph keeps balanced inline math changes on the affected line": "dirty graph must keep balanced inline math local",
        },
        "src/note_view/note_view_shared.cppinc": {
            "TryRefreshMarkdownRenderCacheForDirtyGraph": "note render cache must have a dirty-graph incremental refresh path",
            "TryApplyRenderLineIndexEdit": "note render line index must be updated from the carried TextEdit",
            "SeedMarkdownFenceStateFromPreviousLine": "incremental render refresh must preserve inherited fence/container state",
            "note::NoteDirtyGraphAllowsRenderEarlyStop(graph, lineCountChanged)": "incremental render refresh must stop early only for safe dirty graphs",
            "MarkNoteRenderDirtyLines(startLine, rebuiltLastLine, lineCountChanged)": "incremental render refresh must invalidate only rebuilt line ranges",
            "lineSpacingGraph = *pendingGraph;": "line spacing refresh must carry the dirty graph into its fast path",
            "note::NoteDirtyGraphAllowsLineSpacingFastPath(*lineSpacingGraph)": "line spacing refresh must use dirty-graph fast-path eligibility",
            "InvalidateNoteRenderDirtyLines(g_hNoteEdit, dirtyRange": "note repaint must target dirty render lines when possible",
            "struct NoteEditorTextCoreBinding": "zero-copy TextCore access must carry an editor-bound document identity",
            "editorTextCoreBinding.Matches(sourceIdentity)": "TextCore reuse must require the editor-bound revision",
            "core->MatchesRaw(observedText)": "the editor binding must be published from an exact canonical-text comparison",
            "std::upper_bound(": "rendered caret boundaries must retain logarithmic raw-offset lookup",
            "if (!layoutFrame.renderActive && !GetCharPos(hWnd, lineStart, &linePos))": "structured run placement must not query RichEdit positions per extent line",
            "flowEndX = std::max(flowEndX, segBaseX + segWidth);": "rendered flow extent must include first-glyph bearing alignment",
            "g_noteImeCompositionRequiresWholeViewFallback = true;": "structural IME edits must keep the current composition in whole-view fallback",
            "struct NoteImeCompositionPresentationAnchor": "IME hybrid painting must retain a bounded preedit anchor rather than a text copy",
            "CanRetainCommittedNoteLayoutDuringImeComposition": "IME hybrid painting must prove anchor geometry before reusing a committed snapshot",
            "TryMapCommittedRawIndexThroughImeComposition": "IME hybrid painting must map committed source offsets through a preedit delta",
            "currentNextLineStart.y - currentLineStart.y": "IME hybrid painting must reject a preedit that changes following-row geometry",
            "AdvanceNoteImePresentationPhaseForWindow": "IME presentation must have one event-driven phase transition owner",
            "ResultCommittedAwaitingEnd": "an IME result must remain committed despite residual IMM composition text",
        },
        "src/note_view/note_view_note_ops.cppinc": {
            "IsNoteLineRawHelper(hWnd, line)": "note render overlay must identify raw helper lines",
            "isStaleEditingLine": "note render overlay must isolate stale editing lines",
            "drawStaleRawLine": "note render overlay must draw current raw text only for stale/raw lines",
            "NotePresentationLineSurface::NativeRaw": "caret raw line must use native RichEdit drawing instead of stale overlay drawing",
            "const note::NoteTextCore* core = CurrentNoteTextCore();": "pending render-canvas expansion and local geometry must use the synchronized TextCore",
            "currentLogicalStartsForStale": "stale paint must reuse its frame-local logical-line mapping",
            "QueryEditorLogicalLineRange(hWnd, line, lineCount, &lineStart": "stable link lookup must use the current editor-line range",
            "lineForRawIndex(*nativeHitRaw)": "stable hit testing must map its selected offset through the local editor-line adapter",
            "editorLineLayout.HitTest(clientX, clientY)": "structured hit testing must share EditorLineLayout geometry",
            "selectionSegmentRangeRect(": "structured selection must share EditorLineLayout geometry",
            "selectionSegmentBoundaryScratch.push_back({segStart, segmentX})": "anchored selection must begin at its own rendered run position",
            "note::EditorLineLayout segmentLayout": "anchored selection must use a local shared-layout view",
            "BuildRenderedSegmentPlacements(hWnd, hdc, line,": "structured horizontal extent must share run placement geometry",
            "PaintNativeNoteClient(hWnd, hdc, nativeRawRegion);": "structured paint must clip native RichEdit text to NativeRaw lines",
            "ResolveNoteNativePaintScope(frameAction)": "native paint scope must come from the presentation policy",
            "CanReuseCommittedLayoutForImeComposition(hWnd)": "hybrid IME painting must require an explicit local-layout proof",
            "g_noteRenderStaleLines.first != g_noteRenderStaleLines.last": "IME hybrid reuse must reject multi-line stale ranges",
            "bindingCurrentForPresentation": "a proven IME anchor must be the only alternate to a current TextCore binding",
            "TryGetNoteImeCompositionAnchorLine(hWnd, &imeAnchorLine)": "IME native ownership must remain on the anchored logical row",
            "const bool imeComposing = IsNoteImeComposing();": "frame presentation must use the event-owned IME phase rather than a second IMM poll",
        },
        "src/note/note_presentation.cpp": {
            "NoteNativePaintScope ResolveNoteNativePaintScope": "native text paint scope must be a pure presentation policy",
            "NotePresentationPlan ResolveNotePresentationPlan": "frame ownership must be resolved by the pure presentation plan",
            "EditorLineLayout::TextRangeRect": "shared editor-line geometry must provide source-range rectangles",
            "EditorLineLayout::HitTest": "shared editor-line geometry must provide hit testing",
            "state.ime_composition_can_reuse_committed_layout": "stale IME composition must not reuse committed lines without adapter proof",
            "NoteImePresentationPhase::ResultCommittedAwaitingEnd": "a pure IME phase machine must preserve acknowledged results until WM_IME_END",
        },
        "src/ui/core/main_window_proc.cppinc": {
            "ExpandNoteRenderCanvasForPendingEdit(g_hNoteEdit);": "legacy rendered mode must retain its bounded pending raw-helper canvas update",
            "The final route": "the final route must document that pending raw-helper canvas expansion is intentionally bypassed",
            "NoteEditorTextMutationObserved(g_hNoteEdit);": "every RichEdit EN_CHANGE must invalidate the zero-copy TextCore binding first",
        },
        "src/note_view/note_view_input_helpers.cppinc": {
            "DrawNoteEditVisualAssistOverlay": "note visual assists must have one paint entry point",
            "GetEditTextRangeForIndexing(hWnd, lineStart, lineEnd)": "visible tab/whitespace assists must read only the display-line range",
            "FinalFrameCanUseCustomPaint(hWnd)": "IME caret ownership must first use the current final-frame contract",
            "TryGetFinalNoteRenderCaretRect(hWnd, outRect, outInputBand)": "IME geometry must come from the same final frame as glyph painting",
            "WithdrawFinalNoteRenderFrame(hWnd);": "an unresolved final caret must withdraw the frame rather than reuse legacy geometry",
            "GetNoteImeCompositionSnapshot": "IME preedit text and caret must be captured together without reading the complete editor",
        },
        "src/note_view/note_view_input_proc.cppinc": {
            "BeginNoteImeCompositionPresentationAnchor(hWnd);": "IME start must capture the preedit's committed row before RichEdit mutates it",
            "UpdateNoteImeCompositionPresentationAnchor(hWnd, composition.text);": "IME updates must publish only bounded preedit geometry",
            "ClearNoteImeCompositionPresentationAnchor(hWnd);": "IME completion must discard the temporary anchor before markup refresh",
            "note::NoteImePresentationEvent::ResultCommitted": "GCS_RESULTSTR must end provisional IME presentation before RichEdit emits EN_CHANGE",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    shared_text = (REPO_ROOT / "src/note_view/note_view_shared.cppinc").read_text(encoding="utf-8", errors="ignore")
    incremental_pos = shared_text.find("TryRefreshMarkdownRenderCacheForDirtyGraph")
    fallback_pos = shared_text.find("RebuildMarkdownRenderCache(text);", incremental_pos)
    if incremental_pos < 0 or fallback_pos < 0 or incremental_pos > fallback_pos:
        errors.append("src/note_view/note_view_shared.cppinc: dirty-graph incremental render refresh must be attempted before full render cache rebuild")
    ops_text = (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(encoding="utf-8", errors="ignore")
    expand_pos = ops_text.find("void ExpandNoteRenderCanvasForPendingEdit")
    next_measure_pos = ops_text.find("static int MeasureRenderedContentBottomPx", expand_pos)
    if expand_pos < 0 or next_measure_pos < 0:
        errors.append("src/note_view/note_view_note_ops.cppinc: pending render-canvas expansion must remain an isolated helper")
    else:
        expand_text = ops_text[expand_pos:next_measure_pos]
        if "GetEditTextForIndexing(hWnd)" in expand_text or "BuildLogicalLineStartsFromIndexingText" in expand_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: pending render-canvas expansion must not copy or split the full note")
    helper_text = (REPO_ROOT / "src/note_view/note_view_input_helpers.cppinc").read_text(encoding="utf-8", errors="ignore")
    assist_pos = helper_text.find("static void DrawNoteEditVisualAssistOverlay")
    next_helper_pos = helper_text.find("// ---------------------------------------------------------------------\n// NoteEdit", assist_pos)
    if assist_pos < 0 or next_helper_pos < 0:
        errors.append("src/note_view/note_view_input_helpers.cppinc: visual assist paint must remain an isolated helper")
    elif "GetEditTextForIndexing(hWnd)" in helper_text[assist_pos:next_helper_pos]:
        errors.append("src/note_view/note_view_input_helpers.cppinc: visual assist paint must not read the full note")
    ime_pos = helper_text.find("static bool TryGetNoteImeCaretRect")
    ime_end = helper_text.find("void SyncNoteImeCandidateWindowToCaret", ime_pos)
    if ime_pos < 0 or ime_end < 0:
        errors.append("src/note_view/note_view_input_helpers.cppinc: IME caret resolution must remain an isolated helper")
    else:
        ime_text = helper_text[ime_pos:ime_end]
        if "GetNoteLogicalLineStarts(hWnd" in ime_text:
            errors.append("src/note_view/note_view_input_helpers.cppinc: IME caret resolution must not rebuild a full line index")
        if "CurrentNoteTextCoreModel()" in ime_text:
            errors.append("src/note_view/note_view_input_helpers.cppinc: IME caret resolution must not materialize the canonical model")
        if "GetWindowTextLengthW(hWnd)" in ime_text:
            errors.append("src/note_view/note_view_input_helpers.cppinc: IME caret resolution must not use text length as a TextCore identity proof")
    overlay_pos = ops_text.find("static void DrawHighlightOverlay")
    next_measure_pos = ops_text.find("static int MeasureRenderedLineVisualWidth", overlay_pos)
    if overlay_pos < 0 or next_measure_pos < 0:
        errors.append("src/note_view/note_view_note_ops.cppinc: highlight overlay must remain an isolated paint helper")
    else:
        overlay_text = ops_text[overlay_pos:next_measure_pos]
        if "GetNoteLogicalLineStarts(hWnd" in overlay_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: current highlight paint must reuse TextCore/frame line indices")
        if "CurrentNoteTextCoreModel()" in overlay_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: highlight paint must not materialize the canonical model for line geometry")
        if "GetEditTextForIndexing(hWnd)" in overlay_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: highlight paint must not copy the complete editor for a stale row")
        if "QueryEditorLogicalLineBand(hWnd" not in overlay_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: highlight paint must query current line geometry through the local editor-line adapter")
        if "BuildNoteRawLineSurfaceResolver(hWnd)" not in overlay_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: visible line surfaces must share one raw-line resolver per paint")
        if "IsNoteLineRawHelper(hWnd, line)" in overlay_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: visible line surfaces must not reread selection state per line")
    link_pos = ops_text.find("static std::optional<std::wstring> LinkIdAtCharIndex")
    rendered_caret_pos = ops_text.find("static bool TryGetRenderedCaretRect")
    raw_caret_pos = ops_text.find("static bool TryGetRawCaretRect", rendered_caret_pos)
    if rendered_caret_pos < 0 or raw_caret_pos < 0 or link_pos < 0:
        errors.append("src/note_view/note_view_note_ops.cppinc: rendered and raw caret geometry must remain isolated helpers")
    else:
        rendered_caret_text = ops_text[rendered_caret_pos:raw_caret_pos]
        raw_caret_text = ops_text[raw_caret_pos:link_pos]
        if ("GetNoteLogicalLineStarts(hWnd" in rendered_caret_text or
                "CurrentNoteTextCoreModel()" in rendered_caret_text or
                "RefreshMarkdownRenderCache(" in rendered_caret_text):
            errors.append("src/note_view/note_view_note_ops.cppinc: rendered caret geometry must use the local editor-line adapter")
        if ("GetNoteLogicalLineStarts(hWnd" in raw_caret_text or
                "CurrentNoteTextCoreModel()" in raw_caret_text):
            errors.append("src/note_view/note_view_note_ops.cppinc: raw caret geometry must use the local editor-line adapter")
    native_hit_pos = ops_text.find("static std::optional<size_t> HitTestNativeNoteRawPosition", link_pos)
    if link_pos < 0 or native_hit_pos < 0:
        errors.append("src/note_view/note_view_note_ops.cppinc: link lookup must remain an isolated helper")
    elif ("GetNoteLogicalLineStarts(hWnd" in ops_text[link_pos:native_hit_pos] or
            "CurrentNoteTextCoreModel()" in ops_text[link_pos:native_hit_pos]):
        errors.append("src/note_view/note_view_note_ops.cppinc: stable link lookup must use the local editor-line adapter")
    hit_pos = ops_text.find("static std::optional<size_t> HitTestMarkupPosition")
    next_math_pos = ops_text.find("static mathrender::Layout MeasureMathLineLayout", hit_pos)
    if hit_pos < 0 or next_math_pos < 0:
        errors.append("src/note_view/note_view_note_ops.cppinc: markup hit testing must remain an isolated helper")
    else:
        hit_text = ops_text[hit_pos:next_math_pos]
        if ("GetNoteLogicalLineStarts(hWnd" in hit_text or
                "CurrentNoteTextCoreModel()" in hit_text or
                "GetEditTextForIndexing(hWnd)" in hit_text):
            errors.append("src/note_view/note_view_note_ops.cppinc: stable markup hit testing must not materialize the complete note")
        if ("QueryEditorLogicalLineBand(hWnd" not in hit_text or
                "QueryEditorLogicalLineRange(hWnd" not in hit_text):
            errors.append("src/note_view/note_view_note_ops.cppinc: stable markup hit testing must use the local editor-line adapter")
    boundary_pos = shared_text.find("static bool BuildRenderedBoundariesForLineWithScratch")
    boundary_end = shared_text.find("static int FindLineForRaw", boundary_pos)
    if boundary_pos < 0 or boundary_end < 0:
        errors.append("src/note_view/note_view_shared.cppinc: rendered boundary generation must remain an isolated helper")
    elif ("GetNoteLogicalLineStarts(hWnd" in shared_text[boundary_pos:boundary_end] or
            "CurrentNoteTextCoreModel()" in shared_text[boundary_pos:boundary_end] or
            "QueryEditorLogicalLineRange(hWnd" not in shared_text[boundary_pos:boundary_end]):
        errors.append("src/note_view/note_view_shared.cppinc: rendered boundaries must use the local editor-line range")
    placement_pos = shared_text.find("static bool BuildRenderedSegmentPlacements")
    placement_end = shared_text.find("static bool BuildRenderedBoundariesForLineWithScratch", placement_pos)
    if placement_pos < 0 or placement_end < 0:
        errors.append("src/note_view/note_view_shared.cppinc: rendered segment placement must remain an isolated helper")
    else:
        placement_text = shared_text[placement_pos:placement_end]
        if ("GetNoteLogicalLineStarts(hWnd" in placement_text or
                "GetEditTextForIndexing(hWnd)" in placement_text):
            errors.append("src/note_view/note_view_shared.cppinc: rendered segment placement must not copy the complete editor")
        if "const RenderedLinePlacement* editorLineRange" not in placement_text:
            errors.append("src/note_view/note_view_shared.cppinc: rendered segment placement must accept a local editor-line range")
    find_line_pos = shared_text.find("static int FindLineForRaw(size_t raw) {")
    find_line_end = shared_text.find("static int FindBoundaryIndexAtOrBefore", find_line_pos)
    if find_line_pos < 0 or find_line_end < 0:
        errors.append("src/note_view/note_view_shared.cppinc: normal-mode line lookup must remain an isolated helper")
    else:
        find_line_text = shared_text[find_line_pos:find_line_end]
        if "CurrentNoteTextCoreModel()" in find_line_text:
            errors.append("src/note_view/note_view_shared.cppinc: normal-mode line lookup must not materialize the canonical model")
        if "EM_LINEFROMCHAR" not in find_line_text:
            errors.append("src/note_view/note_view_shared.cppinc: normal-mode line lookup must use current editor line coordinates")
    selection_pos = helper_text.find("static bool ResolveNoteLineSelectionHit")
    selection_end = helper_text.find("static bool SelectNoteLines", selection_pos)
    if selection_pos < 0 or selection_end < 0:
        errors.append("src/note_view/note_view_input_helpers.cppinc: line selection hit testing must remain an isolated helper")
    else:
        selection_text = helper_text[selection_pos:selection_end]
        if "GetNoteLogicalLineStarts(hWnd" not in selection_text:
            errors.append("src/note_view/note_view_input_helpers.cppinc: line selection hit testing must retain an unsynchronized fallback")
        if "CurrentNoteTextCoreModel()" in selection_text:
            errors.append("src/note_view/note_view_input_helpers.cppinc: stable line selection must use the local editor-line adapter")
        if "QueryEditorLogicalLineBand(hWnd" not in selection_text:
            errors.append("src/note_view/note_view_input_helpers.cppinc: stable line selection must query current row geometry locally")
    input_proc_text = (REPO_ROOT / "src/note_view/note_view_input_proc.cppinc").read_text(encoding="utf-8", errors="ignore")
    main_switch_pos = input_proc_text.find("    switch (msg) {", input_proc_text.find("if (IsNoteOpeningFocusVisualHoldActive"))
    paint_pos = input_proc_text.find("case WM_PAINT:", main_switch_pos)
    paint_end = input_proc_text.find("case WM_IME_STARTCOMPOSITION:", paint_pos)
    if paint_pos < 0 or paint_end < 0:
        errors.append("src/note_view/note_view_input_proc.cppinc: note paint must remain a bounded message handler")
    else:
        paint_text = input_proc_text[paint_pos:paint_end]
        if "SendMessageW(hWnd, WM_PRINTCLIENT" in paint_text:
            errors.append("src/note_view/note_view_input_proc.cppinc: native text printing must be owned by the presentation path")
        if "PaintNativeNoteClient(hWnd, paintDC);" not in paint_text:
            errors.append("src/note_view/note_view_input_proc.cppinc: resize drag must retain the full native fallback")
    resolver_pos = shared_text.find("static NoteRawLineSurfaceResolver BuildNoteRawLineSurfaceResolver")
    resolver_end = shared_text.find("static void BeginNoteOpeningFocusVisualHold", resolver_pos)
    if resolver_pos < 0 or resolver_end < 0:
        errors.append("src/note_view/note_view_shared.cppinc: raw line ownership must be resolved by one reusable value")
    else:
        resolver_text = shared_text[resolver_pos:resolver_end]
        if "IsCurrentNoteTextCoreSynchronizedWithEditor(hWnd)" not in resolver_text:
            errors.append("src/note_view/note_view_shared.cppinc: raw line ownership must prefer the synchronized TextCore")
        if "SendMessageW(hWnd, EM_GETSEL" not in resolver_text:
            errors.append("src/note_view/note_view_shared.cppinc: raw line ownership must capture selection once per resolver")
    frame_pos = ops_text.find("static NoteRenderPresentationFrame BuildNoteRenderPresentationFrame")
    frame_end = ops_text.find("static void RefreshNoteRenderPresentationFrameAfterCommit", frame_pos)
    if frame_pos < 0 or frame_end < 0:
        errors.append("src/note_view/note_view_note_ops.cppinc: presentation-frame resolution must remain isolated")
    else:
        frame_text = ops_text[frame_pos:frame_end]
        if "const bool textCoreEditorCurrent = IsCurrentNoteTextCoreSynchronizedWithEditor(hWnd);" not in frame_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: presentation-frame resolution must capture the TextCore/editor binding once")
        if "bool bindingCurrentForFrame = textCoreEditorCurrent;" not in frame_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: presentation-frame binding recovery must retain the initial binding proof")
        if ("captureStaleText &&\n"
                "            TryRepairNoteTextCoreEditorBindingForPaint(hWnd, &frame.currentTextForStale)" not in frame_text):
            errors.append("src/note_view/note_view_note_ops.cppinc: binding recovery must be paint-only and reuse its one text snapshot")
        if "GetEditTextForIndexing(hWnd)" in frame_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: presentation-frame line-count selection must not read the complete editor")
        if "core->logical_line_count() != g_lineCache.size()" not in frame_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: presentation-frame stale line count must use the current TextCore scalar")
        if "NotePresentationFrameState{renderActive, false, false, false, false, false," not in frame_text:
            errors.append("src/note_view/note_view_note_ops.cppinc: unbound TextCore/editor state must select whole-view native fallback")
    editor_line_pos = shared_text.find("static bool QueryEditorLogicalLineRange")
    editor_line_end = shared_text.find("static bool InvalidateNoteRenderDirtyLines", editor_line_pos)
    if editor_line_pos < 0 or editor_line_end < 0:
        errors.append("src/note_view/note_view_shared.cppinc: local editor-line geometry must remain an isolated helper")
    else:
        editor_line_text = shared_text[editor_line_pos:editor_line_end]
        if ("EM_LINEINDEX" not in editor_line_text or
                "EM_LINELENGTH" not in editor_line_text or
                "GetCharPos" not in editor_line_text):
            errors.append("src/note_view/note_view_shared.cppinc: local editor-line geometry must use RichEdit line and position APIs")
        if "GetEditTextForIndexing(hWnd)" in editor_line_text or "CurrentNoteTextCoreModel()" in editor_line_text:
            errors.append("src/note_view/note_view_shared.cppinc: local editor-line geometry must not materialize the complete note")
    main_window_text = (REPO_ROOT / "src/ui/core/main_window_proc.cppinc").read_text(
        encoding="utf-8", errors="ignore")
    if "static void RequeueNoteOverlayRefreshForCurrentRevision(HWND hWnd)" not in main_window_text:
        errors.append("src/ui/core/main_window_proc.cppinc: deferred overlay refresh must retain a retry owner")
    if main_window_text.count("RequeueNoteOverlayRefreshForCurrentRevision(g_hMainWnd);") != 1:
        errors.append("src/ui/core/main_window_proc.cppinc: only a live IME may requeue a timer-level overlay refresh")
    if "RequeueNoteOverlayRefreshForCurrentRevision(hWnd);" not in main_window_text:
        errors.append("src/ui/core/main_window_proc.cppinc: input changes during IME must schedule a post-composition overlay refresh")
    if ("if (immediate && targetRevision != 0 && targetRevision != CurrentEditRevision())" not in
            main_window_text):
        errors.append("src/ui/core/main_window_proc.cppinc: stale immediate overlay requests must normalize at scheduling")
    ime_input_text = (REPO_ROOT / "src/note_view/note_view_input_proc.cppinc").read_text(
        encoding="utf-8", errors="ignore")
    result_pos = ime_input_text.find("case WM_IME_COMPOSITION: {")
    default_proc_pos = ime_input_text.find(
        "LRESULT res = DispatchNoteImeToNative(hWnd, msg, wParam, lParam);", result_pos)
    result_refresh_pos = ime_input_text.find(
        "PostMessageW(GetParent(hWnd), kMsgNoteOverlayUpdate, 1,", default_proc_pos)
    if result_pos < 0 or default_proc_pos < 0 or result_refresh_pos < default_proc_pos:
        errors.append("src/note_view/note_view_input_proc.cppinc: IME result must request markup only after RichEdit accepts canonical text")
    if ("IsNoteImeCanonicalChangeDeferred(g_hNoteEdit)" not in main_window_text or
            "AcceptNoteImeResultWithContinuation(hWnd, imeResult, composition)" not in ime_input_text):
        errors.append("IME preedit EN_CHANGE must defer canonical edits and accept continuing results through the proven result-only boundary")
    change_pos = main_window_text.find("if (from == g_hNoteEdit && code == EN_CHANGE) {")
    change_end = main_window_text.find("if (from == g_hAnnotSummary", change_pos)
    change_text = main_window_text[change_pos:change_end]
    acknowledgement_pos = change_text.find("const auto acceptedChange = ObserveCurrentNoteTextChange(g_hNoteEdit);")
    revision_pos = change_text.find("NotifyEditRevisionChanged();")
    if (change_pos < 0 or change_end < 0 or acknowledgement_pos < 0 or revision_pos < acknowledgement_pos or
            "acceptedChange == NoteCanonicalTextChangeResult::Unchanged" not in change_text or
            "acceptedChange == NoteCanonicalTextChangeResult::Deferred" not in change_text):
        errors.append("note EN_CHANGE must acknowledge canonical mutation before revisions/staging and preserve unchanged/deferred state")
    return errors


def find_note_ime_layout_coordinate_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/note_view/note_view_shared.cppinc": {
            "struct NoteLayoutFrame": "note layout coordinates must be centralized in NoteLayoutFrame",
            "frame.rawLeftPx = margins.desiredLeft - frame.scrollX;": "render-mode raw text X must include the render horizontal scroll offset",
            "frame.overlayLeftPx = margins.desiredLeft - frame.scrollX;": "render overlay X must use the same margin/scroll basis as raw text",
            "NoteLayoutFrameLineBaseXForRenderState": "render overlay line base X must come from the shared layout frame",
            "NoteLayoutFrameClientXFromNativeLineX": "native RichEdit X must be translated through the shared layout frame",
            "NoteLayoutFrameAnchorScrollX": "scroll anchoring must use render scroll in render mode and native scroll otherwise",
            "UpdateNoteVisualAssistMargins(hWnd, false);": "format rect updates must apply visual-assist margins before building the layout frame",
            "g_noteRenderScrollX = layoutFrame.scrollX;": "format rect updates must commit the clamped render horizontal scroll",
        },
        "src/note_view/note_view_input_helpers.cppinc": {
            "TryGetNoteImeCaretRect": "IME candidate positioning must use one caret-rect resolver",
            "FinalFrameCanUseCustomPaint(hWnd)": "IME caret resolution must prefer the published final frame when it paints the client",
            "TryGetFinalNoteRenderCaretRect(hWnd, outRect, outInputBand)": "IME caret geometry must use the final frame for both raw and structured surfaces",
            "WithdrawFinalNoteRenderFrame(hWnd);": "a failed final caret lookup must choose whole-native fallback rather than legacy geometry",
            "if (!TryGetNativeNoteCaretRect(hWnd, outRect)) return false;": "IME caret resolution must fall back to native RichEdit geometry",
            "*outInputBand = *outRect;": "whole-native IME input band must use the actual native caret geometry",
            "ImmSetCompositionWindow(himc, &comp)": "IME composition window must be positioned from the resolved caret rect",
            "ImmSetCandidateWindow(himc, &cand)": "IME candidate window must use the resolved input-band exclusion",
            "note::ResolveNoteImeCandidatePlacement": "IME candidate exclusion must protect visible input and DPI-scaled clearance",
            "frame->ime_decorations()": "wrapped preedit exclusion must use the same measured fragments as painting",
            "note::NoteImeClientRect band{inputBand.left, inputBand.top, inputBand.right, inputBand.bottom}": "IME exclusion must use the allocated input band rather than just glyph height",
            "context.imeCandidateWindowSyncInProgress": "IMM positioning must guard synchronous notification reentry",
            "ScopedImeContextRelease": "IMM positioning must release its borrowed context on every return path",
            "if (ShouldSyncNoteImeAfterDefaultMessage(msg))": "prediction positioning must not require a live composition session",
            "SetCaretPos(rc.left, rc.top)": "native caret must be synchronized to the resolved render/raw caret rect",
            "GetGUIThreadInfo(GetCurrentThreadId(), &info)": "native caret synchronization must inspect the current focus lifetime rather than cached shape",
            "SPI_GETCARETWIDTH": "final caret width must respect the Windows accessibility setting",
            "if (!positioned || !shown) requireNativeFallback();": "failed native caret placement must withdraw final geometry without changing text",
        },
        "src/note_view/note_view_final_render.cppinc": {
            "paint.paint_caret = false;": "runtime final paint must not duplicate the Windows caret",
            "frame->Caret(&caret)": "final IME caret geometry must be resolved from the immutable published frame",
            "clientY(line->bottom_px)": "IME input band must include preserved raw row height from the final allocation",
            "context.finalRenderScrollY": "final caret geometry must use the same vertical scroll origin as painting",
            "FinalRawTextIndexToRichEditIndex": "final source positions must be translated to RichEdit only at the input transport edge",
        },
        "src/note_view/note_view_input_proc.cppinc": {
            "SyncNoteImeCandidateWindowToCaret(hWnd);": "note input procedure must resync IME candidate position after IME and scroll-affecting events",
            "case WM_IME_NOTIFY: {": "prediction open/change notifications must synchronize their placement post-default",
            "wParam == IMN_OPENCANDIDATE || wParam == IMN_CHANGECANDIDATE": "candidate notifications must include both initial display and updates",
            "if (g_noteImeComposing) SyncNoteImeCandidateWindowToCaret(hWnd);": "scroll and layout changes during composition must resync the IME candidate window",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    helper_text = (REPO_ROOT / "src/note_view/note_view_input_helpers.cppinc").read_text(encoding="utf-8", errors="ignore")
    resolver_pos = helper_text.find("static bool TryGetNoteImeCaretRect")
    sync_pos = helper_text.find("void SyncNoteImeCandidateWindowToCaret", resolver_pos)
    composition_pos = helper_text.find("ImmSetCompositionWindow", sync_pos)
    candidate_pos = helper_text.find("ImmSetCandidateWindow", sync_pos)
    if resolver_pos < 0 or sync_pos < 0 or composition_pos < 0 or candidate_pos < 0 or resolver_pos > sync_pos or composition_pos > candidate_pos:
        errors.append("src/note_view/note_view_input_helpers.cppinc: IME candidate sync must use the shared caret rect resolver before setting composition/candidate windows")
    snapshot_pos = helper_text.find("static bool GetNoteImeCompositionSnapshot")
    snapshot_end = helper_text.find("static bool MoveNoteCaretIntoCommittedFullWidthParenPair", snapshot_pos)
    if snapshot_pos < 0 or snapshot_end < 0:
        errors.append("src/note_view/note_view_input_helpers.cppinc: IMM snapshot reading must remain a bounded helper")
    else:
        snapshot_text = helper_text[snapshot_pos:snapshot_end]
        if "lParam" in snapshot_text:
            errors.append("src/note_view/note_view_input_helpers.cppinc: IMM field availability must not depend on composition change flags")
        for needle in ("ScopedImeContextRelease", "if (copied != size) return false;", "GCS_CURSORPOS", "GCS_COMPATTR", "GCS_COMPCLAUSE", "catch (const std::exception&)"):
            if needle not in snapshot_text:
                errors.append("src/note_view/note_view_input_helpers.cppinc: IMM text/cursor snapshot must release its context and reject partial reads or allocation failure")
                break
    input_text = (REPO_ROOT / "src/note_view/note_view_input_proc.cppinc").read_text(encoding="utf-8", errors="ignore")
    ime_pos = input_text.find("case WM_IME_COMPOSITION: {")
    default_pos = input_text.find("CallWindowProcW(g_oldNoteProc", ime_pos)
    read_pos = input_text.find("GetNoteImeCompositionSnapshot", ime_pos)
    observe_pos = input_text.find("note::NoteImePresentationEvent::ObservedCompositionText", ime_pos)
    if min(ime_pos, default_pos, read_pos, observe_pos) < 0 or not (read_pos < default_pos and observe_pos < default_pos):
        errors.append("src/note_view/note_view_input_proc.cppinc: IMM preedit must be normalized before RichEdit's nested EN_CHANGE")
    focus_pos = input_text.find("case WM_KILLFOCUS: {")
    focus_end = input_text.find("case WM_VSCROLL:", focus_pos)
    focus_text = input_text[focus_pos:focus_end]
    settle_pos = focus_text.find("DispatchNoteImeToNative(hWnd, msg, wParam, lParam)")
    reset_pos = focus_text.find("EndFinalNoteImePreedit(hWnd)")
    flush_pos = focus_text.find("MaybeFlushNoteStageOnCaretOrFocusChange")
    if min(focus_pos, focus_end, settle_pos, reset_pos, flush_pos) < 0 or not (settle_pos < reset_pos < flush_pos) or "return result;" not in focus_text:
        errors.append("src/note_view/note_view_input_proc.cppinc: focus loss must settle native input once, close the preedit session, then flush canonical stage")
    ops_text = (REPO_ROOT / "src/note_view/note_view_note_ops.cppinc").read_text(encoding="utf-8", errors="ignore")
    rendered_pos = ops_text.find("static bool TryGetRenderedCaretRect")
    raw_pos = ops_text.find("static bool TryGetRawCaretRect")
    if rendered_pos < 0 or raw_pos < 0 or rendered_pos > raw_pos:
        errors.append("src/note_view/note_view_note_ops.cppinc: rendered caret rect should remain the primary geometry path before raw fallback")
    return errors

def find_layout_flicker_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/ui/splitter.cpp": {
            "SetTimer(hWnd, kSplitDragTimerId, 16, nullptr)": "splitter drag must coalesce live layout updates instead of relaying every mouse move directly",
            "ApplyLayout(hWnd, LayoutPass::Live)": "splitter drag timer must apply a live layout pass",
            "ApplyLayout(hWnd, LayoutPass::Commit)": "splitter drag end must settle with a commit layout pass",
        },
        "src/ui/layout.cpp": {
            "UINT baseFlags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW;": "layout moves must suppress per-child redraw during batched repositioning",
            "baseFlags |= SWP_DEFERERASE;": "live layout must defer background erase during splitter drags",
            "BeginDeferWindowPos(static_cast<int>(batch.ops.size()))": "layout must batch child moves by parent",
            "DeferWindowPos(hdwp, op.hwnd, nullptr, op.x, op.y, op.w, op.h, op.flags)": "layout must use DeferWindowPos for batched child moves",
            "EndDeferWindowPos(hdwp)": "layout must commit child move batches atomically",
            "RedrawLayoutRegion(hWnd, &result.rootDirtyRect)": "layout must redraw only the union of changed old/new bounds when possible",
            "result.bottomPaneBoundsChanged && pass == LayoutPass::Commit": "bottom pane refresh must be tied to committed bounds changes",
            "RefreshCurrentNoteBottomPane();": "bottom pane refresh must occur after committed layout changes",
        },
        "src/ui/core/main_window_proc.cppinc": {
            "case WM_ERASEBKGND:": "main window must handle background erase explicitly",
            "return 1;": "main window must suppress background erase flicker",
            "LayoutChildrenLive(hWnd);": "WM_SIZE during live resize must use live layout",
            "RedrawWorkspaceLayout(hWnd);": "committed resize/splitter changes must settle with a full workspace redraw",
        },
        "src/pdf_view/zoom_flow.cppinc": {
            "g_zoomAnchor.valid = true;": "PDF zoom must capture a page-local anchor",
            "g_zoomAnchor.focusClient = focusInClient;": "PDF zoom anchor must remember the client focus point",
            "ApplyPdfScaleDirect(pdfWnd, newScale, focusInClient, false);": "PDF zoom must update layout immediately without forcing page rerender on the input event",
            "SetTimer(pdfWnd, kZoomTimerId, kZoomTimerIntervalMs, nullptr);": "PDF zoom rerender must be delayed through the zoom timer",
            "g_pdf.scrollX = docX + padX - g_zoomAnchor.focusClient.x;": "PDF zoom must restore horizontal scroll from the captured page anchor",
            "g_pdf.scrollY = docY + padY - g_zoomAnchor.focusClient.y;": "PDF zoom must restore vertical scroll from the captured page anchor",
            "InvalidateRect(pdfWnd, nullptr, FALSE);": "PDF zoom repaint must avoid erase background",
        },
        "src/settings/settings_common.cppinc": {
            "struct SettingsApplyEffects": "settings apply must classify update effects before touching UI",
            "if (effects.layout)": "settings apply must request layout only when layout-affecting settings changed",
            "if (effects.refreshBottomPane)": "settings apply must refresh bottom pane only when bottom-pane settings changed",
            "InvalidateRect(g_hPdfView, nullptr, FALSE)": "settings PDF invalidation must avoid erase background",
        },
        "src/settings/settings_general_controls.cppinc": {
            "effects.layout = pinChanged || noteModeChanged ||": "general settings must restrict layout refresh to layout-affecting changes",
            "effects.refreshBottomPane = pinChanged || noteModeChanged;": "bottom assist/right-bottom refresh must be separated from unrelated settings",
            "effects.invalidatePdfView = selectionStyleChanged || pdfFlowModeChanged || pointerOffsetChanged;": "PDF repaint must be separated from general settings layout changes",
        },
    }
    for rel, needles in required_by_file.items():
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")

    layout_text = (REPO_ROOT / "src/ui/layout.cpp").read_text(encoding="utf-8", errors="ignore")
    no_redraw_pos = layout_text.find("SWP_NOREDRAW")
    defer_pos = layout_text.find("BeginDeferWindowPos")
    redraw_pos = layout_text.find("RedrawLayoutRegion(hWnd, &result.rootDirtyRect)")
    if no_redraw_pos < 0 or defer_pos < 0 or redraw_pos < 0 or not (no_redraw_pos < defer_pos < redraw_pos):
        errors.append("src/ui/layout.cpp: child moves must be batched with no-redraw before a single dirty-region redraw")

    zoom_text = (REPO_ROOT / "src/pdf_view/zoom_flow.cppinc").read_text(encoding="utf-8", errors="ignore")
    anchor_pos = zoom_text.find("g_zoomAnchor.valid = true;")
    immediate_pos = zoom_text.find("ApplyPdfScaleDirect(pdfWnd, newScale, focusInClient, false);")
    timer_pos = zoom_text.find("SetTimer(pdfWnd, kZoomTimerId, kZoomTimerIntervalMs, nullptr);")
    if anchor_pos < 0 or immediate_pos < 0 or timer_pos < 0 or not (anchor_pos < immediate_pos < timer_pos):
        errors.append("src/pdf_view/zoom_flow.cppinc: PDF zoom must capture anchor, apply viewport layout, then defer rerender")

    settings_text = (REPO_ROOT / "src/settings/settings_general_controls.cppinc").read_text(encoding="utf-8", errors="ignore")
    layout_effect_pos = settings_text.find("effects.layout = pinChanged || noteModeChanged ||")
    bottom_effect_pos = settings_text.find("effects.refreshBottomPane = pinChanged || noteModeChanged;")
    pdf_effect_pos = settings_text.find("effects.invalidatePdfView = selectionStyleChanged || pdfFlowModeChanged || pointerOffsetChanged;")
    if layout_effect_pos < 0 or bottom_effect_pos < 0 or pdf_effect_pos < 0:
        errors.append("src/settings/settings_general_controls.cppinc: settings apply effects must keep layout, bottom pane, and PDF invalidation separate")

    return errors
def find_note_save_history_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/note/note_kernel.h": {
            "void ClearHistory();": "the note kernel must expose explicit history clearing",
        },
        "src/note/note_kernel.cpp": {
            "void LocalNoteKernel::ClearHistory()": "the note kernel must implement history clearing",
            "history_.Clear();": "kernel history clearing must clear both undo and redo stacks",
        },
        "src/note_view/note_view_note_ops.cppinc": {
            "kernel->ClearHistory();": "clearing the current note history must clear the kernel history",
            "EM_EMPTYUNDOBUFFER": "clearing the current note history must clear RichEdit history",
        },
        "src/features/automation/main_ui_automation.cppinc": {
            "Direct note save did not clear note undo/redo history.": "UI automation must reject undo/redo surviving a direct note save",
            "RunUiAutomationNoteIntegratedSaveHistoryScenario": "UI automation must cover integrated-save history clearing",
            "Integrated note save did not establish an undo/redo history boundary.": "UI automation must reject undo/redo surviving an integrated save",
        },
    }
    for rel, needles in required_by_file.items():
        text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")

    stage_text = (REPO_ROOT / "src/file_output/file_output_stage.cpp").read_text(encoding="utf-8", errors="ignore")
    direct_start = stage_text.find("bool SaveNoteFile(HWND owner)")
    direct_end = stage_text.find("bool SaveNoteIfDirty(HWND owner)", direct_start)
    direct_save = stage_text[direct_start:direct_end] if direct_start >= 0 and direct_end >= 0 else ""
    if direct_save.count("ClearCurrentNoteUndoHistory();") < 2:
        errors.append("src/file_output/file_output_stage.cpp: every successful direct note-save path must clear undo/redo history")

    integrate_start = stage_text.find("bool IntegrateStageMetaToDestination")
    integrate_end = stage_text.find("struct BackgroundSaveWorkerResult", integrate_start)
    integrate_save = stage_text[integrate_start:integrate_end] if integrate_start >= 0 and integrate_end >= 0 else ""
    if "ClearCurrentNoteUndoHistory();" not in integrate_save:
        errors.append("src/file_output/file_output_stage.cpp: foreground stage integration must clear current-note undo/redo history")

    background_start = stage_text.find("for (const auto &entry : result->integrated)")
    background_end = stage_text.find("completion.ok = result->ok;", background_start)
    background_save = stage_text[background_start:background_end] if background_start >= 0 and background_end >= 0 else ""
    if "ClearCurrentNoteUndoHistory();" not in background_save:
        errors.append("src/file_output/file_output_stage.cpp: background stage integration must clear current-note undo/redo history")
    return errors

def find_multi_instance_launch_regressions() -> list[str]:
    errors: list[str] = []
    required_by_file = {
        "src/app/startup_instance.cpp": {
            'CanonicalPackageKey()': "single-instance scope must derive from the package setup path",
            'L"pdf_note_workspace_setup.json"': "single-instance scope must include the adjacent setup file",
            'PackageInstanceSuffix()': "mutex and events must be namespaced per package",
            'CanonicalPackageKeyForExecutablePath': "package identity must be reusable for target-process validation",
            'QueryFullProcessImageNameW': "window activation must verify the target process package",
            'static_cast<WPARAM>(GetCurrentProcessId())': "single-instance document IPC must identify its sending package process",
        },
        "src/app/bootstrap.cppinc": {
            'CreateMutexW(nullptr, TRUE, mutexName.c_str())': "main windows must keep normal single-instance activation behavior",
            'EnumWindows(FindCurrentPackageMainWindowProc': "activation must search only current-package windows",
            'IsProcessInCurrentMainPackage(processId)': "activation must reject windows from another package",
            'ReleaseAllDocumentOpenLocks();': "application shutdown must release document locks",
        },
        "src/workspace/workspace_write_lock.cpp": {
            'WorkspaceOperationLock::WorkspaceOperationLock': "shared workspace operations must acquire a short-lived lock",
            'g_workspaceWriteLockKey = key;': "same-workspace windows must register a normalized workspace key without lifetime exclusion",
            'DocumentOpenLockCandidate::DocumentOpenLockCandidate': "opened files must acquire a process-wide document lock",
            'Local\\\\PdfNoteDocumentOpenLock_': "opened files must use a distinct named mutex",
            'ReleaseAllDocumentOpenLocks': "all document locks must be released during shutdown",
            'ShouldNotifyDocumentOpenLockFailure': "startup-driven opens must be able to suppress only their conflict notice",
            'g_suppressedDocumentOpenLockConflictSequence': "startup restore must identify a suppressed document conflict",
        },
        "src/pdf_view/annotation_store.cppinc": {
            'DocumentOpenLockCandidate documentLock': "PDF and image opens must acquire the document lock",
            'documentLock.CommitReplacing': "a successful PDF/image switch must release only the previous document lock",
            'ShouldNotifyDocumentOpenLockFailure()': "PDF conflict notices must honor startup-only suppression",
        },
        "src/note_view/note_view_note_ops.cppinc": {
            'DocumentOpenLockCandidate documentLock': "note opens must acquire the document lock",
            'documentLock.CommitReplacing': "a successful note switch must release only the previous document lock",
            'ShouldNotifyDocumentOpenLockFailure()': "note conflict notices must honor startup-only suppression",
        },
        "src/main.cpp": {
            'ScopedDocumentOpenLockNoticeSuppression suppressDocumentOpenLockNotice;': "startup document opens must suppress only their conflict notice",
            '!suppressDocumentOpenLockNotice.sawSuppressedConflict()': "startup restore must not replace a suppressed conflict with a partial-restore notice",
        },
        "src/pdf_view/navigation.cppinc": {
            'ReleaseDocumentOpenLock(std::filesystem::path(g_pdf.path))': "closing a PDF/image must release its document lock",
        },
        "src/core/app_core.cpp": {
            'WorkspaceOperationLock workspaceLock(path.parent_path(), &workspaceLockError)': "workspace configuration writes must be serialized",
        },
        "src/file_output/file_output_stage.cpp": {
            'WorkspaceOperationLock workspaceLock(std::filesystem::path(g_workspaceRoot), &workspaceLockError)': "stage integration must be serialized per workspace",
        },
        "src/workspace/workspace_actions.cpp": {
            'WorkspaceOperationLock workspaceLock(std::filesystem::path(g_workspaceRoot), &workspaceLockError)': "workspace imports must be serialized",
        },
        "src/workspace/file_ops_move_rename.cppinc": {
            'WorkspaceOperationLock workspaceLock(std::filesystem::path(g_workspaceRoot), &workspaceLockError)': "workspace moves and renames must be serialized",
        },
    }
    for rel, needles in required_by_file.items():
        text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="ignore")
        for needle, message in needles.items():
            if needle not in text:
                errors.append(f"{rel}: {message}")
    forbidden_by_file = {
        "src/app/startup_instance.cpp": (
            'L"--new-instance"', 'L"--choose-workspace"', 'CreateProcessW(executable.c_str()'),
        "src/app/bootstrap.cppinc": ('newInstanceRequested', 'FindWindowW(kMainClass, nullptr)'),
        "src/ui/menus/menu_build.cpp": ('ID_FILE_NEW_WINDOW',),
        "src/app/command_dispatch.cppinc": ('ID_FILE_NEW_WINDOW', 'LaunchNewMainWindow'),
        "src/ui/core/main_window_proc.cppinc": ('TryGetStartupWorkspaceRoot', 'ShouldChooseStartupWorkspace'),
    }
    for rel, needles in forbidden_by_file.items():
        text = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="ignore")
        for needle in needles:
            if needle in text:
                errors.append(f"{rel}: general main-window multi-instance route must remain removed ({needle})")
    main_proc_text = (REPO_ROOT / "src/ui/core/main_window_proc.cppinc").read_text(encoding="utf-8", errors="ignore")
    ipc_contract = (
        "IsProcessInCurrentMainPackage(static_cast<DWORD>(senderValue))",
        "text[charCount - 1] != L'\\0'",
        "std::find(text, text + charCount - 1, L'\\0')",
    )
    if any(needle not in main_proc_text for needle in ipc_contract):
        errors.append("src/ui/core/main_window_proc.cppinc: startup document IPC must authenticate package scope and reject malformed strings")
    viewer_text = (REPO_ROOT / "src/readonly_viewer/main.cpp").read_text(encoding="utf-8", errors="ignore")
    viewer_ipc_contract = (
        "charCount > 32768",
        "std::find(pathData, pathData + charCount - 1, L'\\0')",
    )
    if any(needle not in viewer_text for needle in viewer_ipc_contract):
        errors.append("src/readonly_viewer/main.cpp: tab-transfer IPC must bound and validate incoming paths")
    return errors


def find_persistence_view_boundary_regressions() -> list[str]:
    """Keep persistence independent from the concrete RichEdit implementation."""
    errors: list[str] = []
    stage_path = REPO_ROOT / "src/file_output/file_output_stage.cpp"
    output_path = REPO_ROOT / "src/file_output/file_output.cpp"
    bridge_path = REPO_ROOT / "src/bridge/view_bridge.h"
    note_view_path = REPO_ROOT / "src/note_view/note_view_shared.cppinc"

    stage_text = stage_path.read_text(encoding="utf-8", errors="ignore")
    output_text = output_path.read_text(encoding="utf-8", errors="ignore")
    bridge_text = bridge_path.read_text(encoding="utf-8", errors="ignore")
    note_view_text = note_view_path.read_text(encoding="utf-8", errors="ignore")

    for relative_path, text in ((stage_path, stage_text), (output_path, output_text)):
        if '#include "note_view/note_view.h"' in text:
            errors.append(
                f"{relative_path.relative_to(REPO_ROOT)}: persistence must use view_bridge, not note_view internals"
            )
    pdf_view_text = (REPO_ROOT / "src/pdf_view/pdf_view.cpp").read_text(encoding="utf-8", errors="ignore")
    if '#include "note_view/note_view.h"' in pdf_view_text:
        errors.append("src/pdf_view/pdf_view.cpp: cross-view operations must use view_bridge, not note_view internals")
    workspace_actions_text = (REPO_ROOT / "src/workspace/workspace_actions.cpp").read_text(encoding="utf-8", errors="ignore")
    if '#include "note_view/note_view.h"' in workspace_actions_text:
        errors.append("src/workspace/workspace_actions.cpp: workspace lifecycle must use view_bridge, not note_view internals")
    workspace_config_text = (REPO_ROOT / "src/workspace/workspace_config_io.cpp").read_text(encoding="utf-8", errors="ignore")
    if '#include "note_view/note_view.h"' in workspace_config_text:
        errors.append("src/workspace/workspace_config_io.cpp: workspace configuration must use view_bridge, not note_view internals")
    settings_core_text = (REPO_ROOT / "src/settings/settings_core_api.cpp").read_text(encoding="utf-8", errors="ignore")
    if '#include "note_view/note_view.h"' in settings_core_text:
        errors.append("src/settings/settings_core_api.cpp: settings must use view_bridge, not note_view internals")
    search_text = (REPO_ROOT / "src/search/search.cpp").read_text(encoding="utf-8", errors="ignore")
    if '#include "note_view/note_view.h"' in search_text:
        errors.append("src/search/search.cpp: search must use view_bridge, not note_view internals")
    layout_text = (REPO_ROOT / "src/ui/layout.cpp").read_text(encoding="utf-8", errors="ignore")
    if '#include "note_view/note_view.h"' in layout_text:
        errors.append("src/ui/layout.cpp: layout must use view_bridge, not note_view internals")
    for relative_path in ("src/ui/menus/main_menu_snapshot.cpp", "src/ui/menus/menu_build.cpp"):
        menu_text = (REPO_ROOT / relative_path).read_text(encoding="utf-8", errors="ignore")
        if '#include "note_view/note_view.h"' in menu_text:
            errors.append(f"{relative_path}: menus must use view_bridge, not note_view internals")
    for forbidden in ("g_hNoteEdit", "EM_GETSEL", "EM_GETSCROLLPOS", "EM_SETMODIFY"):
        if forbidden in stage_text:
            errors.append(
                f"src/file_output/file_output_stage.cpp: editor-specific {forbidden} must stay in note_view"
            )

    bridge_contract = (
        "struct NoteEditorPersistenceState",
        "HasCurrentNoteEditorForPath",
        "CaptureCurrentNoteEditorPersistenceState",
        "ClearNoteEditorSilently",
        "RefreshCurrentNoteBottomPane",
        "ExitCurrentNoteNormalMode",
        "RefreshCurrentNoteAfterFontChange",
        "ApplyCurrentNoteViewConfiguration",
        "CurrentNoteEditorIsModified",
        "CanExecuteNoteUndoRedoFromFocus",
        "ClearCurrentNoteSearchMarker",
        "SetCurrentNoteSearchMarker",
        "SelectCurrentNoteLine",
        "MarkCurrentNoteEditorPersisted",
        "SynchronizeActiveNoteEditorToKernel",
        "CommitActiveNoteEditBoundary",
    )
    if any(needle not in bridge_text for needle in bridge_contract):
        errors.append("src/bridge/view_bridge.h: note persistence boundary contract is incomplete")

    implementation_contract = (
        "bool HasCurrentNoteEditorForPath",
        "CaptureCurrentNoteEditorPersistenceState",
        "void MarkCurrentNoteEditorPersisted",
    )
    if any(needle not in note_view_text for needle in implementation_contract):
        errors.append("src/note_view/note_view_shared.cppinc: note persistence boundary implementation is incomplete")
    return errors

def main() -> int:
    problems: list[str] = []

    problems.extend(compile_python_files())

    powershell_encoding_problems = find_windows_powershell_encoding_violations()
    if powershell_encoding_problems:
        problems.append("Windows PowerShell encoding violation(s) detected:")
        problems.extend(powershell_encoding_problems)

    fs_problems = find_new_throwing_filesystem_calls()
    if fs_problems:
        problems.append("new potentially-throwing std::filesystem call(s) detected:")
        problems.extend(fs_problems)

    raw_timer_ids = find_raw_win32_timer_ids()
    if raw_timer_ids:
        problems.append("raw Win32 timer ID literal(s) detected; use a named constant:")
        problems.extend(raw_timer_ids)

    long_path_problems = find_long_path_contract_violations()
    if long_path_problems:
        problems.append("long-path contract violation(s) detected:")
        problems.extend(long_path_problems)

    ui_string_problems = find_forbidden_ui_strings()
    if ui_string_problems:
        problems.append("forbidden UI string(s) detected:")
        problems.extend(ui_string_problems)
    command_id_collisions = find_command_id_collisions()
    if command_id_collisions:
        problems.append("CommandId collision(s) detected:")
        problems.extend(command_id_collisions)

    font_combo_problems = find_font_combo_display_regressions()
    if font_combo_problems:
        problems.append("font combo display regression(s) detected:")
        problems.extend(font_combo_problems)

    textbox_position_problems = find_textbox_position_regressions()
    if textbox_position_problems:
        problems.append("text box position regression(s) detected:")
        problems.extend(textbox_position_problems)
    rotated_ellipse_problems = find_rotated_ellipse_rotation_regressions()
    if rotated_ellipse_problems:
        problems.append("rotated ellipse rotation regression(s) detected:")
        problems.extend(rotated_ellipse_problems)
    textbox_vertical_writing_mode_problems = find_textbox_vertical_writing_mode_regressions()
    if textbox_vertical_writing_mode_problems:
        problems.append("text box vertical writing mode regression(s) detected:")
        problems.extend(textbox_vertical_writing_mode_problems)
    annotation_summary_problems = find_annotation_summary_regressions()
    if annotation_summary_problems:
        problems.append("annotation panel summary regression(s) detected:")
        problems.extend(annotation_summary_problems)
    debug_resource_monitor_problems = find_debug_resource_monitor_regressions()
    if debug_resource_monitor_problems:
        problems.append("debug resource monitor regression(s) detected:")
        problems.extend(debug_resource_monitor_problems)
    shortcut_problems = find_annotation_shortcut_regressions()
    if shortcut_problems:
        problems.append("annotation shortcut regression(s) detected:")
        problems.extend(shortcut_problems)
    palette_problems = find_palette_regressions()
    if palette_problems:
        problems.append("palette regression(s) detected:")
        problems.extend(palette_problems)
    search_problems = find_search_regressions()
    if search_problems:
        problems.append("search regression(s) detected:")
        problems.extend(search_problems)
    textbox_font_sync_problems = find_textbox_font_sync_regressions()
    if textbox_font_sync_problems:
        problems.append("text box font sync regression(s) detected:")
        problems.extend(textbox_font_sync_problems)
    textbox_commit_layout_problems = find_textbox_commit_layout_regressions()
    if textbox_commit_layout_problems:
        problems.append("text box commit layout regression(s) detected:")
        problems.extend(textbox_commit_layout_problems)
    annotation_text_quality_problems = find_annotation_text_quality_regressions()
    if annotation_text_quality_problems:
        problems.append("annotation text quality regression(s) detected:")
        problems.extend(annotation_text_quality_problems)
    annotation_layer_problems = find_annotation_layer_regressions()
    if annotation_layer_problems:
        problems.append("annotation layer regression(s) detected:")
        problems.extend(annotation_layer_problems)
    png_export_quality_problems = find_png_export_quality_regressions()
    if png_export_quality_problems:
        problems.append("PNG export quality regression(s) detected:")
        problems.extend(png_export_quality_problems)
    pdf_textbox_visibility_problems = find_pdf_textbox_visibility_regressions()
    if pdf_textbox_visibility_problems:
        problems.append("pdf text box visibility regression(s) detected:")
        problems.extend(pdf_textbox_visibility_problems)
    annotation_save_timing_problems = find_annotation_save_timing_regressions()
    if annotation_save_timing_problems:
        problems.append("annotation save timing regression(s) detected:")
        problems.extend(annotation_save_timing_problems)
    existing_pdf_annotation_policy_problems = find_existing_pdf_annotation_policy_regressions()
    if existing_pdf_annotation_policy_problems:
        problems.append("existing PDF annotation policy regression(s) detected:")
        problems.extend(existing_pdf_annotation_policy_problems)
    slide_crop_export_problems = find_slide_crop_export_regressions()
    if slide_crop_export_problems:
        problems.append("slide crop export regression(s) detected:")
        problems.extend(slide_crop_export_problems)
    note_placement_problems = find_note_placement_regressions()
    if note_placement_problems:
        problems.append("note placement regression(s) detected:")
        problems.extend(note_placement_problems)
    link_workflow_problems = find_link_workflow_regressions()
    if link_workflow_problems:
        problems.append("link workflow regression(s) detected:")
        problems.extend(link_workflow_problems)
    assist_pane_visibility_problems = find_assist_pane_visibility_regressions()
    if assist_pane_visibility_problems:
        problems.append("assist pane visibility regression(s) detected:")
        problems.extend(assist_pane_visibility_problems)
    startup_last_open_problems = find_startup_last_open_regressions()
    if startup_last_open_problems:
        problems.append("startup last-open regression(s) detected:")
        problems.extend(startup_last_open_problems)
    note_input_latency_problems = find_note_input_latency_regressions()
    if note_input_latency_problems:
        problems.append("note input latency regression(s) detected:")
        problems.extend(note_input_latency_problems)
    note_save_history_problems = find_note_save_history_regressions()
    if note_save_history_problems:
        problems.append("note save-history regression(s) detected:")
        problems.extend(note_save_history_problems)
    note_tex_route_problems = find_note_tex_route_regressions()
    if note_tex_route_problems:
        problems.append("note TeX route regression(s) detected:")
        problems.extend(note_tex_route_problems)
    final_render_publication_boundary_problems = find_final_render_publication_boundary_regressions()
    if final_render_publication_boundary_problems:
        problems.append("final render publication boundary regression(s) detected:")
        problems.extend(final_render_publication_boundary_problems)
    note_render_incremental_problems = find_note_render_incremental_regressions()
    if note_render_incremental_problems:
        problems.append("note render incremental regression(s) detected:")
        problems.extend(note_render_incremental_problems)
    note_ime_layout_coordinate_problems = find_note_ime_layout_coordinate_regressions()
    if note_ime_layout_coordinate_problems:
        problems.append("note IME/layout coordinate regression(s) detected:")
        problems.extend(note_ime_layout_coordinate_problems)
    layout_flicker_problems = find_layout_flicker_regressions()
    if layout_flicker_problems:
        problems.append("layout/flicker regression(s) detected:")
        problems.extend(layout_flicker_problems)
    note_indent_delete_problems = find_note_indent_delete_regressions()
    if note_indent_delete_problems:
        problems.append("note indent delete regression(s) detected:")
        problems.extend(note_indent_delete_problems)
    note_auto_pair_problems = find_note_auto_pair_bracket_regressions()
    if note_auto_pair_problems:
        problems.append("note auto-pair bracket regression(s) detected:")
        problems.extend(note_auto_pair_problems)
    workspace_config_compatibility_problems = find_workspace_config_compatibility_regressions()
    if workspace_config_compatibility_problems:
        problems.append("workspace config compatibility regression(s) detected:")
        problems.extend(workspace_config_compatibility_problems)
    pdf_textbox_tap_commit_problems = find_pdf_textbox_tap_commit_regressions()
    if pdf_textbox_tap_commit_problems:
        problems.append("pdf textbox tap commit regression(s) detected:")
        problems.extend(pdf_textbox_tap_commit_problems)
    operation_feedback_problems = find_operation_feedback_regressions()
    if operation_feedback_problems:
        problems.append("operation feedback regression(s) detected:")
        problems.extend(operation_feedback_problems)
    runtime_safety_problems = find_runtime_safety_regressions()
    if runtime_safety_problems:
        problems.append("runtime safety regression(s) detected:")
        problems.extend(runtime_safety_problems)
    multi_instance_problems = find_multi_instance_launch_regressions()
    if multi_instance_problems:
        problems.append("multi-instance launch regression(s) detected:")
        problems.extend(multi_instance_problems)
    persistence_view_boundary_problems = find_persistence_view_boundary_regressions()
    if persistence_view_boundary_problems:
        problems.append("persistence/view boundary regression(s) detected:")
        problems.extend(persistence_view_boundary_problems)

    if problems:
        for item in problems:
            print(item, file=sys.stderr)
        return 1

    print("codebase validation passed")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError as exc:
        print(f"codebase validation could not read a required path: {exc}", file=sys.stderr)
        raise SystemExit(1)
