#!/usr/bin/env python3
"""
Batch-migrate original .clrop files in a workspace into the current v1 schema.

Safety behavior:
- Read source first.
- Build migrated JSON in memory.
- Rename old original file to backup name in-place.
- Write new .clrop via temp + atomic replace using the original name.
- If write fails, restore backup.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import hashlib
import json
import os
import re
import sys
from pathlib import Path
from typing import Any, Dict, Iterable, List, Optional, Sequence, Tuple

CURRENT_EXT = ".clrop"
BACKUP_STEM_RE = re.compile(r"\.legacy_\d{8}_\d{6}(?:_\d+)?$", re.IGNORECASE)
WORKSPACE_ROOT_RE = re.compile(r'"workspaceRoot"\s*:\s*"([^"]+)"')
TEMP_EXTERNAL_DIRS_RE = re.compile(r'"tempExternalLectureDirs"\s*:\s*\[(.*?)\]', re.DOTALL)
QUOTED_STRING_RE = re.compile(r'"([^"]*)"')
RESOURCE_DIR_NAME = "__resource__"
SETUP_JSON_NAMES = ("pdf_workspace_setup.json", "pdf_viewer_setup.json")

RECOGNIZED_TYPES = {
    "text",
    "math",
    "marker-text",
    "marker-free",
    "line",
    "arrow",
    "wave",
    "freehand",
    "link-marker",
    "shape",
}

INT_TYPE_MAP = {
    0: "marker-text",
    1: "marker-free",
    2: "text",
    3: "line",
    4: "arrow",
    5: "wave",
    6: "freehand",
    7: "link-marker",
    8: "math",
}


def now_iso() -> str:
    return _dt.datetime.now(_dt.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def num(v: Any, default: float = 0.0) -> float:
    if isinstance(v, bool):
        return default
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, str):
        try:
            return float(v.strip())
        except Exception:
            return default
    return default


def as_int(v: Any, default: int = 0) -> int:
    if isinstance(v, bool):
        return default
    if isinstance(v, int):
        return v
    if isinstance(v, float):
        return int(round(v))
    if isinstance(v, str):
        try:
            return int(round(float(v.strip())))
        except Exception:
            return default
    return default


def as_str(v: Any, default: str = "") -> str:
    if isinstance(v, str):
        return v
    if v is None:
        return default
    return str(v)


def clamp(v: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, v))


def to_hex_color(v: Any, fallback: str = "#000000") -> str:
    if isinstance(v, str):
        s = v.strip()
        if len(s) == 7 and s.startswith("#"):
            try:
                int(s[1:], 16)
                return "#" + s[1:].upper()
            except Exception:
                return fallback
        if len(s) == 6:
            try:
                int(s, 16)
                return "#" + s.upper()
            except Exception:
                return fallback
        return fallback
    if isinstance(v, int):
        # COLORREF -> 0x00BBGGRR
        r = v & 0xFF
        g = (v >> 8) & 0xFF
        b = (v >> 16) & 0xFF
        return f"#{r:02X}{g:02X}{b:02X}"
    return fallback


def normalize_type(raw: Any) -> Optional[str]:
    if isinstance(raw, int):
        return INT_TYPE_MAP.get(raw)
    s = as_str(raw, "").strip().lower().replace("_", "-").replace(" ", "-")
    if not s:
        return None
    alias = {
        "textbox": "text",
        "text-box": "text",
        "mathbox": "math",
        "math-box": "math",
        "markertext": "marker-text",
        "markerfree": "marker-free",
        "highlight": "marker-text",
        "linkmarker": "link-marker",
    }
    s = alias.get(s, s)
    if s in RECOGNIZED_TYPES:
        return s
    return None


def parse_point(v: Any) -> Optional[List[float]]:
    if isinstance(v, (list, tuple)) and len(v) >= 2:
        return [num(v[0]), num(v[1])]
    if isinstance(v, dict):
        if "x" in v and "y" in v:
            return [num(v.get("x")), num(v.get("y"))]
        if "px" in v and "py" in v:
            return [num(v.get("px")), num(v.get("py"))]
    return None


def normalize_path(v: Any) -> List[List[float]]:
    out: List[List[float]] = []
    if not isinstance(v, list):
        return out
    if v and all(isinstance(x, (int, float)) for x in v):
        for i in range(0, len(v) - 1, 2):
            out.append([num(v[i]), num(v[i + 1])])
        return out
    for e in v:
        p = parse_point(e)
        if p is not None:
            out.append(p)
    return out


def normalize_quads(v: Any) -> List[List[float]]:
    out: List[List[float]] = []
    if not isinstance(v, list):
        return out
    if v and all(isinstance(x, (int, float)) for x in v):
        for i in range(0, len(v), 8):
            seg = v[i : i + 8]
            if len(seg) == 8:
                out.append([num(x) for x in seg])
        return out
    for e in v:
        if isinstance(e, list) and len(e) == 8:
            out.append([num(x) for x in e])
    return out


def rect_to_quad(x1: float, y1: float, x2: float, y2: float) -> List[float]:
    min_x = min(x1, x2)
    max_x = max(x1, x2)
    min_y = min(y1, y2)
    max_y = max(y1, y2)
    return [min_x, max_y, max_x, max_y, max_x, min_y, min_x, min_y]


def parse_bbox(v: Any) -> Optional[List[float]]:
    if isinstance(v, (list, tuple)) and len(v) == 4:
        return [num(v[0]), num(v[1]), num(v[2]), num(v[3])]
    return None


def build_text_bbox(item: Dict[str, Any]) -> Optional[List[float]]:
    bbox = parse_bbox(item.get("bbox"))
    if bbox is not None:
        return bbox
    if all(k in item for k in ("x1", "y1", "x2", "y2")):
        x1 = num(item.get("x1"))
        y1 = num(item.get("y1"))
        x2 = num(item.get("x2"))
        y2 = num(item.get("y2"))
        left = min(x1, x2)
        right = max(x1, x2)
        top = max(y1, y2)
        bottom = min(y1, y2)
        return [left, top, right - left, top - bottom]
    return None


def normalize_shape_kind(v: Any) -> str:
    s = as_str(v, "").strip().lower().replace("_", "-").replace(" ", "-")
    return s if s else "rect"


def normalize_shape_draw_mode(v: Any) -> str:
    s = as_str(v, "").strip().lower().replace("_", "-").replace(" ", "-")
    if s in {"stroke", "fill", "fill-stroke"}:
        return s
    return "stroke"


def synthesize_item_id(src: Dict[str, Any], page_no: int, item_index: int) -> str:
    existing = as_str(src.get("id"), "").strip()
    if existing:
        return existing
    payload = json.dumps(src, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    digest = hashlib.sha1(payload.encode("utf-8")).hexdigest()[:12]
    return f"migrated-p{page_no + 1}-i{item_index + 1}-{digest}"


def normalize_item(src: Dict[str, Any], page_no: int, item_index: int) -> Optional[Dict[str, Any]]:
    kind = normalize_type(src.get("type"))
    if kind is None:
        return None

    created = as_str(src.get("created"), "") or now_iso()
    updated = as_str(src.get("updated"), "") or created

    color_default = "#FFFF00" if "marker" in kind else "#000000"
    out: Dict[str, Any] = {
        "type": kind,
        "id": synthesize_item_id(src, page_no, item_index),
        "created": created,
        "updated": updated,
        "color": to_hex_color(src.get("color"), color_default),
    }

    alpha_raw = src.get("alpha")
    alpha_default = 0.35 if kind in {"marker-text", "marker-free"} else 1.0
    alpha = clamp(num(alpha_raw, alpha_default), 0.0, 1.0)
    out["alpha"] = alpha

    if kind in {"line", "arrow", "wave"}:
        out["width"] = max(0.1, num(src.get("width"), 2.0))
        p1 = parse_point(src.get("p1"))
        p2 = parse_point(src.get("p2"))
        if p1 is None and all(k in src for k in ("x1", "y1")):
            p1 = [num(src.get("x1")), num(src.get("y1"))]
        if p2 is None and all(k in src for k in ("x2", "y2")):
            p2 = [num(src.get("x2")), num(src.get("y2"))]
        if (p1 is None or p2 is None) and isinstance(src.get("path"), list):
            pts = normalize_path(src.get("path"))
            if len(pts) >= 2:
                p1 = p1 or pts[0]
                p2 = p2 or pts[-1]
        if p1 is None or p2 is None:
            return None
        out["p1"] = p1
        out["p2"] = p2
        if isinstance(src.get("dash"), list):
            dash = [num(x) for x in src["dash"] if isinstance(x, (int, float))]
            if dash:
                out["dash"] = dash
        return out

    if kind in {"marker-free", "freehand"}:
        out["width"] = max(0.1, num(src.get("width"), 8.0 if kind == "marker-free" else 2.0))
        path = normalize_path(src.get("path"))
        if not path:
            return None
        out["path"] = path
        return out

    if kind == "marker-text":
        quads = normalize_quads(src.get("quads"))
        if not quads and all(k in src for k in ("x1", "y1", "x2", "y2")):
            quads = [rect_to_quad(num(src.get("x1")), num(src.get("y1")), num(src.get("x2")), num(src.get("y2")))]
        if not quads:
            return None
        out["quads"] = quads
        return out

    if kind in {"text", "math"}:
        out["pt"] = max(1.0, num(src.get("pt"), num(src.get("fontPt"), 12.0)))
        out["font"] = as_str(src.get("font"), as_str(src.get("fontName"), ""))
        out["border"] = as_str(src.get("border"), "none") or "none"
        out["content"] = as_str(src.get("content"), as_str(src.get("text"), ""))
        lines = src.get("lines")
        if isinstance(lines, list):
            out["lines"] = [as_str(x, "") for x in lines]
        bbox = build_text_bbox(src)
        if bbox is None:
            return None
        out["bbox"] = bbox
        if kind == "math":
            mk = as_str(src.get("math_kind"), as_str(src.get("mathKind"), "latex")).lower()
            out["math_kind"] = "markup" if mk == "markup" else "latex"
        return out

    if kind == "link-marker":
        out["width"] = max(0.1, num(src.get("width"), 6.0))
        p1 = parse_point(src.get("p1"))
        if p1 is None and all(k in src for k in ("x1", "y1")):
            p1 = [num(src.get("x1")), num(src.get("y1"))]
        if p1 is None:
            return None
        out["p1"] = p1
        out["link_id"] = as_str(src.get("link_id"), as_str(src.get("linkId"), ""))
        out["note_path"] = as_str(src.get("note_path"), as_str(src.get("notePath"), ""))
        return out

    if kind == "shape":
        bbox = build_text_bbox(src)
        if bbox is None:
            return None
        out["bbox"] = bbox
        out["width"] = max(0.1, num(src.get("width"), 2.0))
        out["shape_kind"] = normalize_shape_kind(src.get("shape_kind", src.get("shapeKind")))
        out["shape_draw_mode"] = normalize_shape_draw_mode(
            src.get("shape_draw_mode", src.get("shapeDrawMode"))
        )
        return out

    return None


def iter_source_pages(root: Dict[str, Any]) -> Iterable[Tuple[int, List[Dict[str, Any]]]]:
    pages = root.get("pages")
    if isinstance(pages, list):
        for p in pages:
            if not isinstance(p, dict):
                continue
            page_no = as_int(p.get("page", p.get("pageIndex", -1)), -1)
            items = p.get("items", p.get("annots"))
            if page_no < 0 or not isinstance(items, list):
                continue
            src_items = [x for x in items if isinstance(x, dict)]
            yield page_no, src_items
        return

    annots = root.get("annots", root.get("annotations"))
    if isinstance(annots, list):
        grouped: Dict[int, List[Dict[str, Any]]] = {}
        for a in annots:
            if not isinstance(a, dict):
                continue
            page_no = as_int(a.get("page", a.get("pageIndex", -1)), -1)
            if page_no < 0:
                continue
            grouped.setdefault(page_no, []).append(a)
        for k in sorted(grouped.keys()):
            yield k, grouped[k]
        return

    items = root.get("items")
    if isinstance(items, list):
        page_no = as_int(root.get("page", root.get("pageIndex", 0)), 0)
        src_items = [x for x in items if isinstance(x, dict)]
        yield page_no, src_items


def compute_pdf_id(clrop_path: Path, root: Dict[str, Any]) -> Dict[str, Any]:
    src_pdf = root.get("pdf_id")
    old_path = ""
    old_size = 0
    old_sha = ""
    if isinstance(src_pdf, dict):
        old_path = as_str(src_pdf.get("path"), "")
        old_size = max(0, as_int(src_pdf.get("size"), 0))
        old_sha = as_str(src_pdf.get("sha256"), "")

    pdf_path: Optional[Path] = None
    if old_path:
        p = Path(old_path)
        if p.exists():
            pdf_path = p
    if pdf_path is None:
        sibling = clrop_path.with_suffix(".pdf")
        if sibling.exists():
            pdf_path = sibling

    if pdf_path is None:
        path_str = old_path if old_path else str(clrop_path.with_suffix(".pdf"))
        return {
            "path": path_str,
            "size": old_size,
            "sha256": old_sha,
        }

    h = hashlib.sha256()
    with pdf_path.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return {
        "path": str(pdf_path),
        "size": pdf_path.stat().st_size,
        "sha256": h.hexdigest(),
    }


def normalize_document(root: Dict[str, Any], clrop_path: Path) -> Dict[str, Any]:
    out_pages: List[Dict[str, Any]] = []
    for page_no, src_items in iter_source_pages(root):
        items: List[Dict[str, Any]] = []
        for item_index, src in enumerate(src_items):
            ni = normalize_item(src, page_no, item_index)
            if ni is not None:
                items.append(ni)
        if items:
            out_pages.append({"page": page_no, "items": items})

    out_pages.sort(key=lambda p: as_int(p.get("page"), 0))

    return {
        "version": 1,
        "pdf_id": compute_pdf_id(clrop_path, root),
        "pages": out_pages,
    }


def is_current_v1_strict(doc: Dict[str, Any]) -> bool:
    if not isinstance(doc, dict):
        return False
    if as_int(doc.get("version"), -1) != 1:
        return False
    pid = doc.get("pdf_id")
    if not isinstance(pid, dict):
        return False
    if not isinstance(pid.get("path"), str):
        return False
    if not isinstance(pid.get("size"), int):
        return False
    if not isinstance(pid.get("sha256"), str):
        return False
    pages = doc.get("pages")
    if not isinstance(pages, list):
        return False
    for p in pages:
        if not isinstance(p, dict):
            return False
        if not isinstance(p.get("page"), int):
            return False
        items = p.get("items")
        if not isinstance(items, list):
            return False
        for it in items:
            if not isinstance(it, dict):
                return False
            t = normalize_type(it.get("type"))
            if t is None:
                return False
            if not isinstance(it.get("id"), str) or not it.get("id"):
                return False
            if not isinstance(it.get("created"), str) or not it.get("created"):
                return False
            if not isinstance(it.get("updated"), str) or not it.get("updated"):
                return False
            if t in {"line", "arrow", "wave"}:
                if parse_point(it.get("p1")) is None or parse_point(it.get("p2")) is None:
                    return False
            elif t in {"marker-free", "freehand"}:
                if not normalize_path(it.get("path")):
                    return False
            elif t == "marker-text":
                if not normalize_quads(it.get("quads")):
                    return False
            elif t in {"text", "math"}:
                if parse_bbox(it.get("bbox")) is None:
                    return False
            elif t == "link-marker":
                if parse_point(it.get("p1")) is None:
                    return False
            elif t == "shape":
                if parse_bbox(it.get("bbox")) is None:
                    return False
    return True


def read_json(path: Path) -> Dict[str, Any]:
    raw = path.read_text(encoding="utf-8-sig")
    obj = json.loads(raw)
    if not isinstance(obj, dict):
        raise ValueError("root is not object")
    return obj


def backup_name(path: Path) -> Path:
    ts = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    base = path.with_suffix("")
    ext = path.suffix
    for n in range(1000):
        suffix = f".legacy_{ts}" if n == 0 else f".legacy_{ts}_{n}"
        cand = Path(str(base) + suffix + ext)
        if not cand.exists():
            return cand
    raise RuntimeError(f"failed to allocate backup name for {path}")


def write_atomic(path: Path, text: str) -> None:
    tmp = path.with_suffix(path.suffix + ".tmp_migrate")
    with tmp.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def write_report(path: Path, payload: Dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(payload, ensure_ascii=False, indent=2)
    write_atomic(path, text + "\n")


def is_generated_backup_path(path: Path) -> bool:
    return bool(BACKUP_STEM_RE.search(path.stem))


def load_workspace_root_from_setup(setup_path: Path) -> Path:
    raw = setup_path.read_text(encoding="utf-8-sig")
    workspace_root: Optional[str] = None
    try:
        obj = json.loads(raw)
        if isinstance(obj, dict):
            candidate = obj.get("workspaceRoot")
            if isinstance(candidate, str) and candidate.strip():
                workspace_root = candidate.strip()
    except Exception:
        workspace_root = None
    if workspace_root is None:
        match = WORKSPACE_ROOT_RE.search(raw)
        if match:
            workspace_root = match.group(1).strip()
    if not workspace_root:
        raise ValueError(f"workspaceRoot not found in setup json: {setup_path}")
    root = Path(workspace_root)
    if not root.is_absolute():
        root = setup_path.parent / root
    return root


def load_scan_roots_from_setup(setup_path: Path) -> List[Path]:
    raw = setup_path.read_text(encoding="utf-8-sig")
    roots: List[Path] = []
    primary = load_workspace_root_from_setup(setup_path)
    roots.append(primary)

    extra_dirs: List[str] = []
    try:
        obj = json.loads(raw)
        if isinstance(obj, dict):
            value = obj.get("tempExternalLectureDirs")
            if isinstance(value, list):
                for item in value:
                    if isinstance(item, str) and item.strip():
                        extra_dirs.append(item.strip())
    except Exception:
        pass

    if not extra_dirs:
        match = TEMP_EXTERNAL_DIRS_RE.search(raw)
        if match:
            extra_dirs = [s.strip() for s in QUOTED_STRING_RE.findall(match.group(1)) if s.strip()]

    for item in extra_dirs:
        path = Path(item)
        if not path.is_absolute():
            path = setup_path.parent / path
        roots.append(path)

    seen = set()
    unique_roots: List[Path] = []
    for root in roots:
        key = str(root).lower()
        if key in seen:
            continue
        seen.add(key)
        unique_roots.append(root)
    return unique_roots


def resolve_scan_roots(root_arg: Optional[str], setup_json_arg: Optional[str]) -> List[Path]:
    if root_arg and setup_json_arg:
        raise ValueError("--root and --setup-json cannot be used together")
    if root_arg:
        return [Path(root_arg)]
    if setup_json_arg:
        return load_scan_roots_from_setup(Path(setup_json_arg))
    for name in SETUP_JSON_NAMES:
        candidate = Path.cwd() / name
        if candidate.exists():
            return load_scan_roots_from_setup(candidate)
    raise ValueError("workspace root is not specified; pass --root or --setup-json")


def migrate_one(src_path: Path, force: bool, dry_run: bool) -> Tuple[str, str]:
    root = read_json(src_path)
    current_ok = is_current_v1_strict(root)
    if current_ok and not force:
        return ("skipped", "already-current")

    migrated = normalize_document(root, src_path)
    if not is_current_v1_strict(migrated):
        return ("failed", "normalized result is not strict-v1")

    json_text = json.dumps(migrated, ensure_ascii=False, separators=(",", ":"))

    if dry_run:
        return ("would-migrate", f"dry-run src={src_path.name} dst={src_path.name}")

    src_bak = backup_name(src_path)
    os.replace(src_path, src_bak)
    try:
        write_atomic(src_path, json_text)
    except Exception as e:
        # Restore old files if new write fails.
        if not src_path.exists() and src_bak.exists():
            os.replace(src_bak, src_path)
        return ("failed", f"write failed, restored backup: {e}")

    details = [f"backup={src_bak.name}"]
    details.append(f"dst={src_path.name}")
    return ("migrated", ", ".join(details))


def iter_clrop_files(root: Path) -> Iterable[Path]:
    resource_name = RESOURCE_DIR_NAME.lower()
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [name for name in dirnames if name.lower() != resource_name]
        for filename in filenames:
            path = Path(dirpath) / filename
            if path.suffix.lower() != CURRENT_EXT:
                continue
            if is_generated_backup_path(path):
                continue
            yield path


def iter_clrop_files_in_roots(roots: Sequence[Path]) -> Iterable[Path]:
    seen = set()
    for root in roots:
        if not root.exists() or not root.is_dir():
            continue
        for path in iter_clrop_files(root):
            key = str(path).lower()
            if key in seen:
                continue
            seen.add(key)
            yield path


def relative_to_any_root(path: Path, roots: Sequence[Path]) -> str:
    for root in roots:
        try:
            return str(path.relative_to(root))
        except Exception:
            continue
    return str(path)


def main(argv: Sequence[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Migrate original .clrop files under a workspace root into strict v1 format."
    )
    ap.add_argument("--root", help="Workspace root directory to scan recursively.")
    ap.add_argument(
        "--setup-json",
        help="Path to pdf_workspace_setup.json or pdf_viewer_setup.json used to resolve workspaceRoot.",
    )
    ap.add_argument("--dry-run", action="store_true", help="Show actions without modifying files.")
    ap.add_argument("--force", action="store_true", help="Rewrite even already-current v1 files.")
    ap.add_argument("--report", help="Write a JSON report to the given path.")
    args = ap.parse_args(argv)

    try:
        roots = resolve_scan_roots(args.root, args.setup_json)
    except Exception as e:
        print(f"[ERROR] {e}", file=sys.stderr)
        return 2
    existing_roots = [root for root in roots if root.exists() and root.is_dir()]
    if not existing_roots:
        joined = ", ".join(str(root) for root in roots)
        print(f"[ERROR] no scan roots found or directories missing: {joined}", file=sys.stderr)
        return 2

    files = sorted(iter_clrop_files_in_roots(existing_roots))
    if not files:
        print("[INFO] no .clrop files found")
        return 0

    stats = {
        "migrated": 0,
        "would-migrate": 0,
        "skipped": 0,
        "failed": 0,
    }
    results: List[Dict[str, str]] = []

    for p in files:
        try:
            status, detail = migrate_one(
                p,
                force=args.force,
                dry_run=args.dry_run,
            )
        except Exception as e:
            status, detail = ("failed", str(e))
        stats[status] = stats.get(status, 0) + 1
        results.append(
            {
                "path": str(p),
                "relative_path": relative_to_any_root(p, existing_roots),
                "status": status,
                "detail": detail,
            }
        )
        print(f"[{status}] {p} :: {detail}")

    print(
        "[SUMMARY] "
        + " ".join(
            f"{k}={stats.get(k, 0)}"
            for k in ("migrated", "would-migrate", "skipped", "failed")
        )
    )
    if args.report:
        payload = {
            "tool": "migrate_clrop_v1",
            "report_version": 1,
            "generated_at": now_iso(),
            "workspace_root": str(existing_roots[0]),
            "scan_roots": [str(root) for root in existing_roots],
            "dry_run": bool(args.dry_run),
            "force": bool(args.force),
            "counts": stats,
            "results": results,
        }
        try:
            write_report(Path(args.report), payload)
            print(f"[REPORT] {args.report}")
        except Exception as e:
            print(f"[ERROR] failed to write report: {e}", file=sys.stderr)
            return 1
    return 1 if stats.get("failed", 0) > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
