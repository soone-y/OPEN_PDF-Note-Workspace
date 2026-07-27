#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import shutil
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path


PHASE1_PATHS = (
    "program/updater.exe",
    "program/update_service.exe",
    "program/updatecheckuilo.dll",
    "program/updater.ini",
    "program/senddoc.exe",
    "update-settings.ini",
    "share/registry/onlineupdate.xcd",
    "program/minidump_upload.exe",
    "program/updchklo.dll",
    "program/mar.exe",
    "program/quickstart.exe",
    "program/sweb.exe",
    "program/mailmerge.py",
    "program/classes/java_websocket.jar",
)

CONVERSION_ONLY_PATHS = (
    # PDF import is outside this app's LibreOffice responsibility. PDF handling
    # is done by PDFium; LibreOffice is kept only for Office-to-PDF conversion.
    "program/xpdfimport.exe",
    "share/xpdfimport",
    # Online/wiki publishing and solver extensions are not part of document
    # conversion and add network-facing or UI-only surface area.
    "share/extensions/wiki-publisher",
    "share/extensions/nlpsolver",
    # Standalone GUI/module launchers and maintenance tools. Conversion uses
    # soffice.com with explicit headless arguments.
    "program/sbase.exe",
    "program/scalc.exe",
    "program/sdraw.exe",
    "program/simpress.exe",
    "program/smath.exe",
    "program/soffice_safe.exe",
    "program/swriter.exe",
    "program/gengal.exe",
    "program/twain32shim.exe",
    "program/unopkg.com",
    "program/unopkg.exe",
    "program/uno.exe",
    "program/unoinfo.exe",
    "program/regview.exe",
    "program/odbcconfig.exe",
    "program/opencltest.exe",
    "program/spsupp_helper.exe",
    "program/gpgme-w32spawn.exe",
)

HEADLESS_ONLY_PATHS = (
    # GUI-only resources and built-in scripts/macros. The app never exposes
    # LibreOffice UI and uses it only for headless conversion of staged files.
    "share/gallery",
    "share/wizards",
    "program/wizards",
    "share/Scripts",
    "share/basic",
    "share/tipoftheday",
)

TEMPLATE_PATHS = (
    # Templates are used for creating new LibreOffice documents. This app only
    # converts existing Word/PowerPoint files to PDF through headless soffice.
    "share/template",
)

AUTHORING_DATA_PATHS = (
    # Authoring helpers for creating/editing LibreOffice documents. Headless
    # conversion should not use these to render existing Word/PowerPoint files.
    "share/autotext",
    "share/autocorr",
    "share/labels",
    "share/classification",
    "share/wordbook",
    "share/config/wizard",
)

STALE_REGISTRY_PATHS = (
    # Registry fragments for removed features or external directory samples.
    "share/registry/pdfimport.xcd",
    "share/registry/librelogo.xcd",
    "share/registry/oo-ldap.xcd.sample",
    "share/registry/oo-ad-ldap.xcd.sample",
)

KEEP_UI_LOCALES = frozenset(("ja", "en-US", "en-GB", "en-ZA"))
KEEP_DICTIONARY_EXTENSIONS = frozenset(("dict-en",))
KEEP_PROGRAM_RESOURCE_LOCALES = frozenset(("common", "ja", "en_GB", "en_ZA", "en_US"))
REPO_LOCAL_RESOURCE_DIR = ".local/repo_resource"

SCRIPTING_RUNTIME_PATHS = (
    # Python/UNO scripting runtime. This app never executes LibreOffice macros
    # or scripts; it only drives soffice.com for headless document conversion.
    "program/python312.dll",
    "program/python3.dll",
    "program/python.exe",
    "program/pyuno.pyd",
    "program/pythonloaderlo.dll",
    "program/pythonloader.py",
    "program/pythonloader.uno.ini",
    "program/pythonscript.py",
    "program/uno.py",
    "program/unohelper.py",
    "program/officehelper.py",
    "program/msgbox.py",
    "program/scriptforge.py",
    "program/scriptforge.pyi",
    "program/access2base.py",
    "program/desktophelper.txt",
)

DATABASE_JAVA_PATHS = (
    # Base/database/reportbuilder and Java UNO runtime are outside this app's
    # Word/PowerPoint-to-PDF responsibility.
    "program/classes",
    "program/abplo.dll",
    "program/dbplo.dll",
    "program/hsqldb.dll",
    "program/jdbclo.dll",
    "program/postgresql-sdbc-impllo.dll",
    "program/libpq.dll",
    "program/java_uno.dll",
    "program/javaloaderlo.dll",
    "program/javavendors.xml",
    "program/javavmlo.dll",
    "program/jvmfwk3.ini",
    "share/firebird",
    "share/config/soffice.cfg/dbaccess",
    "share/config/soffice.cfg/modules/dbapp",
    "share/config/soffice.cfg/modules/dbbrowser",
    "share/config/soffice.cfg/modules/dbquery",
    "share/config/soffice.cfg/modules/dbrelation",
    "share/config/soffice.cfg/modules/dbreport",
    "share/config/soffice.cfg/modules/dbtable",
    "share/config/soffice.cfg/modules/dbtdata",
    "share/config/soffice.cfg/modules/sabpilot",
    "share/config/soffice.cfg/modules/sbibliography",
    "share/config/soffice.cfg/modules/swreport",
    "share/registry/base.xcd",
    "share/registry/postgresql.xcd",
    "share/registry/reportbuilder.xcd",
)

CALC_PATHS = (
    # Retained as an analysis inventory only. Removing this group degrades
    # embedded Excel objects in DOCX conversion. The CLI rejects --calc.
    "program/calclo.dll",
    "program/scdlo.dll",
    "program/scfiltlo.dll",
    "program/sclo.dll",
    "program/scnlo.dll",
    "program/scuilo.dll",
    "program/vbaobjlo.dll",
    "share/calc",
    "share/config/soffice.cfg/modules/scalc",
    "share/registry/calc.xcd",
    "share/xslt/export/spreadsheetml",
    "share/xslt/import/spreadsheetml",
    "share/xslt/export/uof/odf2uof_spreadsheet.xsl",
    "share/xslt/import/uof/uof2odf_spreadsheet.xsl",
)

NONCONVERSION_LEFTOVER_PATHS = (
    # Leftovers for already removed or out-of-scope functionality.
    "program/pdfimportlo.dll",
    "program/PresentationMinimizerlo.dll",
    "program/Engine12.dll",
    "program/intl/fbintl.dll",
    "program/intl/fbintl.conf",
    "share/config/soffice.cfg/writerperfect",
)

# Keep libcurl.dll unless its importers are removed first. Current LibreOffice
# 26.2.3.2 imports it from mergedlo.dll and LanguageToollo.dll; deleting it
# makes soffice fail before conversion starts.


@dataclass
class RemovalItem:
    path: Path
    rel: str
    kind: str
    size_bytes: int


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Back up and remove selected unused LibreOffice image files."
    )
    parser.add_argument(
        "--repo-root",
        default=".",
        help="Repository root. Defaults to current directory.",
    )
    parser.add_argument(
        "--image-root",
        default="third_party/libreoffice/image",
        help="LibreOffice image path relative to repo root.",
    )
    parser.add_argument(
        "--backup-root",
        default=None,
        help=(
            "Backup destination. Defaults to "
            ".local/repo_resource/tmp/libreoffice_reduction_backup/<timestamp>."
        ),
    )
    parser.add_argument(
        "--phase1",
        action="store_true",
        help="Include obvious non-conversion communication/web/upload launcher candidates.",
    )
    parser.add_argument(
        "--cache",
        action="store_true",
        help="Include __pycache__ directories and *.pyc/*.pyo files.",
    )
    parser.add_argument(
        "--conversion-only",
        action="store_true",
        help="Include non-conversion launchers/resources outside Office-to-PDF conversion responsibility.",
    )
    parser.add_argument(
        "--headless-only",
        action="store_true",
        help="Include GUI-only resources and bundled scripts/macros not needed for headless conversion.",
    )
    parser.add_argument(
        "--templates",
        action="store_true",
        help="Include LibreOffice document templates not needed for converting existing files.",
    )
    parser.add_argument(
        "--authoring-data",
        action="store_true",
        help="Include new-document/editing helper data not needed for headless conversion.",
    )
    parser.add_argument(
        "--ui-locales-ja-en",
        action="store_true",
        help="Remove UI registry locale resources except Japanese and English.",
    )
    parser.add_argument(
        "--stale-registry",
        action="store_true",
        help="Include registry fragments for features already removed from the image.",
    )
    parser.add_argument(
        "--dictionaries-ja-en",
        action="store_true",
        help="Remove bundled dictionaries except English; no Japanese dictionary extension is present.",
    )
    parser.add_argument(
        "--program-resources-ja-en",
        action="store_true",
        help="Remove program UI resource locale directories except Japanese and English.",
    )
    parser.add_argument(
        "--scripting-runtime",
        action="store_true",
        help="Include Python/UNO scripting runtime not needed for headless conversion.",
    )
    parser.add_argument(
        "--ui-icon-themes",
        action="store_true",
        help="Include LibreOffice UI icon theme archives not needed for headless conversion.",
    )
    parser.add_argument(
        "--database-java",
        action="store_true",
        help="Include Base/database/reportbuilder and Java UNO runtime not needed for Word/PowerPoint conversion.",
    )
    parser.add_argument(
        "--calc",
        action="store_true",
        help="Disabled: Calc resources are required by embedded Excel objects in DOCX conversion.",
    )
    parser.add_argument(
        "--nonconversion-leftovers",
        action="store_true",
        help="Include remaining non-conversion leftovers whose primary entry points were already removed.",
    )
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Actually copy to backup and remove. Without this, only prints the plan.",
    )
    parser.add_argument(
        "--format",
        choices=("text", "json"),
        default="text",
        help="Output format.",
    )
    args = parser.parse_args()
    if args.calc:
        parser.error(
            "--calc is disabled: Calc resources are required to preserve embedded "
            "Excel objects during DOCX-to-PDF conversion"
        )
    return args


def ensure_inside(path: Path, root: Path) -> Path:
    resolved = path.resolve()
    root_resolved = root.resolve()
    if resolved != root_resolved and root_resolved not in resolved.parents:
        raise ValueError(f"path escapes image root: {path}")
    return resolved


def path_size(path: Path) -> int:
    if path.is_file():
        return path.stat().st_size
    total = 0
    for child in path.rglob("*"):
        if child.is_file():
            total += child.stat().st_size
    return total


def ui_locale_from_registry_res_name(name: str) -> str | None:
    if name.startswith("fcfg_langpack_") and name.endswith(".xcd"):
        return name.removeprefix("fcfg_langpack_").removesuffix(".xcd")
    if name.startswith("registry_") and name.endswith(".xcd"):
        return name.removeprefix("registry_").removesuffix(".xcd")
    return None


def is_removable_dictionary_extension(path: Path) -> bool:
    return path.is_dir() and path.name.startswith("dict-") and path.name not in KEEP_DICTIONARY_EXTENSIONS


def is_removable_program_resource_locale(path: Path) -> bool:
    return path.is_dir() and path.name not in KEEP_PROGRAM_RESOURCE_LOCALES


def collect_items(
    image_root: Path,
    include_phase1: bool,
    include_cache: bool,
    include_conversion_only: bool,
    include_headless_only: bool,
    include_templates: bool,
    include_authoring_data: bool,
    include_ui_locales_ja_en: bool,
    include_stale_registry: bool,
    include_dictionaries_ja_en: bool,
    include_program_resources_ja_en: bool,
    include_scripting_runtime: bool,
    include_ui_icon_themes: bool,
    include_database_java: bool,
    include_calc: bool,
    include_nonconversion_leftovers: bool,
) -> list[RemovalItem]:
    items: dict[Path, RemovalItem] = {}

    def add(path: Path) -> None:
        if not path.exists():
            return
        resolved = ensure_inside(path, image_root)
        if any(parent == resolved or parent in resolved.parents for parent in items):
            return
        for child in [item_path for item_path in items if resolved in item_path.parents]:
            del items[child]
        rel = resolved.relative_to(image_root.resolve()).as_posix()
        kind = "dir" if resolved.is_dir() else "file"
        items[resolved] = RemovalItem(path=resolved, rel=rel, kind=kind, size_bytes=path_size(resolved))

    if include_phase1:
        for rel in PHASE1_PATHS:
            add(image_root / rel)
        for path in image_root.glob("LibreOffice_*_Win_x86-64.msi"):
            if path.is_file():
                add(path)

    if include_conversion_only:
        for rel in CONVERSION_ONLY_PATHS:
            add(image_root / rel)

    if include_headless_only:
        for rel in HEADLESS_ONLY_PATHS:
            add(image_root / rel)

    if include_templates:
        for rel in TEMPLATE_PATHS:
            add(image_root / rel)

    if include_authoring_data:
        for rel in AUTHORING_DATA_PATHS:
            add(image_root / rel)

    if include_stale_registry:
        for rel in STALE_REGISTRY_PATHS:
            add(image_root / rel)

    if include_ui_locales_ja_en:
        registry_res = image_root / "share" / "registry" / "res"
        if registry_res.exists():
            for path in registry_res.iterdir():
                if not path.is_file():
                    continue
                locale = ui_locale_from_registry_res_name(path.name)
                if locale is not None and locale not in KEEP_UI_LOCALES:
                    add(path)

    if include_dictionaries_ja_en:
        extensions = image_root / "share" / "extensions"
        if extensions.exists():
            for path in extensions.iterdir():
                if is_removable_dictionary_extension(path):
                    add(path)

    if include_program_resources_ja_en:
        resources = image_root / "program" / "resource"
        if resources.exists():
            for path in resources.iterdir():
                if is_removable_program_resource_locale(path):
                    add(path)

    if include_scripting_runtime:
        for rel in SCRIPTING_RUNTIME_PATHS:
            add(image_root / rel)
        program = image_root / "program"
        if program.exists():
            for path in program.glob("python-core-*"):
                if path.is_dir():
                    add(path)

    if include_ui_icon_themes:
        config = image_root / "share" / "config"
        if config.exists():
            for path in config.glob("images_*.zip"):
                if path.is_file():
                    add(path)

    if include_database_java:
        for rel in DATABASE_JAVA_PATHS:
            add(image_root / rel)

    if include_calc:
        for rel in CALC_PATHS:
            add(image_root / rel)

    if include_nonconversion_leftovers:
        for rel in NONCONVERSION_LEFTOVER_PATHS:
            add(image_root / rel)

    if include_cache:
        cache_dirs: list[Path] = []
        for path in image_root.rglob("__pycache__"):
            if path.is_dir():
                cache_dirs.append(path.resolve())
                add(path)
        for suffix in ("*.pyc", "*.pyo"):
            for path in image_root.rglob(suffix):
                if path.is_file():
                    resolved = path.resolve()
                    if any(cache_dir == resolved.parent or cache_dir in resolved.parents for cache_dir in cache_dirs):
                        continue
                    add(path)

    return sorted(items.values(), key=lambda item: item.rel)


def copy_to_backup(item: RemovalItem, image_root: Path, backup_root: Path) -> None:
    destination = backup_root / item.rel
    destination.parent.mkdir(parents=True, exist_ok=True)
    if item.path.is_dir():
        if destination.exists():
            shutil.rmtree(destination)
        shutil.copytree(item.path, destination)
    else:
        shutil.copy2(item.path, destination)


def remove_item(item: RemovalItem) -> None:
    if item.path.is_dir():
        shutil.rmtree(item.path)
    else:
        item.path.unlink()


def item_to_dict(item: RemovalItem) -> dict[str, object]:
    return {
        "path": item.rel,
        "kind": item.kind,
        "size_bytes": item.size_bytes,
    }


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    image_root = ensure_inside(repo_root / args.image_root, repo_root)
    if not image_root.exists():
        raise SystemExit(f"LibreOffice image root not found: {image_root}")

    if (
        not args.phase1
        and not args.cache
        and not args.conversion_only
        and not args.headless_only
        and not args.templates
        and not args.authoring_data
        and not args.ui_locales_ja_en
        and not args.stale_registry
        and not args.dictionaries_ja_en
        and not args.program_resources_ja_en
        and not args.scripting_runtime
        and not args.ui_icon_themes
        and not args.database_java
        and not args.calc
        and not args.nonconversion_leftovers
    ):
        raise SystemExit(
            "Select at least one target group: --phase1, --cache, --conversion-only, "
            "--headless-only, --templates, --authoring-data, --ui-locales-ja-en, "
            "--stale-registry, --dictionaries-ja-en, --program-resources-ja-en, "
            "--scripting-runtime, --ui-icon-themes, --database-java, --calc, "
            "or --nonconversion-leftovers."
        )

    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_root = Path(args.backup_root).resolve() if args.backup_root else (
        repo_root / REPO_LOCAL_RESOURCE_DIR / "tmp" / "libreoffice_reduction_backup" / timestamp
    )
    items = collect_items(
        image_root,
        args.phase1,
        args.cache,
        args.conversion_only,
        args.headless_only,
        args.templates,
        args.authoring_data,
        args.ui_locales_ja_en,
        args.stale_registry,
        args.dictionaries_ja_en,
        args.program_resources_ja_en,
        args.scripting_runtime,
        args.ui_icon_themes,
        args.database_java,
        args.calc,
        args.nonconversion_leftovers,
    )
    total = sum(item.size_bytes for item in items)

    report = {
        "apply": args.apply,
        "image_root": str(image_root),
        "backup_root": str(backup_root),
        "count": len(items),
        "total_bytes": total,
        "items": [item_to_dict(item) for item in items],
    }

    if args.apply:
        backup_root.mkdir(parents=True, exist_ok=True)
        for item in items:
            copy_to_backup(item, image_root, backup_root)
            remove_item(item)

    if args.format == "json":
        print(json.dumps(report, ensure_ascii=False, indent=2))
    else:
        mode = "APPLY" if args.apply else "DRY-RUN"
        print(f"{mode}: {len(items)} items, {total / 1024 / 1024:.2f} MB")
        print(f"Image: {image_root}")
        print(f"Backup: {backup_root}")
        for item in items[:200]:
            print(f"  {item.kind:4} {item.size_bytes / 1024 / 1024:8.2f} MB  {item.rel}")
        if len(items) > 200:
            print(f"  ... {len(items) - 200} more")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
