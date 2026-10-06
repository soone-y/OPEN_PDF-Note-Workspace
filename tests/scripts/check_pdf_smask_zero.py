import argparse
import pathlib
import re
import sys
import zlib


def _extract_obj(pdf: bytes, obj_num: int, generation: int) -> bytes | None:
    # A deliberately limited parser for generated PDFs. Unsupported or
    # ambiguous structures are inspection failures, never evidence of OK.
    matches = list(re.finditer(rb"(?:^|[\r\n])%d\s+%d\s+obj\b" % (obj_num, generation), pdf))
    if len(matches) != 1:
        return None
    start = matches[0].start()
    end = pdf.find(b"endobj", start)
    if end < 0:
        return None
    return pdf[start : end + len(b"endobj")]


def _extract_stream(obj_bytes: bytes) -> tuple[bytes, bytes] | None:
    marker = re.search(rb"\bstream(?:\r\n|\n|\r)", obj_bytes)
    if marker is None:
        return None
    header = obj_bytes[:marker.start()]
    if re.search(rb"/Length\s+\d+\s+\d+\s+R\b", header):
        # An indirect length needs a real PDF resolver.
        return None
    length = re.search(rb"/Length\s+(\d+)\b", header)
    if length is None:
        return None
    data_start = marker.end()
    data_end = data_start + int(length.group(1))
    if data_end > len(obj_bytes) or not obj_bytes[data_end:].lstrip(b"\r\n \t").startswith(b"endstream"):
        return None
    data = obj_bytes[data_start:data_end]
    return data, header


def _is_all_zero(data: bytes) -> bool:
    # Fast path without scanning everything into a set.
    return all(b == 0 for b in data)


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(
        description="Detect fully-transparent /SMask streams (often causes 'blank white' PDFs)."
    )
    ap.add_argument("pdf", type=pathlib.Path)
    args = ap.parse_args(argv)

    try:
        pdf = args.pdf.read_bytes()
    except OSError as error:
        print(f"SMask inspection could not read PDF: {error}", file=sys.stderr)
        return 1
    if not pdf.startswith(b"%PDF-"):
        print("SMask inspection requires a PDF signature.", file=sys.stderr)
        return 1
    refs = sorted({(int(m.group(1)), int(m.group(2)))
                   for m in re.finditer(rb"/SMask\s+(\d+)\s+(\d+)\s+R\b", pdf)})
    if re.search(rb"/SMask\b(?!\s+(?:\d+\s+\d+\s+R\b|/None\b))", pdf):
        print("SMask inspection found an unsupported mask reference.", file=sys.stderr)
        return 1
    if not refs:
        print("OK: no /SMask references found.")
        return 0

    bad: list[str] = []
    unreadable: list[str] = []
    for obj_num, generation in refs:
        label = f"{obj_num} {generation}"
        obj = _extract_obj(pdf, obj_num, generation)
        if not obj:
            unreadable.append(f"{label}: object missing, incomplete, or ambiguous")
            continue
        stream = _extract_stream(obj)
        if not stream:
            unreadable.append(f"{label}: stream or direct length could not be parsed")
            continue
        data, header = stream
        if b"/DecodeParms" in header:
            unreadable.append(f"{label}: unsupported decode parameters")
            continue
        filter_match = re.search(rb"/Filter\s*(/FlateDecode\b|\[\s*/FlateDecode\s*\])", header)
        if b"/Filter" in header and filter_match is None:
            unreadable.append(f"{label}: unsupported stream filter")
            continue
        try:
            if filter_match:
                decoder = zlib.decompressobj()
                dec = decoder.decompress(data) + decoder.flush()
                if not decoder.eof or decoder.unused_data:
                    raise ValueError("incomplete or trailing compressed data")
            else:
                dec = data
            if not dec:
                raise ValueError("empty mask stream")
        except (zlib.error, ValueError) as error:
            unreadable.append(f"{label}: {error}")
            continue
        if _is_all_zero(dec):
            bad.append(label)

    if unreadable:
        print("SMask inspection could not complete:", file=sys.stderr)
        for detail in unreadable:
            print(f"- {detail}", file=sys.stderr)
        if bad:
            print("NG: fully-zero /SMask stream(s) detected:", ", ".join(bad))
        return 1

    if bad:
        print("NG: fully-zero /SMask stream(s) detected:", ", ".join(bad))
        return 2

    print("OK: /SMask streams look non-zero.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
