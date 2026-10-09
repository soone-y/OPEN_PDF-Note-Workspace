#!/usr/bin/env python3
"""Inventory/classify local upstream QA inputs; write metadata only, never fetch files."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import zipfile
import zlib
from collections import Counter
from pathlib import Path, PurePosixPath

ROOT = Path(__file__).resolve().parents[2]
CATALOG = ROOT / "tests/config/libreoffice_conversion_corpus.json"
ENCODINGS = ROOT / "tests/config/libreoffice_fixture_encodings.json"
EXTENSIONS = (".docx", ".pptx", ".xlsx")
GROUPS = {
    "all": "全QA入力（分類の重複を除く）",
    "writer-export": "Writer OOXML出力用入力",
    "writer-import": "Writer OOXML読込み用入力",
    "layout": "レイアウト・印刷設定",
    "charts": "グラフ・chart2",
    "embedded": "埋込みオブジェクト・埋込みpackage",
    "calc-related": "Calc・XLSX・グラフ・埋込み（広めの関連群）",
    "formulas": "セル数式・shared formula",
    "pivot": "ピボットテーブル",
    "conditional-format": "条件付き書式",
    "math": "OMML等の数式",
    "tables": "表",
    "images": "画像",
    "media": "音声・動画を含む入力",
    "smartart": "diagram/SmartArt",
    "external-links": "外部関係の記述を含む入力（取得はしない）",
    "exception-candidates": "fail/暗号化/破損等の調査候補（除外しない）",
    "inspection-incomplete": "container分類の一部が不能・上限到達（除外しない）",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def decode_rc4_cve(data: bytes) -> bytes:
    """Mirror the public QA obfuscation in unotest's filters-test.cxx."""
    key = b"CVE"
    state = list(range(256))
    j = 0
    for i in range(256):
        j = (j + state[i] + key[i % len(key)]) % 256
        state[i], state[j] = state[j], state[i]
    i = j = 0
    output = bytearray()
    for value in data:
        i = (i + 1) % 256
        j = (j + state[i]) % 256
        state[i], state[j] = state[j], state[i]
        output.append(value ^ state[(state[i] + state[j]) % 256])
    return bytes(output)


def fixture_encodings(source: Path, items: list[dict]) -> dict[str, dict]:
    metadata = json.loads(ENCODINGS.read_text(encoding="utf-8"))
    if metadata["version"] != 1:
        raise ValueError("Unsupported fixture encoding metadata")
    encodings = {}
    known = {item["path"]: item for item in items}
    seen = set()
    for entry in metadata["samples"]:
        name = entry["path"]
        if name in seen or entry["codec"] != "rc4-cve":
            raise ValueError("Invalid fixture encoding metadata")
        seen.add(name)
        for field in ("raw_sha256", "decoded_sha256"):
            if not re.fullmatch(r"[0-9a-f]{64}", entry[field]):
                raise ValueError("Invalid fixture encoding hash")
        if name in known:
            if known[name]["sha256"] != entry["raw_sha256"]:
                raise ValueError("Fixture raw hash differs from selected catalog")
            data = (source / name).read_bytes()
            if hashlib.sha256(data).hexdigest() != entry["raw_sha256"]:
                raise ValueError("Fixture raw hash mismatch")
            if hashlib.sha256(decode_rc4_cve(data)).hexdigest() != entry["decoded_sha256"]:
                raise ValueError("Fixture decoded hash mismatch")
            encodings[name] = entry
    if encodings:
        test_path = (source / metadata["source_test"]).resolve(strict=True)
        if not test_path.is_relative_to(source.resolve()) or sha256(test_path) != metadata["source_test_sha256"]:
            raise ValueError("Official fixture preparation source changed")
    return encodings


def prepare_input(original: Path, staged: Path, item: dict, encoding: dict | None) -> str:
    if sha256(original) != item["sha256"]:
        raise ValueError("Original input hash mismatch")
    if encoding is None:
        shutil.copy2(original, staged)
        expected = item["sha256"]
    else:
        if encoding["codec"] != "rc4-cve" or encoding["raw_sha256"] != item["sha256"]:
            raise ValueError("Invalid input preparation binding")
        data = decode_rc4_cve(original.read_bytes())
        expected = encoding["decoded_sha256"]
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("Decoded input hash mismatch")
        staged.write_bytes(data)
        shutil.copystat(original, staged)
    if sha256(staged) != expected:
        raise ValueError("Staged input hash mismatch")
    return expected


def discover(source: Path, extensions=EXTENSIONS) -> list[str]:
    source = source.resolve(strict=True)
    found = []
    def fail(error):
        raise error
    for qa in sorted(source.glob("*/qa")):
        if not qa.is_dir():
            continue
        for current, dirs, files in os.walk(qa, followlinks=False, onerror=fail):
            for name in dirs + files:
                path = Path(current) / name
                if path.is_symlink() or not path.resolve().is_relative_to(source):
                    raise ValueError(f"QA path link/escape requires review: {path}")
            for name in files:
                path = Path(current) / name
                if path.suffix.lower() in extensions:
                    found.append(path.relative_to(source).as_posix())
    return sorted(found)


def classify(source: Path, relative: str) -> list[str]:
    lower = relative.lower()
    module = relative.split("/")[0]
    groups = {"all", "module-" + module, "format-" + Path(relative).suffix[1:].lower()}
    if "/ooxmlexport/" in lower:
        groups.add("writer-export")
    if "/ooxmlimport/" in lower:
        groups.add("writer-import")
    if any(word in lower for word in ("layout", "print", "page_scale")):
        groups.add("layout")
    if module == "chart2":
        groups.add("charts")
    if any(word in lower for word in ("/fail/", "password", "encrypted", "corrupt", "broken", "invalid")):
        groups.add("exception-candidates")
    budget = 8 * 1024 * 1024
    try:
        with zipfile.ZipFile(source / relative) as archive:
            for part in archive.infolist():
                name = part.filename.lower()
                if "/embeddings/" in name:
                    groups.add("embedded")
                if "/charts/" in name:
                    groups.add("charts")
                if "pivottable" in name or "pivotcache" in name:
                    groups.add("pivot")
                if "/diagrams/" in name:
                    groups.add("smartart")
                if "/media/" in name:
                    groups.add("images" if Path(name).suffix in (".png", ".jpg", ".jpeg", ".emf", ".wmf", ".svg", ".gif", ".tif", ".tiff", ".bmp") else "media")
                if not name.endswith((".xml", ".rels")):
                    continue
                if part.file_size > budget:
                    groups.add("inspection-incomplete")
                    continue
                content = archive.read(part)
                budget -= len(content)
                if name.startswith("xl/") and re.search(rb"<(?:\w+:)?f[\s>]", content):
                    groups.add("formulas")
                if b"conditionalFormatting" in content:
                    groups.add("conditional-format")
                if b"oMath" in content:
                    groups.add("math")
                if re.search(rb"<(?:\w+:)?tbl[\s>]", content) or "/tables/" in name:
                    groups.add("tables")
                if b' TargetMode="External"' in content:
                    groups.add("external-links")
    except (zipfile.BadZipFile, RuntimeError, NotImplementedError, zlib.error, EOFError, UnicodeDecodeError):
        groups.update(("exception-candidates", "inspection-incomplete"))
    if module in ("sc", "chart2") or Path(relative).suffix == ".xlsx" or groups & {"embedded", "charts"}:
        groups.add("calc-related")
    return sorted(groups)


def build_catalog(source: Path, version: str) -> dict:
    paths = discover(source)
    if not paths:
        raise ValueError("No QA Office inputs found")
    entries = [{"path": path, "sha256": sha256(source / path), "groups": classify(source, path)} for path in paths]
    counts = Counter(group for item in entries for group in item["groups"])
    return {"catalog_version": 1, "source_version": version, "extensions": list(EXTENSIONS),
            "scope": "all */qa recursively", "sample_count": len(entries),
            "classification_note": "Path/container hints only; not execution coverage or expected outcome. No automatic exclusions.",
            "groups": {group: {"description": GROUPS.get(group, group), "count": count}
                       for group, count in sorted(counts.items())}, "samples": entries}


def read_catalog(path: Path) -> dict:
    catalog = json.loads(path.read_text(encoding="utf-8"))
    items = catalog["samples"]
    paths = [item["path"] for item in items]
    if not items or len(set(paths)) != len(paths):
        raise ValueError("Empty or duplicate corpus inputs")
    for item in items:
        relative = PurePosixPath(item["path"])
        if relative.is_absolute() or ".." in relative.parts or ":" in item["path"] or "\\" in item["path"]:
            raise ValueError("Unsafe corpus relative path")
        if not re.fullmatch(r"[0-9a-f]{64}", item["sha256"]):
            raise ValueError("Invalid corpus hash")
    if "catalog_version" in catalog:
        if catalog["catalog_version"] != 1 or catalog["sample_count"] != len(items) or catalog["extensions"] != list(EXTENSIONS):
            raise ValueError("Invalid full-corpus identity/count/scope")
        counts = Counter(group for item in items for group in item["groups"])
        if set(counts) != set(catalog["groups"]) or any(catalog["groups"][group]["count"] != count for group, count in counts.items()):
            raise ValueError("Group counts do not cover the catalog")
        if counts["all"] != len(items):
            raise ValueError("Full corpus must contain every input exactly once")
    elif len(items) != 11:
        raise ValueError("Legacy corpus must contain all 11 inputs")
    return catalog


def select(catalog: dict, groups: list[str], samples: list[str], *, acceptance: bool) -> list[dict]:
    if acceptance:
        if groups or samples or "catalog_version" not in catalog:
            raise ValueError("Acceptance requires the full catalog, without subset selectors")
        return catalog["samples"]
    known = {item["path"] for item in catalog["samples"]}
    unknown = set(samples) - known
    if unknown:
        raise ValueError(f"Unknown individual inputs: {sorted(unknown)}")
    if set(groups) - set(catalog.get("groups", {})):
        raise ValueError(f"Unknown groups: {groups}")
    if not groups and not samples:
        return catalog["samples"]
    selected = [item for item in catalog["samples"] if item["path"] in samples or set(item.get("groups", [])) & set(groups)]
    if not selected:
        raise ValueError("Selected corpus is empty")
    return selected


def verify_inventory(source: Path, catalog: dict) -> None:
    expected = {item["path"] for item in catalog["samples"]}
    actual = set(discover(source, catalog["extensions"]))
    if expected != actual:
        raise ValueError(f"QA corpus changed: missing={sorted(expected-actual)[:10]}, added={sorted(actual-expected)[:10]}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--source-version", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    source = Path(args.source_dir).resolve(strict=True)
    catalog = build_catalog(source, args.source_version)
    output = Path(args.output).resolve()
    if output.is_relative_to(source):
        raise ValueError("Metadata output must be outside the source root")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(catalog, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    print(f"documents={catalog['sample_count']} groups={len(catalog['groups'])} catalog={output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
