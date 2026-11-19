#!/usr/bin/env python3
"""Prepare the decomp's C sources for the native port.

For every source file of the decomp that the port compiles, write a copy under OUT/ that clang can build. The sources
themselves are written for the port (CodeWarrior assembly and functions the platform layer provides are inside
`#ifndef PLATFORM_PORT`, register writes with side effects use PORT_IO_WRITE); here:
  - writes to side-effect registers that do not go through PORT_IO_WRITE stop the build;
  - files of an overlay get `#pragma clang section` so that the overlay's .data/.bss can be restored to their initial
    state whenever the game loads the overlay again, as the DS does by reading it from the ROM.

Also writes OUT/sources.txt (one generated path per line) and OUT/overlays.txt.
Files are only rewritten when their content changes, to keep incremental builds incremental.

Usage: portify.py <decomp> <out> <exclude-list> <version>
"""
import os
import re
import sys
from pathlib import Path

# Registers whose writes have side effects must go through the platform layer: PORT_IO_WRITE(reg_X, v), which
# forwards them to the geometry engine, the VRAM mapper or the interrupt controller. A plain `reg_X = v;` (or |=,
# &=, ^=) in a compiled file stops the build: write it as PORT_IO_WRITE in the source.
SIDE_EFFECT_REG = re.compile(r"\b(reg_(?:G3_[A-Z0-9_x]+|G3X_GXFIFO|GX_VRAMCNT_[A-I]|OS_IF))\s*([|&^]?)=(?!=)")


def check_reg_writes(text, rel):
    m = SIDE_EFFECT_REG.search(text)
    if m:
        line = text.count("\n", 0, m.start()) + 1
        sys.exit(f"portify: {rel}:{line}: write to {m.group(1)} must be PORT_IO_WRITE({m.group(1)}, ...)")


# ------------------------------------------------------------------ DS address relocation
# The DS regions live in one block of memory, the "DS window" (port/platform/include/ds_addr.h): DS addresses in the
# sources become DSA(address). An address is recognised by its context: a pointer cast in front of it, a HW_ define,
# or an entry of RELOCATE_LITERALS (integer-typed addresses). Constants that only look like addresses (bit masks,
# fixed-point values) are left alone.
DS_REGIONS = [
    (0x02000000, 0x02400000), (0x027e0000, 0x02800000), (0x01ff8000, 0x02000000), (0x04000000, 0x04010000),
    (0x04100000, 0x04104000), (0x05000000, 0x05000800), (0x07000000, 0x07000800), (0x08000000, 0x08010000),
    (0x06000000, 0x06080000), (0x06200000, 0x06220000), (0x06400000, 0x06440000), (0x06600000, 0x06620000),
    (0x067e0000, 0x068a4000),
]
RELOCATE_LITERALS = {
    "libs/NitroSDK/gx/src/bg_screen_char_pointers.c": {0x6000000, 0x6200000},
    "libs/NitroSDK/gx/src/vram_load_2d_graphics.c": {0x6400000, 0x6600000, 0x6880000, 0x6890000, 0x6894000, 0x6898000,
                                                    0x68a0000},
    "src/03/Graphics/text_canvas.c": {0x6400000, 0x6600000},
    "libs/NitroSDK/fs/src/overlay_rom_loader.c": {0x27ffe40, 0x27ffe48},
    "libs/NitroSDK/os/include/interrupt_control.h": {0x027e0000},
    "libs/NitroSDK/os/include/reset_interrupts_halt.h": {0x027ffc00},
    "libs/NitroSDK/mb/include/multiboot_file_register.h": {0x027ffe00},
}
NOT_ADDRESSES = {0x2040000}  # TCP MSS option (src/00/Lib/CPS/tcp_ip_stack.c)
HEX = r"0x[0-9a-fA-F]+[uUlL]*\b"
CAST_ADDR = re.compile(r"(\(\s*(?:const\s+|volatile\s+)*\w+(?:\s+(?:const|volatile))*\s*\*+\s*\)\s*\(?\s*)(" + HEX + ")")
DEFINE_HW = re.compile(r"(#define\s+HW_\w+\s+)(" + HEX + ")")
LITERAL = re.compile(r"(?<![\w.])(" + HEX + ")")


def is_ds_address(lit):
    v = int(lit.rstrip("uUlL"), 16)
    return any(lo <= v < hi for lo, hi in DS_REGIONS)


def relocate(text, rel):
    def sub_cast(m):
        return m.group(1) + (f"DSA({m.group(2)})" if is_ds_address(m.group(2)) else m.group(2))
    text = CAST_ADDR.sub(sub_cast, text)
    text = DEFINE_HW.sub(sub_cast, text)
    # The decompiler's expansion of the SDK's register macros: addresses held in u32 variables, written with a
    # U suffix (0x4001008u). Bit masks with a suffix in the same ranges are listed in NOT_ADDRESSES.
    def sub_suffixed(m):
        lit = m.group(1)
        if lit[-1] in "uU" and is_ds_address(lit) and int(lit.rstrip("uUlL"), 16) not in NOT_ADDRESSES \
                and text[max(0, m.start() - 4):m.start()] != "DSA(":
            return f"DSA({lit})"
        return lit
    text = LITERAL.sub(sub_suffixed, text)
    wanted = RELOCATE_LITERALS.get(rel)
    if wanted:
        def sub_lit(m):
            v = int(m.group(1).rstrip("uUlL"), 16)
            if v in wanted and text[max(0, m.start() - 4):m.start()] != "DSA(":
                return f"DSA({m.group(1)})"
            return m.group(1)
        text = LITERAL.sub(sub_lit, text)
    return text


def module_units(decomp, version):
    """Map source path -> overlay name ('ov000' ...) for overlay units."""
    units = {}
    ovdir = Path(decomp) / "config" / version / "arm9" / "overlays"
    for ov in sorted(ovdir.iterdir()):
        for line in open(ov / "delinks.txt"):
            line = line.rstrip()
            if line and not line.startswith((" ", "\t", "/")) and line.endswith(":"):
                units[line[:-1]] = ov.name
    return units


def build_units(decomp, version):
    """Every source the real build links: the units of delinks.txt (main, ITCM, DTCM, overlays)."""
    units = []
    cfg = Path(decomp) / "config" / version / "arm9"
    for d in [cfg / "delinks.txt", cfg / "itcm" / "delinks.txt", cfg / "dtcm" / "delinks.txt"] + sorted(
            (cfg / "overlays").glob("*/delinks.txt")):
        for line in open(d):
            line = line.rstrip()
            if line and not line.startswith((" ", "\t", "/")) and line.endswith(":"):
                units.append(line[:-1])
    return units


def write_if_changed(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        if path.read_text() == text:
            return
    except OSError:
        pass
    path.write_text(text)


def main():
    decomp, out, exclude_file, version = sys.argv[1:5]
    decomp = Path(decomp)
    out = Path(out)
    excludes = []
    for line in open(exclude_file):
        line = line.split("#", 1)[0].strip()
        if line:
            excludes.append(line)

    def excluded(rel):
        return any(rel == e or (e.endswith("/") and rel.startswith(e)) for e in excludes)

    overlays = module_units(decomp, version)

    sources, used_overlays = [], set()
    for rel in sorted(set(build_units(decomp, version))):
        src = decomp / rel
        if not rel.endswith(".c") or excluded(rel):
            continue
        text = src.read_text(encoding="utf-8", errors="surrogateescape")
        check_reg_writes(text, rel)
        text = relocate(text, rel)
        ov = overlays.get(rel)
        header = f'#line 1 "{rel}"\n'
        dst = out / "src" / rel
        write_if_changed(dst, header + text)
        sources.append(dst.as_posix())

    # Headers: one tree with the decomp's include/ and the port's shadow headers on top, so that a quoted include
    # resolves to the same file from everywhere (CodeWarrior's -gccinc semantics).
    inc = out / "include"
    compat = Path(__file__).resolve().parent.parent / "port" / "compat" / "include"
    shadowed = {p.relative_to(compat).as_posix() for p in compat.rglob("*.h")}
    headers = [(h, h.relative_to(decomp / "include").as_posix()) for h in sorted((decomp / "include").rglob("*.h"))]
    for d in sorted(decomp.glob("libs/**/include")):
        # The C library's headers (stdio.h, string.h ...) go to their own folder, used only for the game's code, so
        # that the platform layer gets the host's.
        prefix = "../msl_include/" if d.parts[-2] == "msl" else ""
        headers += [(h, prefix + h.relative_to(d).as_posix()) for h in sorted(d.rglob("*.h"))]
    for h, rel in headers:
        text = h.read_text(encoding="utf-8", errors="surrogateescape")
        src_rel = h.relative_to(decomp).as_posix()
        text = relocate(text, src_rel)
        if rel == "types.h":
            # mwcc aligns 64-bit integers to 4 bytes (u64 members sit at offsets like 0x2c); keep that layout.
            for t in ("u64", "s64", "vu64", "vs64"):
                text, n = re.subn(rf"^typedef (.*) {t};$", rf"typedef \1 __attribute__((aligned(4))) {t};", text, flags=re.M)
                if n != 1:
                    sys.exit(f"portify: types.h: typedef of {t} not found")
        if rel in shadowed:
            rel += ".decomp"
        write_if_changed(inc / rel, text)
    for h in sorted(compat.rglob("*.h")):
        rel = h.relative_to(compat).as_posix()
        text = re.sub(r'#include_next "([^"]+)"', r'#include "\1.decomp"', h.read_text())
        write_if_changed(inc / rel, text)

    write_if_changed(out / "sources.txt", "\n".join(sources) + "\n")
    write_if_changed(out / "overlays.txt", "\n".join(sorted(used_overlays)) + "\n")
    write_if_changed(out / "overlay_units.txt",
                     "".join(f"{overlays[r]} {r}\n" for r in sorted(overlays) if (out / "src" / r).exists()))


if __name__ == "__main__":
    main()
