"""Small on-disk PE fixtures for import inspection, never executable programs."""

import struct


def make_pe(imported_dll: str | None = None, *, symbol: str | None = "ImportedFunction", pe64: bool = True) -> bytes:
    data = bytearray(1024)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<H", data, 0x86, 1)
    optional = 0x98
    optional_size = 240 if pe64 else 224
    directory_offset = 112 if pe64 else 96
    struct.pack_into("<H", data, 0x94, optional_size)
    struct.pack_into("<H", data, optional, 0x20B if pe64 else 0x10B)
    struct.pack_into("<I", data, optional + 60, 512)
    struct.pack_into("<I", data, optional + directory_offset - 4, 16)
    struct.pack_into("<IIII", data, optional + optional_size + 8, 512, 0x1000, 512, 512)
    if imported_dll is not None:
        struct.pack_into("<II", data, optional + directory_offset + 8, 0x1000, 40)
        struct.pack_into("<IIIII", data, 512, 0x10A0, 0, 0, 0x1080, 0x10A0)
        dll = imported_dll.encode("ascii") + b"\0"
        if len(dll) > 32:
            raise ValueError("DLL name is too long for this fixture")
        data[640:640 + len(dll)] = dll
        thunk = 0x10C0 if symbol is not None else ((1 << (63 if pe64 else 31)) | 1)
        struct.pack_into("<Q" if pe64 else "<I", data, 672, thunk)
        if symbol is not None:
            name = symbol.encode("ascii") + b"\0"
            if len(name) > 62:
                raise ValueError("symbol name is too long for this fixture")
            data[706:706 + len(name)] = name
    return bytes(data)
