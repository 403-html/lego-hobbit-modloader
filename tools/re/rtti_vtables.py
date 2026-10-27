#!/usr/bin/env python3
"""List every class with a vtable in a 32-bit MSVC executable, using RTTI.

    pip install pefile
    python tools/re/rtti_vtables.py "C:/.../LEGOHobbit.exe" > vtables.txt
    python tools/re/rtti_vtables.py LEGOHobbit.exe --json vtables.json

Output (text): one line per vtable
    <vtable RVA>  <methods>  <this offset>  <class>
RVAs are relative to the image base (0x400000 in Ghidra/IDA by default). The
same resolution runs inside the loader as HmlApi::find_vtable.

Layout used (MSVC x86):
    vtable[-1] -> CompleteObjectLocator {sig=0, offset, cdOffset, TypeDescriptor*, ...}
    TypeDescriptor {vftable*, spare, ".?AVName@@"}
"""
import argparse
import json
import re
import struct
import sys

import pefile


def scan(path):
    pe = pefile.PE(path, fast_load=True)
    if pe.FILE_HEADER.Machine != 0x14C:
        sys.exit("only 32-bit (i386) executables are supported")
    base = pe.OPTIONAL_HEADER.ImageBase
    img = pe.get_memory_mapped_image()
    secs = {s.Name.rstrip(b"\0").decode(): (s.VirtualAddress, s.VirtualAddress + s.Misc_VirtualSize)
            for s in pe.sections}
    rdata, data, text = secs[".rdata"], secs[".data"], secs[".text"]

    def u32(rva):
        return struct.unpack_from("<I", img, rva)[0]

    def in_text(va):
        return text[0] <= va - base < text[1]

    tds = {}
    for m in re.finditer(rb"\.\?A[VU][^\x00]{1,400}?@@\x00", img[data[0]:data[1]]):
        tds[base + data[0] + m.start() - 8] = m.group(0)[:-1].decode("latin-1")

    cols = {}
    for off in range(rdata[0], rdata[1] - 20, 4):
        if u32(off) == 0 and u32(off + 12) in tds:
            cols[base + off] = (tds[u32(off + 12)], u32(off + 4))

    rows = []
    for off in range(rdata[0], rdata[1] - 8, 4):
        col = u32(off)
        if col not in cols or not in_text(u32(off + 4)):
            continue
        name, this_off = cols[col]
        n = 0
        while off + 4 + 4 * n < rdata[1] and in_text(u32(off + 4 + 4 * n)):
            n += 1
            if base + off + 4 * n in cols:  # next vtable's locator starts here
                break
        pretty = name[4:-2] if name.endswith("@@") else name[4:]
        rows.append({"class": pretty, "mangled": name, "vtable_rva": off + 4,
                     "this_offset": this_off, "methods": n})
    rows.sort(key=lambda r: (r["class"], r["this_offset"]))
    return rows, len(tds)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("exe")
    ap.add_argument("--json", help="write JSON to this file instead of text to stdout")
    args = ap.parse_args()
    rows, ntd = scan(args.exe)
    print(f"{ntd} RTTI type names, {len(rows)} vtables", file=sys.stderr)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rows, f, indent=1)
    else:
        for r in rows:
            print(f"{r['vtable_rva']:08X}  {r['methods']:4d}  {r['this_offset']:4d}  {r['class']}")


if __name__ == "__main__":
    main()
