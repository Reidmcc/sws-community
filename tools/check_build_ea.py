"""Check that mod/build_ea.h covers every game address the mod's source names (no game files needed; CI runs it).

An address added to the source without regenerating the EA table would give 0 in the EA app's build. This lists any,
and fails. Regenerate with tools/gen_build_ea.py (needs the EA image in work/ea).

Usage: python tools/check_build_ea.py
"""
import os
import re
import sys

PROJECT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = [os.path.join(PROJECT, "mod", "sws_probe.cpp"), os.path.join(PROJECT, "mod", "config_types.h")]
ADDR = re.compile(r"\b0x(14[0-9a-fA-F]{7})(?:ull|ULL)?\b")


def code_only(text):
    """The source without comments and string literals (addresses there are notes, not uses)."""
    out = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    out = re.sub(r"/\*.*?\*/", " ", out, flags=re.S)
    return re.sub(r"//[^\n]*", "", out)


def main():
    used = set()
    for f in SOURCES:
        used |= {int(m.group(1), 16) for m in ADDR.finditer(code_only(open(f, encoding="utf-8").read()))}
    table = open(os.path.join(PROJECT, "mod", "build_ea.h"), encoding="utf-8").read()
    have = {int(a, 16) for a in re.findall(r"\{ 0x([0-9a-f]+)ull, 0x[0-9a-f]+ull \}", table)}
    missing = sorted(used - have)
    for a in missing: print("missing from build_ea.h: 0x%x" % a)
    print("%d addresses in the source, %d in the EA table, %d missing" % (len(used), len(have), len(missing)))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
