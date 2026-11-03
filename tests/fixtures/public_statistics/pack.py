"""Tiny non-executable distributions for release-control-flow tests only."""
from __future__ import annotations

import argparse
import hashlib
import zipfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--locale", required=True)
    parser.add_argument("--edition", required=True)
    args = parser.parse_args()
    name = "release_FIXTURE" if args.edition == "full" else "release_Lite_FIXTURE"
    directory = args.out / name
    directory.mkdir(parents=True)
    (directory / "docs").mkdir()
    (directory / "docs/README.md").write_text("fixture only", encoding="utf-8")
    for filename in ("pdf_note_workspace.exe", "readonly_viewer.exe", "pdfium.dll"):
        (directory / filename).write_bytes((filename + args.locale + args.edition).encode("ascii"))
    if args.edition == "full":
        (directory / "lo").mkdir()
        (directory / "lo/fixture.txt").write_text("fixture runtime", encoding="utf-8")
    for executable in ("pdf_note_workspace.exe", "readonly_viewer.exe"):
        text = f"format\tpdf-note-build-info-v1\nversion\t1.2.3\nlocale\t{args.locale}\n"
        if executable == "pdf_note_workspace.exe": text += f"edition\t{args.edition}\n"
        for artifact in (executable, "pdfium.dll"):
            text += f"artifact\t{artifact}\t{hashlib.sha256((directory / artifact).read_bytes()).hexdigest()}\n"
        (directory / (executable + ".buildinfo.txt")).write_text(text, encoding="utf-8")
    entries = []
    for path in sorted(directory.rglob("*")):
        if path.is_file(): entries.append(hashlib.sha256(path.read_bytes()).hexdigest() + "  " + path.relative_to(directory).as_posix())
    (directory / "checksums.sha256").write_text("\n".join(entries) + "\n", encoding="utf-8")
    with zipfile.ZipFile(args.out / (name + ".zip"), "w") as archive:
        for path in directory.rglob("*"):
            if path.is_file(): archive.write(path, "PDF-Note-Workspace-1.2.3/" + path.relative_to(directory).as_posix())


if __name__ == "__main__":
    main()
