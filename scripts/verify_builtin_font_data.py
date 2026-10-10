"""Compare all built-in font payloads in two ESP32-C3 ELF files.

Addresses may change or become shared, but all metrics, glyphs, bitmaps,
compression groups, interval, kerning and ligature bytes must remain identical.
Uses only the Python standard library; does not execute or flash the firmware.
"""

import argparse
from pathlib import Path
import re
import struct

from build_builtin_fonts import COMMENT_RE, selected_headers


class Elf32:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if self.data[:6] != b"\x7fELF\x01\x01":
            raise ValueError("Expected a little-endian ELF32 file")
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", self.data)
        if header[2] != 243:
            raise ValueError("Expected a RISC-V firmware ELF")
        self.sections = [struct.unpack_from("<IIIIIIIIII", self.data, header[6] + i * header[11]) for i in range(header[12])]
        self.symbols = {}
        for section in self.sections:
            if section[1] != 2:  # SHT_SYMTAB
                continue
            strings = self.sections[section[6]]
            names = self.data[strings[4]:strings[4] + strings[5]]
            for offset in range(section[4], section[4] + section[5], section[9]):
                name, address, size, _, _, _ = struct.unpack_from("<IIIBBH", self.data, offset)
                name = names[name:names.index(b"\x00", name)].decode("utf-8")
                if name and size:
                    self.symbols[name] = address, size

    def symbol(self, name):
        # File-scope static constants use the Itanium C++ ABI local-name form.
        return self.symbols[f"_ZL{len(name)}{name}"]

    def read(self, address, size):
        for section in self.sections:
            _, kind, _, start, offset, length, *_ = section
            if kind != 8 and start and start <= address and address + size <= start + length:
                return self.data[offset + address - start:offset + address - start + size]
        raise ValueError(f"No file-backed section for {address:#x}+{size}")


# EpdFontData's pointer fields in the current 32-bit C3 ABI. Callback fields are
# not masked: changing a callback/context must fail, just like changing metrics.
POINTER_FIELDS = {0: 0, 1: 4, 2: 8, 8: 32, 10: 40, 11: 44, 12: 48, 13: 52,
                  14: 56, 15: 60, 16: 64, 17: 68, 18: 72, 19: 76, 20: 80, 25: 92}


def verify(before_path, after_path, project_dir, slim=False):
    before = Elf32(before_path)
    after = Elf32(after_path)
    directory = Path(project_dir) / "lib/EpdFont/builtinFonts"
    names = selected_headers((directory / "all.h").read_text(encoding="utf-8"), slim)
    checked_bytes = 0
    for filename in names:
        source = COMMENT_RE.sub("", (directory / filename).read_text(encoding="utf-8"))
        match = re.search(r"static const EpdFontData (\w+)\s*=\s*\{(.*?)\};", source, re.DOTALL)
        if not match:
            raise ValueError(f"Missing EpdFontData in {filename}")
        face, initializer = match.groups()
        fields = [field.strip() for field in initializer.split(",") if field.strip()]
        old_address, old_size = before.symbol(face)
        new_address, new_size = after.symbol(face)
        if old_size != 112 or new_size != 112:
            raise ValueError(f"EpdFontData ABI changed for {face}; update verifier offsets")
        old = bytearray(before.read(old_address, old_size))
        new = bytearray(after.read(new_address, new_size))
        for index, offset in POINTER_FIELDS.items():
            name = fields[index]
            old_pointer = struct.unpack_from("<I", old, offset)[0]
            new_pointer = struct.unpack_from("<I", new, offset)[0]
            if name == "nullptr":
                if old_pointer or new_pointer:
                    raise ValueError(f"Unexpected non-null {face} field {index}")
            else:
                array_address, size = before.symbol(name)
                if old_pointer != array_address or not new_pointer:
                    raise ValueError(f"Unexpected pointer for {face}/{name}")
                if before.read(old_pointer, size) != after.read(new_pointer, size):
                    raise ValueError(f"Font payload changed: {face}/{name}")
                checked_bytes += size
            old[offset:offset + 4] = new[offset:offset + 4] = bytes(4)
        if old != new:
            raise ValueError(f"Font metrics or callbacks changed: {face}")
    print(f"Verified {len(names)} faces: {checked_bytes:,} bytes of font payloads and all font metrics are unchanged")
    return len(names), checked_bytes


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--project-dir", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--slim", action="store_true")
    arguments = parser.parse_args()
    verify(arguments.before, arguments.after, arguments.project_dir, arguments.slim)
