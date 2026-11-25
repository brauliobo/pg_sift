#!/usr/bin/env python3
"""The sources of the fuzz targets (CORE in test/fuzz/Makefile) call no function that only a source outside CORE defines.

A function that moves to another file of src/ and is called from a CORE source leaves the fuzz targets unlinkable. The fix is to add the file that now
defines it to CORE in test/fuzz/Makefile (or to give the shims a stand-in), not to leave it out of the targets.

  scripts/check-fuzz-core.py            check the tree
  scripts/check-fuzz-core.py --selftest
"""
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../bench/steps"))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHIMS = ("test/common/pgshim.c", "test/fuzz/c/shim.c", "test/common/cpushim.c")


def listed(path):
    return [l.strip()[:-2] for l in open(path) if l.strip().endswith(".c") and not l.startswith("#")]


def core(makefile):
    m = re.search(r"^CORE\s*=(.*?)^OBJ", open(makefile).read(), re.S | re.M)
    return m.group(1).replace("\\\n", " ").split()


def strip(text):
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return re.sub(r'"(\\.|[^"\\])*"', '""', text)


def definitions(text):
    lines = strip(text).split("\n")
    found = {}
    for i in range(1, len(lines)):
        m = re.match(r"([A-Za-z_]\w*)\(", lines[i])
        if m and lines[i - 1].strip() and not lines[i - 1].rstrip().endswith((";", ",", "{", "}", "\\")):
            found[m.group(1)] = lines[i - 1].lstrip().startswith("static")
    return found


def calls(text):
    return set(re.findall(r"\b([A-Za-z_]\w*)\s*\(", strip(text)))


def check(src, makefile, root):
    members = core(makefile)
    portable = set(listed(os.path.join(src, "portable.list")) + listed(os.path.join(src, "portable-col.list")))
    read = lambda p: open(p).read()
    held = set()
    for c in members:
        held |= set(definitions(read(os.path.join(src, c + ".c"))))
    for s in SHIMS:
        p = os.path.join(root, s)
        if os.path.exists(p):
            held |= set(definitions(read(p)))
    outside = {}
    for p in sorted(portable - set(members)):
        for name, static in definitions(read(os.path.join(src, p + ".c"))).items():
            if not static and name not in held:
                outside.setdefault(name, p)
    bad = []
    for c in members:
        for name in sorted(calls(read(os.path.join(src, c + ".c"))) & set(outside)):
            bad.append(f"{c}.c calls {name}, defined in {outside[name]}.c, which test/fuzz/Makefile does not list in CORE: add {outside[name]} to CORE")
    return bad


def selftest():
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "s", "col"))
        os.makedirs(os.path.join(d, "test", "fuzz"))
        w = lambda p, t: open(os.path.join(d, p), "w").write(t)
        w("s/portable.list", "")
        w("s/portable-col.list", "col/a.c\ncol/b.c\n")
        w("s/col/a.c", "int\nuses(void)\n{\n\treturn helper() + local();\n}\n")
        w("s/col/b.c", "int\nhelper(void)\n{\n\treturn 1;\n}\n\nstatic int\nlocal(void)\n{\n\treturn 2;\n}\n")
        w("test/fuzz/Makefile", "CORE = col/a\nOBJ = x\n")
        bad = check(os.path.join(d, "s"), os.path.join(d, "test/fuzz/Makefile"), d)
        assert len(bad) == 1 and "helper" in bad[0] and "add col/b to CORE" in bad[0], bad
        w("test/fuzz/Makefile", "CORE = col/a col/b\nOBJ = x\n")
        assert check(os.path.join(d, "s"), os.path.join(d, "test/fuzz/Makefile"), d) == []
    print("check-fuzz-core selftest: ok")


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
        sys.exit(0)
    bad = check(os.path.join(ROOT, "src"), os.path.join(ROOT, "test/fuzz/Makefile"), ROOT)
    for b in bad:
        print(b)
    if bad:
        sys.exit(1)
    print("check-fuzz-core: the fuzz targets' sources call no function that only a source outside CORE defines")
