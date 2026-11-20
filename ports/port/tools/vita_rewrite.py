"""Mirror the decompilation for the Vita, with its main RAM addresses moved to the P1 mirror.

usage: vita_rewrite.py DECOMP OUT

The Vita gives a program no memory at 0x0C000000, but does at 0x8C000000 and (as an alias of the same pages)
0xAC000000: the Dreamcast's own P1 and P2 mirrors of main RAM. The game's C names its buffers at 0x0C...
and 0x0D... (the decomp's form of them), so the mirror's copies of src/ and include/ say 0x8C... and 0x8D...
instead: the same byte of RAM. Every literal of that form in the game is an address (none is a colour or a
mask). The tables gen_host.py reads are linked, not copied. A file is written only when it changes, so a
rerun rebuilds nothing that stayed the same.
"""
import os
import re
import sys
from pathlib import Path

LITERAL = re.compile(r"\b0([xX])0([cCdD])([0-9a-fA-F]{6})\b")
TABLES = ("symbols.txt", "symbols_dc.txt", "mvc2.sub", "mvc2dc.sub", "pointers.tsv", "pointers_dc.tsv")


def rewrite(text):
    count = 0

    def move(m):
        nonlocal count
        count += 1
        return "0" + m[1] + "8" + m[2] + m[3]

    return LITERAL.sub(move, text), count


def put(path, data):
    if path.is_file() and path.read_bytes() == data:
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)


def main():
    decomp, out = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve()
    total = files = 0
    for top in ("src", "include"):
        for src in sorted((decomp / top).rglob("*")):
            if not src.is_file():
                continue
            dst = out / src.relative_to(decomp)
            data = src.read_bytes()
            if src.suffix in (".c", ".h", ".inc"):
                text, n = rewrite(data.decode("latin-1"))
                data = text.encode("latin-1")
                total += n
                files += n > 0
            put(dst, data)
    for name in TABLES:
        for where in (decomp / name, decomp / "build" / name):
            if where.is_file():
                link = out / name
                if not link.is_file() or link.resolve() != where:
                    if link.exists() or link.is_symlink():
                        link.unlink()
                    os.symlink(where, link)
                break
    print(f"vita_rewrite: {total} RAM addresses in {files} files moved to the P1 mirror")


if __name__ == "__main__":
    main()
