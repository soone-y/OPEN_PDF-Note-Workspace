"""Run a local, offscreen comparison against explicit line-delimited \\[ blocks.

The developer supplies a prebuilt probe and local resources. Each measurement
runs in its own process. Input stays read-only, and every run uses a fresh out/
directory. This is a comparison experiment, not a product acceptance gate.
"""

from __future__ import annotations

import argparse
import hashlib
import html
import json
import math
from pathlib import Path
import subprocess
import time


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def extract_blocks(text: str) -> list[tuple[str, str]]:
    """Only accept the standalone delimiters used by the supplied experiment."""
    blocks = []
    title = "数式"
    body = None
    for line in text.splitlines():
        stripped = line.strip()
        if stripped == r"\[":
            if body is not None:
                raise ValueError("Nested opening delimiter")
            body = []
        elif stripped == r"\]":
            if body is None:
                raise ValueError("Closing delimiter without an opening delimiter")
            formula = "\n".join(body).strip()
            if not formula:
                raise ValueError("Empty formula")
            blocks.append((title, formula))
            body = None
        elif body is not None:
            body.append(line)
        elif stripped.startswith("#"):
            title = stripped.lstrip("#").strip()
    if body is not None:
        raise ValueError("Unclosed formula")
    return blocks


def measure(command: list[str], work: Path, log: Path) -> dict:
    # The probe starts no subprocesses. Reap it after failure or timeout.
    with log.open("xb") as stream:
        try:
            process = subprocess.Popen(command, cwd=work, stdout=stream, stderr=stream,
                                       creationflags=subprocess.CREATE_NO_WINDOW)
        except OSError as error:
            return {"status": "process_start_failed", "error": str(error), "log": log.name}
        deadline = time.monotonic() + 30
        reason = None
        try:
            while process.poll() is None:
                if log.stat().st_size > 65536:
                    reason = "log_budget_exceeded"
                    break
                if time.monotonic() >= deadline:
                    reason = "timeout"
                    break
                time.sleep(0.1)
        finally:
            if process.poll() is None:
                process.kill()
            code = process.wait()
    if reason or code:
        return {"status": reason or "process_failed", "exit_code": code, "log": log.name}
    if log.stat().st_size > 65536:
        return {"status": "log_budget_exceeded", "exit_code": code, "log": log.name}
    try:
        metrics = json.loads(log.read_text(encoding="utf-8"))
        required = {"width", "height", "baseline", "init_ms", "first_prepare_ms", "prepare_ms", "draw_ms"}
        if set(metrics) != required:
            raise ValueError("Missing measurement fields")
        if not all(type(value) in (int, float) and math.isfinite(value) and value >= 0
                   for value in metrics.values()):
            raise ValueError("Invalid measurement values")
        if metrics["width"] <= 0 or metrics["height"] <= 0 or metrics["baseline"] > metrics["height"]:
            raise ValueError("Invalid layout")
        return {"status": "render_completed", **metrics, "log": log.name}
    except (ValueError, TypeError):
        return {"status": "invalid_measurements", "exit_code": code, "log": log.name}


def write_gallery(output: Path, rows: list[dict], backend: str) -> None:
    sections = []
    for row in rows:
        measurements = row["engines"]
        images = []
        for engine, label in (("current", "現行 mathrender 単体API"),
                              ("microtex", f"MicroTeX {backend} 単体API")):
            result = measurements[engine]
            details = html.escape(json.dumps(result, ensure_ascii=False))
            image = ""
            if result["status"] == "render_completed":
                image = f'<img alt="{label}の描画" src="{result["image"]}">'
            images.append(f"<article><h3>{label}</h3><pre>{details}</pre><div>{image}</div></article>")
        title = html.escape(row["title"])
        sections.append(f'<section><h2>{row["case"]}. {title} / {row["pixels"]}px</h2>'
                        f'<div class="pair">{"".join(images)}</div></section>')
    page = ('<!doctype html><html lang="ja"><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<title>数式エンジン単体API比較</title><style>'
            'body{font-family:system-ui;margin:24px;background:#eee;color:#111}'
            '.pair{display:flex;gap:16px}article{min-width:0;flex:1;background:white;padding:12px}'
            'article>div{overflow:auto}pre{white-space:pre-wrap;overflow-wrap:anywhere;font-size:12px}'
            'img{max-width:none}section{margin-bottom:32px}'
            '@media(max-width:800px){.pair{display:block}}'
            '</style><h1>数式エンジン単体API比較</h1>'
            '<p>両エンジンの単体APIへ数式本文を直接渡したオフスクリーン描画。'
            'アプリ内の表示比較には、前処理・フォント・DPIをそろえた実アプリ画像を用いる。'
            '画像は原寸表示。'
            '処理時間の単位はms。描画完了と組版の正確性は個別に確認する。</p>'
            + "".join(sections) + '</html>')
    (output / "comparison.html").write_text(page, encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--resources", type=Path, required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--font-px", type=int, nargs="+", default=[16, 24, 36])
    parser.add_argument("--iterations", type=int, default=30)
    parser.add_argument("--expected-blocks", type=int, default=7)
    parser.add_argument("--microtex-backend", choices=("legacy", "openmath"), default="legacy")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[3]
    output = args.output.resolve()
    output.relative_to((repo / "out").resolve())
    source = args.input.resolve(strict=True)
    exe = args.exe.resolve(strict=True)
    resources = args.resources.resolve(strict=True)
    if source.stat().st_size > 1024 * 1024:
        parser.error("Input exceeds 1 MiB")
    if not 1 <= args.iterations <= 1000 or not args.font_px or any(
        pixels < 8 or pixels > 120 for pixels in args.font_px
    ):
        parser.error("Invalid iteration count or font size")
    original_hash = sha256(source)
    blocks = extract_blocks(source.read_text(encoding="utf-8-sig"))
    if len(blocks) != args.expected_blocks:
        parser.error(f"Expected {args.expected_blocks} blocks, found {len(blocks)}")
    output.mkdir(parents=True, exist_ok=False)
    (output / "input.clro").write_bytes(source.read_bytes())
    if sha256(output / "input.clro") != original_hash:
        raise RuntimeError("Input changed while copying")
    identity = {
        "input_sha256": original_hash,
        "exe_sha256": sha256(exe),
        "current_source_sha256": sha256(repo / "src/math/math_render.cpp"),
        "current_header_sha256": sha256(repo / "src/math/math_render.h"),
        "probe_sha256": sha256(Path(__file__).with_name("probe.cpp")),
        "cmake_sha256": sha256(Path(__file__).with_name("CMakeLists.txt")),
        "openmath_adapter_sha256": sha256(Path(__file__).with_name("openmath_gdi_path.h"))
            if args.microtex_backend == "openmath" else None,
        "runner_sha256": sha256(Path(__file__)),
        "resources": {str(path.relative_to(resources)): sha256(path)
                      for path in sorted(resources.rglob("*")) if path.is_file()},
        "font_px": args.font_px, "iterations": args.iterations,
        "style": "display", "font": "Cambria Math", "blocks": len(blocks),
        "microtex_parser": "strict",
        "microtex_backend": args.microtex_backend,
    }
    (output / "identity.json").write_text(json.dumps(identity, indent=2), encoding="utf-8")
    rows = []
    for case, (title, formula) in enumerate(blocks, 1):
        input_path = output / f"case_{case:02}.tex"
        input_path.write_text(formula, encoding="utf-8")
        for pixels in args.font_px:
            row = {"case": case, "title": title, "pixels": pixels, "engines": {}}
            for engine in ("current", "microtex"):
                stem = f"case_{case:02}_{pixels}_{engine}"
                image_path = output / f"{stem}.png"
                result = measure([str(exe), engine, str(resources), str(input_path),
                                  str(image_path), str(pixels), "display", str(args.iterations)],
                                 output, output / f"{stem}.log")
                if result["status"] == "render_completed":
                    if not image_path.is_file() or image_path.stat().st_size == 0:
                        result["status"] = "missing_image"
                    else:
                        result["image"] = image_path.name
                row["engines"][engine] = result
            rows.append(row)
            print(f"case {case}/{len(blocks)} {pixels}px " + " ".join(
                f"{engine}={result['status']}" for engine, result in row["engines"].items()), flush=True)
    input_unchanged = original_hash == sha256(source) == sha256(output / "input.clro")
    completed = sum(result["status"] == "render_completed"
                    for row in rows for result in row["engines"].values())
    total = len(blocks) * len(args.font_px) * 2
    summary = {"input_unchanged": input_unchanged, "render_completed": completed,
               "expected": total, "quality_review": "pending", "rows": rows}
    (output / "summary.json").write_text(json.dumps(summary, ensure_ascii=False, indent=2), encoding="utf-8")
    write_gallery(output, rows, args.microtex_backend)
    print(f"Rendered {completed}/{total}, input_unchanged={input_unchanged}, evidence={output}")
    return 0 if completed == total and input_unchanged else 1


if __name__ == "__main__":
    raise SystemExit(main())
