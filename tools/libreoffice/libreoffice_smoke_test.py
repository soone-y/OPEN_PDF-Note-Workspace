#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import time
import zipfile
from datetime import datetime
from pathlib import Path


REPO_LOCAL_RESOURCE_DIR = ".local/repo_resource"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create tiny DOCX/PPTX files and verify bundled LibreOffice can convert them to PDF."
    )
    parser.add_argument("--repo-root", default=".", help="Repository root. Defaults to current directory.")
    parser.add_argument(
        "--soffice",
        default="third_party/libreoffice/custom_runtime/instdir/program/soffice.com",
        help="Path to soffice.com relative to repo root.",
    )
    parser.add_argument("--timeout", type=int, default=180, help="Per-file conversion timeout seconds.")
    parser.add_argument("--keep", action="store_true", help="Keep temporary smoke-test files.")
    parser.add_argument(
        "--input-dir",
        default=None,
        help="Optional directory containing existing .docx/.pptx samples. If omitted, tiny samples are generated.",
    )
    parser.add_argument(
        "--require-docx",
        action="store_true",
        help="When --input-dir is used, fail unless at least one .docx sample exists.",
    )
    parser.add_argument(
        "--require-pptx",
        action="store_true",
        help="When --input-dir is used, fail unless at least one .pptx sample exists.",
    )
    parser.add_argument(
        "--docx-space-protection",
        choices=("off", "word-joiner-after-space", "word-joiner-token-after-space", "nbsp"),
        default="off",
        help=(
            "Experimental DOCX layout probe. Applies only to the temporary conversion copy. "
            "'word-joiner-token-after-space' protects tokens following ideographic spaces from "
            "LibreOffice line wrapping."
        ),
    )
    return parser.parse_args()


def write_zip(path: Path, entries: dict[str, str]) -> None:
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        for name, text in entries.items():
            archive.writestr(name, text.encode("utf-8"))


def create_docx(path: Path) -> None:
    write_zip(
        path,
        {
            "[Content_Types].xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>""",
            "_rels/.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>""",
            "word/_rels/document.xml.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"/>""",
            "word/document.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:body>
    <w:p><w:r><w:t>LibreOffice DOCX smoke</w:t></w:r></w:p>
    <w:sectPr><w:pgSz w:w="11906" w:h="16838"/><w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1440"/></w:sectPr>
  </w:body>
</w:document>""",
            "docProps/core.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Smoke</dc:title></cp:coreProperties>""",
            "docProps/app.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>PDF-Note-Workspace Smoke</Application></Properties>""",
        },
    )


def create_pptx(path: Path) -> None:
    write_zip(
        path,
        {
            "[Content_Types].xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/ppt/presentation.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.presentation.main+xml"/>
  <Override PartName="/ppt/slides/slide1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slide+xml"/>
  <Override PartName="/ppt/slideLayouts/slideLayout1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideLayout+xml"/>
  <Override PartName="/ppt/slideMasters/slideMaster1.xml" ContentType="application/vnd.openxmlformats-officedocument.presentationml.slideMaster+xml"/>
  <Override PartName="/ppt/theme/theme1.xml" ContentType="application/vnd.openxmlformats-officedocument.theme+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>""",
            "_rels/.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="ppt/presentation.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>""",
            "ppt/presentation.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:presentation xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <p:sldMasterIdLst><p:sldMasterId id="2147483648" r:id="rId1"/></p:sldMasterIdLst>
  <p:sldIdLst><p:sldId id="256" r:id="rId2"/></p:sldIdLst>
  <p:sldSz cx="9144000" cy="6858000" type="screen4x3"/><p:notesSz cx="6858000" cy="9144000"/>
</p:presentation>""",
            "ppt/_rels/presentation.xml.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="slideMasters/slideMaster1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide" Target="slides/slide1.xml"/>
</Relationships>""",
            "ppt/slides/slide1.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sld xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main">
  <p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr>
  <p:sp><p:nvSpPr><p:cNvPr id="2" name="Title 1"/><p:cNvSpPr/><p:nvPr/></p:nvSpPr><p:spPr><a:xfrm><a:off x="1000000" y="1000000"/><a:ext cx="7000000" cy="1000000"/></a:xfrm></p:spPr><p:txBody><a:bodyPr/><a:lstStyle/><a:p><a:r><a:t>LibreOffice PPTX smoke</a:t></a:r></a:p></p:txBody></p:sp>
  </p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>
</p:sld>""",
            "ppt/slides/_rels/slide1.xml.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/></Relationships>""",
            "ppt/slideLayouts/slideLayout1.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldLayout xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" type="blank"><p:cSld name="Blank"><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>""",
            "ppt/slideLayouts/_rels/slideLayout1.xml.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideMaster" Target="../slideMasters/slideMaster1.xml"/></Relationships>""",
            "ppt/slideMasters/slideMaster1.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:sldMaster xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main" xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"><p:cSld><p:spTree><p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr><p:grpSpPr><a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm></p:grpSpPr></p:spTree></p:cSld><p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/><p:sldLayoutIdLst><p:sldLayoutId id="2147483649" r:id="rId1"/></p:sldLayoutIdLst><p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles></p:sldMaster>""",
            "ppt/slideMasters/_rels/slideMaster1.xml.rels": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"><Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/slideLayout" Target="../slideLayouts/slideLayout1.xml"/><Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme" Target="../theme/theme1.xml"/></Relationships>""",
            "ppt/theme/theme1.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<a:theme xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main" name="Office"><a:themeElements><a:clrScheme name="Office"><a:dk1><a:sysClr val="windowText" lastClr="000000"/></a:dk1><a:lt1><a:sysClr val="window" lastClr="FFFFFF"/></a:lt1><a:dk2><a:srgbClr val="1F497D"/></a:dk2><a:lt2><a:srgbClr val="EEECE1"/></a:lt2><a:accent1><a:srgbClr val="4F81BD"/></a:accent1><a:accent2><a:srgbClr val="C0504D"/></a:accent2><a:accent3><a:srgbClr val="9BBB59"/></a:accent3><a:accent4><a:srgbClr val="8064A2"/></a:accent4><a:accent5><a:srgbClr val="4BACC6"/></a:accent5><a:accent6><a:srgbClr val="F79646"/></a:accent6><a:hlink><a:srgbClr val="0000FF"/></a:hlink><a:folHlink><a:srgbClr val="800080"/></a:folHlink></a:clrScheme><a:fontScheme name="Office"><a:majorFont><a:latin typeface="Arial"/></a:majorFont><a:minorFont><a:latin typeface="Arial"/></a:minorFont></a:fontScheme><a:fmtScheme name="Office"><a:fillStyleLst/><a:lnStyleLst/><a:effectStyleLst/><a:bgFillStyleLst/></a:fmtScheme></a:themeElements></a:theme>""",
            "docProps/core.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties" xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>Smoke</dc:title></cp:coreProperties>""",
            "docProps/app.xml": """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"><Application>PDF-Note-Workspace Smoke</Application></Properties>""",
        },
    )


def validate_pdf(path: Path) -> None:
    data = path.read_bytes()
    if len(data) < 1000 or not data.startswith(b"%PDF"):
        raise RuntimeError(f"invalid PDF output: {path} size={len(data)}")


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def unique_sorted(values: list[str]) -> list[str]:
    return sorted({value for value in values if value})


def is_japanese_space_token_char(ch: str) -> bool:
    code = ord(ch)
    return (
        0x3040 <= code <= 0x30FF
        or 0x3400 <= code <= 0x9FFF
        or 0xFF10 <= code <= 0xFF19
        or 0x0041 <= code <= 0x005A
        or 0x0061 <= code <= 0x007A
        or 0x0030 <= code <= 0x0039
        or 0x2010 <= code <= 0x2015
    )


def protect_japanese_space_tokens(text: str) -> str:
    chars = list(text)
    out: list[str] = []
    i = 0
    while i < len(chars):
        ch = chars[i]
        out.append(ch)
        if ch != "\u3000":
            i += 1
            continue

        i += 1
        token: list[str] = []
        while i < len(chars) and chars[i] != "\u3000" and not chars[i].isspace():
            token.append(chars[i])
            i += 1
        for index, token_ch in enumerate(token):
            out.append(token_ch)
            if (
                index + 1 < len(token)
                and is_japanese_space_token_char(token_ch)
                and is_japanese_space_token_char(token[index + 1])
            ):
                out.append("\u2060")
    return "".join(out)


def transform_docx_text_for_space_protection(text: str, mode: str) -> str:
    if mode == "off":
        return text
    if mode == "word-joiner-after-space":
        return re.sub("\u3000(?=[^\u3000\\s])", "\u3000\u2060", text)
    if mode == "word-joiner-token-after-space":
        return protect_japanese_space_tokens(text)
    if mode == "nbsp":
        return text.replace("\u3000", "\u00a0")
    raise ValueError(f"unknown docx space protection mode: {mode}")


def transform_docx_for_space_protection(source: Path, dest: Path, mode: str) -> None:
    if mode == "off" or source.suffix.casefold() != ".docx":
        shutil.copy2(source, dest)
        return

    with zipfile.ZipFile(source, "r") as source_archive, zipfile.ZipFile(
        dest, "w", compression=zipfile.ZIP_DEFLATED
    ) as dest_archive:
        for info in source_archive.infolist():
            data = source_archive.read(info.filename)
            if info.filename == "word/document.xml":
                text = data.decode("utf-8")

                def replace_text(match: re.Match[str]) -> str:
                    return (
                        match.group(1)
                        + transform_docx_text_for_space_protection(match.group(2), mode)
                        + match.group(3)
                    )

                text = re.sub(r"(<w:t[^>]*>)(.*?)(</w:t>)", replace_text, text, flags=re.S)
                data = text.encode("utf-8")
            dest_archive.writestr(info, data)


def stage_sample_for_conversion(source: Path, input_dir: Path, index: int, docx_space_protection: str) -> tuple[Path, str]:
    if docx_space_protection == "off" or source.suffix.casefold() != ".docx":
        return source, "off"
    staged = input_dir / f"{index:03d}_{source.name}"
    transform_docx_for_space_protection(source, staged, docx_space_protection)
    return staged, docx_space_protection


def extract_pdf_font_names(path: Path) -> list[str]:
    data = path.read_bytes()
    names: list[str] = []
    for pattern in (rb"/(?:BaseFont|FontName)\s*/([^\s<>\[\]()/]+)",):
        for match in re.finditer(pattern, data):
            raw = match.group(1)
            try:
                names.append(raw.decode("ascii"))
            except UnicodeDecodeError:
                names.append(raw.decode("latin-1", errors="replace"))
    return unique_sorted(names)


def extract_ooxml_font_names(path: Path) -> list[str]:
    names: list[str] = []
    with zipfile.ZipFile(path, "r") as archive:
        for member in archive.namelist():
            if not member.endswith(".xml"):
                continue
            if not (
                member.startswith("word/")
                or member.startswith("ppt/")
                or member.startswith("xl/")
            ):
                continue
            text = archive.read(member).decode("utf-8", errors="ignore")
            names.extend(re.findall(r'\btypeface="([^"]+)"', text))
            names.extend(re.findall(r'<w:font\b[^>]*\bw:name="([^"]+)"', text))
            for tag in re.findall(r"<w:rFonts\b[^>]*>", text):
                names.extend(re.findall(r'\bw:(?:ascii|eastAsia|hAnsi|cs)="([^"]+)"', tag))
    return unique_sorted(names)


def extract_ooxml_layout_hints(path: Path) -> dict[str, list[str]]:
    hints: dict[str, list[str]] = {}
    if path.suffix.casefold() != ".docx":
        return hints
    with zipfile.ZipFile(path, "r") as archive:
        for member in ("word/document.xml", "word/settings.xml"):
            try:
                text = archive.read(member).decode("utf-8", errors="ignore")
            except KeyError:
                continue
            doc_grid = re.findall(r"<w:docGrid\b[^>]*/?>", text)
            if doc_grid:
                hints.setdefault("word_doc_grid", []).extend(doc_grid)
    for key, values in hints.items():
        hints[key] = unique_sorted(values)
    return hints


def build_manifest_entry(
    source: Path,
    output: Path,
    root: Path,
    *,
    original_source: Path | None = None,
    docx_space_protection: str = "off",
) -> dict[str, object]:
    return {
        "source": str(source),
        "original_source": str(original_source) if original_source is not None else str(source),
        "source_extension": source.suffix.casefold(),
        "docx_space_protection": docx_space_protection,
        "output_pdf": str(output),
        "output_pdf_size": output.stat().st_size,
        "output_pdf_sha256": sha256_file(output),
        "source_ooxml_fonts": extract_ooxml_font_names(source),
        "source_ooxml_layout_hints": extract_ooxml_layout_hints(source),
        "output_pdf_fonts": extract_pdf_font_names(output),
        "temp_root": str(root),
    }


def write_manifest(root: Path, entries: list[dict[str, object]]) -> Path:
    manifest = {
        "created_at": datetime.now().isoformat(timespec="seconds"),
        "entries": entries,
    }
    path = root / "conversion_manifest.json"
    path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return path


def create_unique_temp_root(repo_root: Path) -> Path:
    tmp_root = repo_root / REPO_LOCAL_RESOURCE_DIR / "tmp"
    tmp_root.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
    pid = os.getpid()
    for attempt in range(100):
        suffix = f"{stamp}_{pid}" if attempt == 0 else f"{stamp}_{pid}_{attempt}"
        path = tmp_root / f"lo_smoke_{suffix}"
        try:
            path.mkdir()
            return path
        except FileExistsError:
            continue
    raise RuntimeError(f"could not create unique LibreOffice smoke-test directory under: {tmp_root}")


def cleanup_image_pycache(soffice: Path) -> None:
    image_root = soffice.parent.parent
    for attempt in range(6):
        for cache_dir in sorted(image_root.rglob("__pycache__"), key=lambda p: len(p.parts), reverse=True):
            shutil.rmtree(cache_dir, ignore_errors=True)
        for pattern in ("*.pyc", "*.pyo"):
            for path in image_root.rglob(pattern):
                try:
                    path.unlink()
                except OSError:
                    pass
        if not any(image_root.rglob("__pycache__")) and not any(image_root.rglob("*.pyc")) and not any(image_root.rglob("*.pyo")):
            return
        time.sleep(0.25 * (attempt + 1))


def write_profile_path_config(profile_dir: Path) -> None:
    local_root = profile_dir.parent / "local"
    work_dir = local_root / "work"
    backup_dir = local_root / "backup"
    temp_dir = local_root / "temp"
    user_dir = profile_dir / "user"
    for path in (work_dir, backup_dir, temp_dir, user_dir):
        path.mkdir(parents=True, exist_ok=True)

    def url(path: Path) -> str:
        return path.resolve().as_uri()

    def single_path(name: str, value: str) -> str:
        return (
            f'  <item oor:path="/org.openoffice.Office.Paths/Paths/{name}">\n'
            '    <prop oor:name="IsSinglePath" oor:op="fuse"><value>true</value></prop>\n'
            f'    <prop oor:name="WritePath" oor:op="fuse"><value>{value}</value></prop>\n'
            f'    <prop oor:name="UserPaths" oor:op="fuse"><value><it>{value}</it></value></prop>\n'
            '  </item>\n'
        )

    def common_path(group: str, work_value: str, backup_value: str) -> str:
        return (
            f'  <item oor:path="/org.openoffice.Office.Common/Path/{group}">\n'
            f'    <prop oor:name="Work" oor:op="fuse"><value>{work_value}</value></prop>\n'
            f'    <prop oor:name="Backup" oor:op="fuse"><value>{backup_value}</value></prop>\n'
            '  </item>\n'
        )

    work_url = url(work_dir)
    backup_url = url(backup_dir)
    data = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<oor:data xmlns:oor="http://openoffice.org/2001/registry" '
        'xmlns:xs="http://www.w3.org/2001/XMLSchema" '
        'xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">\n'
        + single_path("Work", work_url)
        + single_path("Backup", backup_url)
        + single_path("Temp", url(temp_dir))
        + '  <item oor:path="/org.openoffice.Office.Paths/Variables">\n'
        f'    <prop oor:name="Work" oor:op="fuse"><value>{work_url}</value></prop>\n'
        '  </item>\n'
        + common_path("Current", work_url, backup_url)
        + common_path("Default", work_url, backup_url)
        + '</oor:data>\n'
    )
    (user_dir / "registrymodifications.xcu").write_text(data, encoding="utf-8")


def convert_one(
    soffice: Path,
    source: Path,
    out_dir: Path,
    profile_dir: Path,
    timeout: int,
    *,
    isolate_home: bool = True,
    configure_profile_paths: bool = True,
) -> Path:
    if configure_profile_paths:
        write_profile_path_config(profile_dir)
    else:
        profile_dir.mkdir(parents=True, exist_ok=True)
    env = os.environ.copy()
    local_root = profile_dir.parent / "local"
    appdata = local_root / "appdata"
    localappdata = local_root / "localappdata"
    temp_dir = local_root / "temp"
    for path in (appdata, localappdata, temp_dir):
        path.mkdir(parents=True, exist_ok=True)
    if isolate_home:
        env["USERPROFILE"] = str(local_root)
        env["HOME"] = str(local_root)
        env["APPDATA"] = str(appdata)
        env["LOCALAPPDATA"] = str(localappdata)
        env["TEMP"] = str(temp_dir)
        env["TMP"] = str(temp_dir)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    env["PYTHONPYCACHEPREFIX"] = str(profile_dir.parent / "pycache")
    profile_url = "file:///" + str(profile_dir).replace("\\", "/")
    cmd = [
        str(soffice),
        "--headless",
        "--nologo",
        "--nodefault",
        "--nolockcheck",
        "--nofirststartwizard",
        "--norestore",
        f"-env:UserInstallation={profile_url}",
        "--convert-to",
        "pdf",
        "--outdir",
        str(out_dir),
        str(source),
    ]
    completed = subprocess.run(
        cmd,
        cwd=str(out_dir),
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        timeout=timeout,
        check=False,
    )
    if completed.returncode != 0:
        raise RuntimeError(f"LibreOffice failed for {source.name} exit={completed.returncode}\n{completed.stdout}")
    output = out_dir / (source.stem + ".pdf")
    if not output.exists():
        raise RuntimeError(f"LibreOffice did not create expected PDF: {output}\n{completed.stdout}")
    validate_pdf(output)
    cleanup_image_pycache(soffice)
    return output


def collect_input_samples(input_dir: Path, require_docx: bool, require_pptx: bool) -> list[Path]:
    if not input_dir.exists() or not input_dir.is_dir():
        raise RuntimeError(f"input directory not found: {input_dir}")
    samples = sorted(
        path for path in input_dir.rglob("*")
        if path.is_file() and path.suffix.casefold() in {".docx", ".pptx"}
    )
    docx_count = sum(1 for path in samples if path.suffix.casefold() == ".docx")
    pptx_count = sum(1 for path in samples if path.suffix.casefold() == ".pptx")
    if require_docx and docx_count == 0:
        raise RuntimeError(f"no .docx samples found in: {input_dir}")
    if require_pptx and pptx_count == 0:
        raise RuntimeError(f"no .pptx samples found in: {input_dir}")
    if not samples:
        raise RuntimeError(f"no .docx/.pptx samples found in: {input_dir}")
    return samples


def main() -> int:
    args = parse_args()
    repo_root = Path(args.repo_root).resolve()
    soffice = (repo_root / args.soffice).resolve()
    if not soffice.exists():
        raise SystemExit(f"soffice not found: {soffice}")

    cleanup_image_pycache(soffice)
    root = create_unique_temp_root(repo_root)
    input_dir = root / "input"
    out_dir = root / "out"
    profile_docx = root / "profile_docx"
    profile_pptx = root / "profile_pptx"
    for path in (input_dir, out_dir, profile_docx, profile_pptx):
        path.mkdir(parents=True, exist_ok=True)

    try:
        manifest_entries: list[dict[str, object]] = []
        if args.input_dir:
            samples = collect_input_samples((repo_root / args.input_dir).resolve(), args.require_docx, args.require_pptx)
            counts = {"docx": 0, "pptx": 0}
            for index, sample in enumerate(samples):
                suffix = sample.suffix.casefold().lstrip(".")
                counts[suffix] += 1
                profile_dir = root / f"profile_{index}_{suffix}"
                sample_out_dir = out_dir / f"{index:03d}_{suffix}"
                profile_dir.mkdir(parents=True, exist_ok=True)
                sample_out_dir.mkdir(parents=True, exist_ok=True)
                staged_sample, applied_protection = stage_sample_for_conversion(
                    sample, input_dir, index, args.docx_space_protection
                )
                pdf = convert_one(soffice, staged_sample, sample_out_dir, profile_dir, args.timeout)
                manifest_entries.append(
                    build_manifest_entry(
                        staged_sample,
                        pdf,
                        root,
                        original_source=sample,
                        docx_space_protection=applied_protection,
                    )
                )
                print(f"{suffix.upper()} OK: {sample} -> {pdf} ({pdf.stat().st_size} bytes)")
            print(f"Summary: DOCX={counts['docx']} PPTX={counts['pptx']}")
        else:
            docx = input_dir / "smoke_docx.docx"
            pptx = input_dir / "smoke_pptx.pptx"
            create_docx(docx)
            create_pptx(pptx)
            staged_docx, applied_protection = stage_sample_for_conversion(
                docx, input_dir, 0, args.docx_space_protection
            )
            docx_pdf = convert_one(soffice, staged_docx, out_dir, profile_docx, args.timeout)
            pptx_pdf = convert_one(soffice, pptx, out_dir, profile_pptx, args.timeout)
            manifest_entries.append(
                build_manifest_entry(
                    staged_docx,
                    docx_pdf,
                    root,
                    original_source=docx,
                    docx_space_protection=applied_protection,
                )
            )
            manifest_entries.append(build_manifest_entry(pptx, pptx_pdf, root))
            print(f"DOCX OK: {docx_pdf} ({docx_pdf.stat().st_size} bytes)")
            print(f"PPTX OK: {pptx_pdf} ({pptx_pdf.stat().st_size} bytes)")
        manifest_path = write_manifest(root, manifest_entries)
        print(f"Manifest: {manifest_path}")
        return 0
    finally:
        cleanup_image_pycache(soffice)
        if args.keep:
            print(f"Kept: {root}")
        else:
            shutil.rmtree(root, ignore_errors=True)


if __name__ == "__main__":
    raise SystemExit(main())
