import argparse
import pathlib
import re
import sys
import zlib


def _extract_obj(pdf: bytes, obj_num: int) -> bytes | None:
    # Very small, naive parser: enough for PDFs we generate (non-incremental xref not required).
    m = re.search(rb"[\r\n]%d\s+0\s+obj\b" % obj_num, pdf)
    if not m:
        return None
    start = m.start()
    end = pdf.find(b"endobj", start)
    if end < 0:
        return None
    return pdf[start : end + len(b"endobj")]


def _extract_stream(obj_bytes: bytes) -> tuple[bytes, bytes] | None:
    si = obj_bytes.find(b"stream")
    if si < 0:
        return None
    after = obj_bytes.find(b"\n", si)
    if after < 0:
        return None
    data_start = after + 1
    ei = obj_bytes.find(b"endstream", data_start)
    if ei < 0:
        return None
    data = obj_bytes[data_start:ei]
    if data.endswith(b"\r"):
        data = data[:-1]
    header = obj_bytes[:si]
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

    pdf = args.pdf.read_bytes()
    refs = sorted({int(m.group(1)) for m in re.finditer(rb"/SMask\s+(\d+)\s+0\s+R\b", pdf)})
    if not refs:
        print("OK: no /SMask references found.")
        return 0

    bad: list[int] = []
    for obj_num in refs:
        obj = _extract_obj(pdf, obj_num)
        if not obj:
            continue
        stream = _extract_stream(obj)
        if not stream:
            continue
        data, header = stream
        if b"/FlateDecode" not in header:
            continue
        try:
            dec = zlib.decompress(data)
        except Exception:
            continue
        if dec and _is_all_zero(dec):
            bad.append(obj_num)

    if bad:
        print("NG: fully-zero /SMask stream(s) detected:", ", ".join(map(str, bad)))
        return 2

    print("OK: /SMask streams look non-zero.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

