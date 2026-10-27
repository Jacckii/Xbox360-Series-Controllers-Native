#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fails the build when an XEX would misbehave once the kernel loads it.

- The image must cover the whole PE. SynthXEX sizes the image from the bytes the PE
  file holds, so a section whose virtual size runs past its raw data (zero-initialised
  .bss) ends up partly outside the loaded image: writes there land in memory the
  module doesn't own.
- With --no-fp (system DLLs): no floating-point instructions. DLL code runs on the
  kernel's DPCs, which have no FP context, and an FP instruction there faults.

  check_xex.py [--no-fp OBJDUMP] app.exe app.xex
"""
import re
import struct
import subprocess
import sys

FP = re.compile(r"\s(l|st)f[sd](u|x|ux)?\s|\sf[a-z]+\.?\s|\sm[ft]fs")


def pe_size_of_image(path):
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    return struct.unpack_from("<I", d, pe + 24 + 56)[0]


def xex_image_size(path):
    d = open(path, "rb").read()
    if d[:4] != b"XEX2":
        sys.exit(f"{path}: not an XEX2 file")
    security_offset = struct.unpack_from(">I", d, 0x10)[0]
    return struct.unpack_from(">I", d, security_offset + 4)[0]


def fp_instructions(objdump, exe):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", exe], capture_output=True,
                         text=True, check=True).stdout
    hits = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+(.*)", line)  # instruction lines only
        if m and FP.search(" " + m.group(2) + " "):
            hits.append(f"{m.group(1)}: {m.group(2).strip()}")
    return hits


def main():
    args = sys.argv[1:]
    objdump = None
    if args[:1] == ["--no-fp"]:
        objdump, args = args[1], args[2:]
    exe, xex = args
    need, have = pe_size_of_image(exe), xex_image_size(xex)
    if have < need:
        sys.exit(f"{xex}: maps {have:#x} bytes but {exe} needs {need:#x} "
                 f"({need - have:#x} unmapped, likely .bss)")
    if objdump:
        fp = fp_instructions(objdump, exe)
        if fp:
            sys.exit(f"{exe}: {len(fp)} floating-point instructions, e.g. {fp[0]}")
    print(f"{xex}: image {have:#x} bytes, ok" + (", no FP" if objdump else ""))


main()
