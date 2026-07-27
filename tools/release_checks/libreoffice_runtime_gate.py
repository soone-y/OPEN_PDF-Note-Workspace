#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence

import binary_scan


# A conversion runtime is not admissible while it includes code capable of
# network communication, update checks, mail sending, crash submission, or
# app-initiated notification sounds.
PROHIBITED_PATHS = (
    "program/libcurl.dll",
    "program/updater.exe",
    "program/update_service.exe",
    "program/updatecheckuilo.dll",
    "program/updchklo.dll",
    "program/minidump_upload.exe",
    "program/senddoc.exe",
    "share/registry/onlineupdate.xcd",
)

PROHIBITED_IMPORTED_DLLS = {
    "libcurl.dll",
    "winhttp.dll",
    "wininet.dll",
    "urlmon.dll",
    "ws2_32.dll",
    "wsock32.dll",
    "websocket" + ".dll",
}

PROHIBITED_MARKERS = (
    "ucb.ucp.webdav.curl",
    "curl_easy_perform",
    "winhttpopen",
    "upload_file_minidump",
    "updatechannel=loonlineupdater",
    "playsound",
    "sndplaysound",
    "messagebeep",
    "telemetry",
)

POLICY_TEXT_FILES = (
    "program/version.ini",
    "update-settings.ini",
    "share/registry/onlineupdate.xcd",
)


@dataclass(frozen=True)
class RuntimeViolation:
    kind: str
    path: str
    evidence: str


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Reject LibreOffice conversion runtimes that retain prohibited communication functionality."
    )
    parser.add_argument(
        "--image",
        default="third_party/libreoffice/custom_runtime/instdir",
        help="LibreOffice image directory to validate. Defaults to the repository-local custom runtime.",
    )
    parser.add_argument("--format", choices=("text", "json"), default="text")
    return parser.parse_args(argv)


def collect_violations(image_root: Path) -> list[RuntimeViolation]:
    violations: list[RuntimeViolation] = []
    program_dir = image_root / "program"
    soffice = program_dir / "soffice.com"
    if not soffice.is_file():
        return [RuntimeViolation("missing-entry-point", "program/soffice.com", "required converter entry point is absent")]

    for relative in PROHIBITED_PATHS:
        if (image_root / relative).exists():
            violations.append(RuntimeViolation("prohibited-path", relative, "prohibited runtime file is present"))

    for path in sorted(binary_scan.iter_binary_files(image_root, ["program"], all_files=False)):
        finding = binary_scan.scan_file(
            path,
            image_root,
            PROHIBITED_MARKERS,
            min_string=5,
            max_strings=40,
        )
        relative = path.relative_to(image_root).as_posix()
        for imported in finding.import_dlls:
            if imported.lower() in PROHIBITED_IMPORTED_DLLS:
                violations.append(RuntimeViolation("prohibited-import", relative, imported))
        for marker in finding.matched_imports + finding.matched_strings:
            violations.append(RuntimeViolation("prohibited-marker", relative, marker))

    for relative in POLICY_TEXT_FILES:
        path = image_root / relative
        if not path.is_file():
            continue
        finding = binary_scan.scan_file(
            path,
            image_root,
            PROHIBITED_MARKERS,
            min_string=5,
            max_strings=40,
        )
        for marker in finding.matched_strings:
            violations.append(RuntimeViolation("prohibited-marker", relative, marker))

    deduped: list[RuntimeViolation] = []
    seen: set[tuple[str, str, str]] = set()
    for violation in violations:
        key = (violation.kind, violation.path, violation.evidence)
        if key not in seen:
            seen.add(key)
            deduped.append(violation)
    return deduped


def main(argv: Sequence[str] | None = None) -> int:
    binary_scan.configure_utf8_output()
    args = parse_args(argv)
    image_root = Path(args.image).resolve()
    violations = collect_violations(image_root)

    if args.format == "json":
        print(
            json.dumps(
                {
                    "image": str(image_root),
                    "eligible_for_office_conversion_release": not violations,
                    "violations": [asdict(item) for item in violations],
                },
                ensure_ascii=False,
                indent=2,
            )
        )
    elif violations:
        print(f"[NG] LibreOffice conversion runtime is not eligible: {image_root}")
        for item in violations:
            print(f"  {item.kind}: {item.path}: {item.evidence}")
    else:
        print(f"[OK] LibreOffice conversion runtime contains no prohibited static indicators: {image_root}")

    return 1 if violations else 0


if __name__ == "__main__":
    raise SystemExit(main())
