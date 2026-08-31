#!/usr/bin/env python3
"""Dump the export names of a 64-bit Windows DLL, one per line, sorted.

Used to (re)generate third_party/ndi/Processing.NDI.Lib.Advanced.x64.def:
the export list feeds a stub import library so CI can link the plugin
without the access-gated NDI Advanced SDK (see CMakeLists.txt).

Usage: python scripts/dump_ndi_exports.py <path-to-dll>
"""

import struct
import sys


def main():
    path = sys.argv[1]
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", "not a PE file"
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt_off = pe + 24
    magic = struct.unpack_from("<H", data, opt_off)[0]
    assert magic == 0x20B, "not PE32+ (need a 64-bit DLL)"
    exp_rva = struct.unpack_from("<I", data, opt_off + 112)[0]

    secs = []
    sec_off = opt_off + struct.unpack_from("<H", data, pe + 20)[0]
    for i in range(nsec):
        o = sec_off + i * 40
        vsize, vaddr, rsize, raw = struct.unpack_from("<IIII", data, o + 8)
        secs.append((vaddr, vsize, raw, rsize))

    def rva2off(rva):
        for vaddr, vsize, raw, rsize in secs:
            if vaddr <= rva < vaddr + max(vsize, rsize):
                return raw + (rva - vaddr)
        raise ValueError(hex(rva))

    eo = rva2off(exp_rva)
    nnames = struct.unpack_from("<I", data, eo + 24)[0]
    names_rva = struct.unpack_from("<I", data, eo + 32)[0]
    no = rva2off(names_rva)
    out = []
    for i in range(nnames):
        nrva = struct.unpack_from("<I", data, no + i * 4)[0]
        s = rva2off(nrva)
        e = data.index(b"\0", s)
        out.append(data[s:e].decode())
    print("\n".join(sorted(out)))


if __name__ == "__main__":
    main()
