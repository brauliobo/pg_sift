#!/usr/bin/env python3
"""Differential test: a direct Python transcription of the tokenization rules
in sift_analyze.h, computed from unicodedata (not from the generated tables),
against `harness dump`. Checks every code point in every folding mode, then
random mixed strings.

    python3 -I tools/analyze/reference.py UCD_DIR ./harness [RANDOM_LINES]
"""
import os
import random
import subprocess
import sys

sys.dont_write_bytecode = True
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gen_tables import SEP, WORD, IDEO, EMOJI, RI, IGN, MARK, tables  # noqa: E402

MAX = 255


def units(b, classes, utf8):
    """(class, source bytes, code point or None) per character or bad byte"""
    out, i = [], 0
    while i < len(b):
        c = b[i]
        if c < 0x80:
            out.append((WORD if chr(c).isascii() and chr(c).isalnum() else SEP, b[i:i + 1], c))
            i += 1
            continue
        if not utf8:
            out.append((WORD, b[i:i + 1], None))
            i += 1
            continue
        n = 2 if 0xC2 <= c <= 0xDF else 3 if 0xE0 <= c <= 0xEF else 4 if 0xF0 <= c <= 0xF4 else 0
        try:
            cp = ord(b[i:i + n].decode("utf-8")) if n else None
        except UnicodeDecodeError:
            cp = None
        if cp is None:
            out.append((SEP, b[i:i + 1], None))
            i += 1
        else:
            out.append((classes[cp], b[i:i + n], cp))
            i += n
    return out


def tokenize(b, mode, classes, fold, utf8=True):
    us, toks, i, off = units(b, classes, utf8), [], 0, [0]
    for u in us:
        off.append(off[-1] + len(u[1]))

    def f(u):
        if u[2] is None:
            return u[1]
        if u[2] < 0x80:
            return chr(u[2]).lower().encode() if mode & 1 else u[1]
        return fold(u[2], mode).encode()

    while i < len(us):
        cls, start, parts = us[i][0], i, []
        if cls in (WORD, MARK):
            while i < len(us) and us[i][0] in (WORD, MARK, IGN):
                if us[i][0] != IGN:
                    parts.append(f(us[i]))
                i += 1
        elif cls in (IDEO, EMOJI, RI):
            parts.append(f(us[i]))
            i += 1
            if cls == RI and i < len(us) and us[i][0] == RI:
                parts.append(f(us[i]))
                i += 1
            while i < len(us) and us[i][0] in (MARK, IGN):
                if cls == IDEO and us[i][0] == MARK:
                    parts.append(f(us[i]))
                i += 1
        else:
            i += 1
            continue
        tok = b""
        for p in parts:
            if len(tok) + len(p) > MAX:
                break
            tok += p
        if tok:
            toks.append(f"{tok.hex()}:{off[start]}:{off[i]}")
    return " ".join(toks)


def run(harness, mode, lines, enc="utf8"):
    inp = "".join(line.hex() + "\n" for line in lines)
    out = subprocess.run([harness, "dump", str(mode), "0", enc], input=inp, capture_output=True,
                         text=True, check=True).stdout
    return out.split("\n")[:-1]


def main(ucd, harness, nrandom=3000):
    classes, fold = tables(ucd)
    every = [("x%cx %c" % (cp, cp)).encode("utf-8", "surrogatepass") for cp in range(1, 0x110000)]
    rnd = random.Random(7)
    pools = [(0x20, 0x7E), (0xC0, 0x24F), (0x300, 0x36F), (0x370, 0x52F), (0x590, 0x6FF),
             (0x900, 0x97F), (0xE00, 0xE7F), (0x3040, 0x30FF), (0x4E00, 0x4F00), (0xAC00, 0xAD00),
             (0x1F1E6, 0x1F1FF), (0x1F300, 0x1FAFF), (0x200B, 0x200F), (0xFE00, 0xFE0F),
             (0xFF00, 0xFFEF), (0x1D400, 0x1D7FF), (0xE0020, 0xE007F), (1, 0x10FFFF)]
    mixed = []
    for _ in range(nrandom):
        s = []
        for _ in range(rnd.randrange(1, 120)):
            lo, hi = rnd.choice(pools)
            s.append(chr(rnd.randint(lo, hi)) * (rnd.randrange(1, 300) if rnd.random() < 0.02 else 1))
        b = "".join(s).encode("utf-8", "surrogatepass")
        if rnd.random() < 0.2:
            k = rnd.randrange(len(b) + 1)
            b = b[:k] + bytes([rnd.randrange(0x80, 0x100)]) + b[k:]
        mixed.append(b)
    bad = 0
    for mode in range(4):
        for name, lines, enc in (("every", every, "utf8"), ("mixed", mixed, "utf8"),
                                 ("sql_ascii", mixed[:500], "sql_ascii")):
            got = run(harness, mode, lines, enc)
            for line, g in zip(lines, got):
                want = tokenize(line, mode, classes, fold, enc == "utf8")
                if g != want:
                    bad += 1
                    if bad <= 10:
                        print(f"mode {mode} {name}: {line!r}\n  got  {g}\n  want {want}")
            print(f"reference: mode {mode} {name}: {len(lines)} lines checked")
    print(f"reference: {bad} mismatches")
    sys.exit(bad != 0)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 3000)
