#!/usr/bin/env python3
"""Read-only inventory, integrity, dependency, and comparison analysis for LibreOffice runtimes."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
from collections import defaultdict
from pathlib import Path
from typing import Iterable, Sequence


RELEASE_CHECKS_DIR = Path(__file__).resolve().parents[1] / "release_checks"
if str(RELEASE_CHECKS_DIR) not in sys.path:
    sys.path.insert(0, str(RELEASE_CHECKS_DIR))

import binary_scan  # noqa: E402


REPORT_VERSION = 1
DEFAULT_REQUIRED_PATHS = (
    "program/soffice.com",
    "program/soffice.bin",
    "program/fundamental.ini",
    "program/services.rdb",
    "program/types.rdb",
)
DEFAULT_ENTRY_POINTS = (
    "program/soffice.com",
    "program/soffice.bin",
)
SYSTEM_DLLS = frozenset(
    {
        "advapi32.dll",
        "bcrypt.dll",
        "cfgmgr32.dll",
        "combase.dll",
        "comctl32.dll",
        "crypt32.dll",
        "d2d1.dll",
        "d3d11.dll",
        "dwrite.dll",
        "dxgi.dll",
        "gdi32.dll",
        "imm32.dll",
        "iphlpapi.dll",
        "kernel32.dll",
        "mpr.dll",
        "msvcp_win.dll",
        "msvcrt.dll",
        "ntdll.dll",
        "ole32.dll",
        "oleaut32.dll",
        "propsys.dll",
        "rpcrt4.dll",
        "secur32.dll",
        "setupapi.dll",
        "shell32.dll",
        "shlwapi.dll",
        "ucrtbase.dll",
        "user32.dll",
        "userenv.dll",
        "usp10.dll",
        "version.dll",
        "winmm.dll",
        "wtsapi32.dll",
    }
)


def normalize_relative(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_system_import(name: str) -> bool:
    folded = name.casefold()
    return (
        folded in SYSTEM_DLLS
        or folded.startswith("api-ms-win-")
        or folded.startswith("ext-ms-win-")
    )


def scan_tree(root: Path, *, hashes: bool) -> dict[str, object]:
    files: list[dict[str, object]] = []
    symlinks: list[dict[str, object]] = []
    directory_count = 0
    by_top_level: dict[str, dict[str, int]] = defaultdict(lambda: {"files": 0, "bytes": 0})
    by_extension: dict[str, dict[str, int]] = defaultdict(lambda: {"files": 0, "bytes": 0})

    for path in sorted(root.rglob("*"), key=lambda item: normalize_relative(item, root).casefold()):
        rel = normalize_relative(path, root)
        if path.is_symlink():
            target = os.readlink(path)
            resolved = path.resolve(strict=False)
            try:
                resolved.relative_to(root)
                escapes_root = False
            except ValueError:
                escapes_root = True
            symlinks.append(
                {
                    "path": rel,
                    "target": target,
                    "resolved": str(resolved),
                    "escapes_root": escapes_root,
                }
            )
            continue
        if path.is_dir():
            directory_count += 1
            continue
        if not path.is_file():
            continue

        size = path.stat().st_size
        top_level = rel.split("/", 1)[0]
        extension = path.suffix.casefold() or "<none>"
        by_top_level[top_level]["files"] += 1
        by_top_level[top_level]["bytes"] += size
        by_extension[extension]["files"] += 1
        by_extension[extension]["bytes"] += size
        item: dict[str, object] = {"path": rel, "size_bytes": size}
        if hashes:
            item["sha256"] = sha256_file(path)
        files.append(item)

    total_bytes = sum(int(item["size_bytes"]) for item in files)
    return {
        "summary": {
            "files": len(files),
            "directories": directory_count,
            "symlinks": len(symlinks),
            "bytes": total_bytes,
        },
        "by_top_level": [
            {"name": name, **values}
            for name, values in sorted(
                by_top_level.items(), key=lambda item: (-item[1]["bytes"], item[0].casefold())
            )
        ],
        "by_extension": [
            {"extension": extension, **values}
            for extension, values in sorted(
                by_extension.items(), key=lambda item: (-item[1]["bytes"], item[0])
            )
        ],
        "files": files,
        "symlinks": symlinks,
    }


def duplicate_groups(values: Iterable[tuple[str, str]]) -> list[dict[str, object]]:
    grouped: dict[str, list[str]] = defaultdict(list)
    for key, value in values:
        grouped[key.casefold()].append(value)
    return [
        {"key": key, "paths": sorted(paths, key=str.casefold)}
        for key, paths in sorted(grouped.items())
        if len(paths) > 1
    ]


def analyze_dependencies(
    root: Path,
    file_items: Sequence[dict[str, object]],
    *,
    reference_root: Path | None,
    entry_points: Sequence[str],
) -> dict[str, object]:
    binary_paths = [
        root / str(item["path"])
        for item in file_items
        if Path(str(item["path"])).suffix.casefold() in binary_scan.BINARY_SUFFIXES
    ]
    local_by_name: dict[str, list[str]] = defaultdict(list)
    for path in binary_paths:
        local_by_name[path.name.casefold()].append(normalize_relative(path, root))

    reference_names: set[str] = set()
    if reference_root is not None:
        for path in reference_root.rglob("*"):
            if path.is_file() and path.suffix.casefold() in binary_scan.BINARY_SUFFIXES:
                reference_names.add(path.name.casefold())

    binaries: list[dict[str, object]] = []
    local_edges: list[dict[str, str]] = []
    missing_reference_dependencies: list[dict[str, str]] = []
    external_imports: dict[str, set[str]] = defaultdict(set)
    graph: dict[str, set[str]] = defaultdict(set)

    for path in sorted(binary_paths, key=lambda item: normalize_relative(item, root).casefold()):
        rel = normalize_relative(path, root)
        data = path.read_bytes()
        imported_dlls, _symbols = binary_scan.parse_pe_imports(data)
        local_imports: list[str] = []
        system_imports: list[str] = []
        other_imports: list[str] = []
        for imported in imported_dlls:
            folded = imported.casefold()
            local_matches = local_by_name.get(folded, [])
            if local_matches:
                local_imports.append(imported)
                for target in local_matches:
                    graph[rel].add(target)
                    local_edges.append({"from": rel, "to": target, "import": imported})
            elif is_system_import(imported):
                system_imports.append(imported)
            else:
                other_imports.append(imported)
                external_imports[imported].add(rel)
                if folded in reference_names:
                    missing_reference_dependencies.append({"from": rel, "import": imported})
        binaries.append(
            {
                "path": rel,
                "size_bytes": path.stat().st_size,
                "local_imports": sorted(local_imports, key=str.casefold),
                "system_imports": sorted(system_imports, key=str.casefold),
                "external_imports": sorted(other_imports, key=str.casefold),
            }
        )

    reachable: set[str] = set()
    pending = [entry for entry in entry_points if (root / entry).is_file()]
    while pending:
        current = pending.pop()
        if current in reachable:
            continue
        reachable.add(current)
        pending.extend(sorted(graph.get(current, ())))

    local_binary_paths = {normalize_relative(path, root) for path in binary_paths}
    static_unreachable = sorted(local_binary_paths - reachable, key=str.casefold)
    return {
        "summary": {
            "binaries": len(binary_paths),
            "local_dependency_edges": len(local_edges),
            "reachable_from_entry_points": len(reachable),
            "static_unreachable_candidates": len(static_unreachable),
            "missing_reference_dependencies": len(missing_reference_dependencies),
        },
        "entry_points": list(entry_points),
        "binaries": binaries,
        "local_edges": sorted(local_edges, key=lambda item: (item["from"].casefold(), item["to"].casefold())),
        "external_imports": [
            {"name": name, "imported_by": sorted(importers, key=str.casefold)}
            for name, importers in sorted(external_imports.items(), key=lambda item: item[0].casefold())
        ],
        "missing_reference_dependencies": sorted(
            missing_reference_dependencies,
            key=lambda item: (item["from"].casefold(), item["import"].casefold()),
        ),
        "duplicate_binary_names": duplicate_groups(
            (Path(path).name, path) for path in local_binary_paths
        ),
        "reachable_paths": sorted(reachable, key=str.casefold),
        "static_unreachable_candidates": static_unreachable,
        "limitations": [
            "Static PE imports do not reveal UNO registry, configuration, data-file, or runtime-loaded component dependencies.",
            "A static-unreachable candidate is an investigation lead, not proof that a file is safe to remove.",
            "External imports may be supplied by Windows or the application directory and are not automatically errors.",
        ],
    }


def build_integrity(
    root: Path,
    tree: dict[str, object],
    dependencies: dict[str, object],
    required_paths: Sequence[str],
) -> dict[str, object]:
    file_items = list(tree["files"])
    file_paths = [str(item["path"]) for item in file_items]
    errors: list[dict[str, str]] = []
    warnings: list[dict[str, str]] = []

    for required in required_paths:
        if not (root / required).is_file():
            errors.append({"kind": "missing-required-path", "path": required})
    for item in tree["symlinks"]:
        if item["escapes_root"]:
            errors.append({"kind": "symlink-escapes-root", "path": str(item["path"])})
    for group in duplicate_groups((path, path) for path in file_paths):
        errors.append({"kind": "case-colliding-path", "path": ", ".join(group["paths"])})
    for item in file_items:
        if int(item["size_bytes"]) == 0:
            warnings.append({"kind": "zero-byte-file", "path": str(item["path"])})
    for item in dependencies["missing_reference_dependencies"]:
        errors.append(
            {
                "kind": "missing-reference-dependency",
                "path": str(item["from"]),
                "detail": str(item["import"]),
            }
        )
    for group in dependencies["duplicate_binary_names"]:
        warnings.append({"kind": "duplicate-binary-name", "path": ", ".join(group["paths"])})

    return {
        "status": "ok" if not errors else "failed",
        "required_paths": list(required_paths),
        "errors": errors,
        "warnings": warnings,
    }


def analyze_runtime(
    root: Path,
    *,
    reference_root: Path | None = None,
    hashes: bool = False,
    largest: int = 30,
    required_paths: Sequence[str] = DEFAULT_REQUIRED_PATHS,
    entry_points: Sequence[str] = DEFAULT_ENTRY_POINTS,
) -> dict[str, object]:
    root = root.resolve()
    if not root.is_dir():
        raise FileNotFoundError(f"runtime root not found: {root}")
    if reference_root is not None:
        reference_root = reference_root.resolve()
        if not reference_root.is_dir():
            raise FileNotFoundError(f"reference runtime root not found: {reference_root}")

    tree = scan_tree(root, hashes=hashes)
    files_by_size = sorted(tree["files"], key=lambda item: (-int(item["size_bytes"]), str(item["path"])))
    inventory = {
        "summary": tree["summary"],
        "by_top_level": tree["by_top_level"],
        "by_extension": tree["by_extension"],
        "largest_files": files_by_size[: max(0, largest)],
        "files": tree["files"],
        "symlinks": tree["symlinks"],
    }
    dependencies = analyze_dependencies(
        root,
        tree["files"],
        reference_root=reference_root,
        entry_points=entry_points,
    )
    integrity = build_integrity(root, tree, dependencies, required_paths)
    return {
        "tool": "libreoffice_runtime_analyzer",
        "report_version": REPORT_VERSION,
        "mode": "analyze",
        "runtime_root": str(root),
        "reference_root": str(reference_root) if reference_root else None,
        "hashes_included": hashes,
        "inventory": inventory,
        "integrity": integrity,
        "dependencies": dependencies,
    }


def file_map(root: Path, *, hashes: bool) -> tuple[dict[str, dict[str, object]], dict[str, object]]:
    tree = scan_tree(root, hashes=hashes)
    return {str(item["path"]): item for item in tree["files"]}, tree["summary"]


def compare_runtimes(baseline: Path, candidate: Path, *, hashes: bool = False) -> dict[str, object]:
    baseline = baseline.resolve()
    candidate = candidate.resolve()
    if not baseline.is_dir():
        raise FileNotFoundError(f"baseline runtime root not found: {baseline}")
    if not candidate.is_dir():
        raise FileNotFoundError(f"candidate runtime root not found: {candidate}")

    before, before_summary = file_map(baseline, hashes=hashes)
    after, after_summary = file_map(candidate, hashes=hashes)
    before_paths = set(before)
    after_paths = set(after)
    added = sorted(after_paths - before_paths, key=str.casefold)
    removed = sorted(before_paths - after_paths, key=str.casefold)
    changed: list[dict[str, object]] = []
    for path in sorted(before_paths & after_paths, key=str.casefold):
        size_changed = before[path]["size_bytes"] != after[path]["size_bytes"]
        hash_changed = hashes and before[path].get("sha256") != after[path].get("sha256")
        if size_changed or hash_changed:
            changed.append(
                {
                    "path": path,
                    "baseline_size_bytes": before[path]["size_bytes"],
                    "candidate_size_bytes": after[path]["size_bytes"],
                    "baseline_sha256": before[path].get("sha256"),
                    "candidate_sha256": after[path].get("sha256"),
                }
            )
    baseline_bytes = int(before_summary["bytes"])
    candidate_bytes = int(after_summary["bytes"])
    return {
        "tool": "libreoffice_runtime_analyzer",
        "report_version": REPORT_VERSION,
        "mode": "compare",
        "baseline_root": str(baseline),
        "candidate_root": str(candidate),
        "hashes_included": hashes,
        "summary": {
            "baseline_files": len(before),
            "candidate_files": len(after),
            "baseline_bytes": baseline_bytes,
            "candidate_bytes": candidate_bytes,
            "bytes_delta": candidate_bytes - baseline_bytes,
            "added_files": len(added),
            "removed_files": len(removed),
            "changed_files": len(changed),
        },
        "added": [{"path": path, **after[path]} for path in added],
        "removed": [{"path": path, **before[path]} for path in removed],
        "changed": changed,
    }


def write_json_atomic(output: Path, payload: dict[str, object], protected_roots: Sequence[Path]) -> None:
    output = output.resolve()
    for root in protected_roots:
        try:
            output.relative_to(root.resolve())
        except ValueError:
            continue
        raise ValueError(f"refusing to write analysis output inside runtime root: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temp = output.with_name(output.name + f".{os.getpid()}.tmp")
    try:
        temp.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        os.replace(temp, output)
    finally:
        if temp.exists():
            temp.unlink()


def print_summary(payload: dict[str, object]) -> None:
    if payload["mode"] == "analyze":
        summary = payload["inventory"]["summary"]
        dep_summary = payload["dependencies"]["summary"]
        print(
            f"files={summary['files']} bytes={summary['bytes']} binaries={dep_summary['binaries']} "
            f"integrity={payload['integrity']['status']}"
        )
    else:
        summary = payload["summary"]
        print(
            f"bytes_delta={summary['bytes_delta']} added={summary['added_files']} "
            f"removed={summary['removed_files']} changed={summary['changed_files']}"
        )


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Analyze LibreOffice runtime capacity, integrity, static dependencies, and structural differences."
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    analyze = subparsers.add_parser("analyze", help="Inventory and validate one runtime.")
    analyze.add_argument("--root", required=True, help="Runtime root containing program/soffice.com.")
    analyze.add_argument(
        "--reference-root",
        help="Optional prior runtime used to detect imports whose local DLL disappeared.",
    )
    analyze.add_argument("--hashes", action="store_true", help="Include SHA-256 for every file.")
    analyze.add_argument("--largest", type=int, default=30, help="Number of largest files to report.")
    analyze.add_argument("--required", action="append", help="Override required relative runtime path.")
    analyze.add_argument("--entry-point", action="append", help="Override static dependency entry point.")
    analyze.add_argument("--output", help="Optional JSON output path outside the runtime root.")
    analyze.add_argument("--format", choices=("text", "json"), default="text")

    compare = subparsers.add_parser("compare", help="Compare baseline and candidate runtime trees.")
    compare.add_argument("--baseline", required=True)
    compare.add_argument("--candidate", required=True)
    compare.add_argument("--hashes", action="store_true", help="Detect same-size content changes with SHA-256.")
    compare.add_argument("--output", help="Optional JSON output path outside both runtime roots.")
    compare.add_argument("--format", choices=("text", "json"), default="text")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    binary_scan.configure_utf8_output()
    args = build_parser().parse_args(argv)
    try:
        if args.command == "analyze":
            root = Path(args.root)
            reference = Path(args.reference_root) if args.reference_root else None
            payload = analyze_runtime(
                root,
                reference_root=reference,
                hashes=args.hashes,
                largest=args.largest,
                required_paths=args.required or DEFAULT_REQUIRED_PATHS,
                entry_points=args.entry_point or DEFAULT_ENTRY_POINTS,
            )
            protected = [root]
            if reference is not None:
                protected.append(reference)
        else:
            baseline = Path(args.baseline)
            candidate = Path(args.candidate)
            payload = compare_runtimes(baseline, candidate, hashes=args.hashes)
            protected = [baseline, candidate]

        if args.output:
            write_json_atomic(Path(args.output), payload, protected)
        if args.format == "json":
            print(json.dumps(payload, ensure_ascii=False, indent=2))
        else:
            print_summary(payload)
            if args.output:
                print(f"report={Path(args.output).resolve()}")
        if payload["mode"] == "analyze" and payload["integrity"]["status"] != "ok":
            return 1
        return 0
    except (OSError, ValueError) as exc:
        print(f"[NG] {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
