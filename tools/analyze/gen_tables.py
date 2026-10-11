#!/usr/bin/env python3
"""Generate src/sift_analyze_tables.h: code point classes and folding maps.

    python3 -I tools/analyze/gen_tables.py UCD_DIR > src/sift_analyze_tables.h

UCD_DIR holds the Unicode 16.0.0 files Scripts.txt, DerivedCoreProperties.txt
and emoji/emoji-data.txt; Python's unicodedata must be the same version. The
tables are frozen in the repository so analysis never changes with the
PostgreSQL or system Unicode version (it is part of the on-disk format).
"""
import sys
import unicodedata as ud

UNICODE = "16.0.0"
SEP, WORD, IDEO, EMOJI, RI, IGN, MARK = range(7)
NCP = 0x110000

# Combining marks treated as accents: Latin/Greek/Cyrillic diacritics, Hebrew
# points, Arabic harakat, symbol marks. Indic vowel signs, viramas, kana voicing
# marks etc. carry meaning and are kept.
STRIP_BLOCKS = [(0x0300, 0x036F), (0x0483, 0x0489), (0x0591, 0x05C7), (0x0610, 0x061A),
                (0x064B, 0x065F), (0x0670, 0x0670), (0x06D6, 0x06ED), (0x0898, 0x089F),
                (0x08CA, 0x08FF), (0x1AB0, 0x1AFF), (0x1DC0, 0x1DFF), (0x20D0, 0x20FF),
                (0xFE20, 0xFE2F)]
# Latin letters without a canonical decomposition, folded like their base letter.
EXTRA = dict(zip("ØøŁłĐđĦħŦŧƗɨƵƶɃƀÐðıĿŀǤǥȻȼɆɇɈɉɌɍɎɏȽƚɫƉɖ",
                 "OoLlDdHhTtIiZzBbDdiLlGgCcEeJjRrYyLllDd"))
EXTRA.update({"Æ": "AE", "æ": "ae", "Œ": "OE", "œ": "oe", "Þ": "TH", "þ": "th"})
APOSTROPHES = {0x02BC}		# modifier letter apostrophe separates like U+0027


def prop_set(path, name):
    out = set()
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        cps, prop = (x.strip() for x in line.split(";")[:2])
        if prop == name:
            lo, _, hi = cps.partition("..")
            out.update(range(int(lo, 16), int(hi or lo, 16) + 1))
    return out


def tables(ucd):
    """Returns (classes, fold): the class of every code point and
    fold(cp, mode) for modes 1 case, 2 accent, 3 both."""
    assert ud.unidata_version == UNICODE, ud.unidata_version
    scripts = f"{ucd}/Scripts.txt"
    ideo = prop_set(scripts, "Han") | prop_set(scripts, "Hiragana")
    cyrillic = prop_set(scripts, "Cyrillic")
    ignorable = prop_set(f"{ucd}/DerivedCoreProperties.txt", "Default_Ignorable_Code_Point")
    pict = prop_set(f"{ucd}/emoji/emoji-data.txt", "Extended_Pictographic")
    strip = {cp for lo, hi in STRIP_BLOCKS for cp in range(lo, hi + 1)
             if ud.category(chr(cp)) in ("Mn", "Me")}

    def cls(cp):
        if cp in APOSTROPHES or cp == 0x200B:
            return SEP
        if cp in ignorable or 0x1F3FB <= cp <= 0x1F3FF:
            return IGN
        if 0x1F1E6 <= cp <= 0x1F1FF:
            return RI
        if cp in pict and cp >= 0x2190:
            return EMOJI
        cat = ud.category(chr(cp))[0]
        if cat == "M":
            return MARK
        if cat in "LN":
            return IDEO if cp in ideo else WORD
        return SEP

    classes = [cls(cp) for cp in range(NCP)]
    folds = lambda cp: classes[cp] in (WORD, IDEO, MARK)

    def accent(ch):
        base = EXTRA.get(ch, ch)
        drop = strip - {0x0306} if ord(ch) in cyrillic else strip
        r = ud.normalize("NFC", "".join(c for c in ud.normalize("NFKD", base) if ord(c) not in drop))
        return r if all(folds(ord(c)) for c in r) else ch

    case = str.casefold
    steps = {1: [case], 2: [accent], 3: [accent, case, accent]}

    def fixpoint(cp, fns):
        s = chr(cp)
        for _ in range(8):
            t = s
            for fn in fns:
                t = "".join(fn(c) if folds(ord(c)) else c for c in t)
            if t == s:
                return s
            s = t
        raise SystemExit(f"no fixpoint for U+{cp:04X}")

    def fold(cp, mode):
        return fixpoint(cp, steps[mode]) if mode and folds(cp) else chr(cp)

    return classes, fold


def main(ucd):
    classes, fold = tables(ucd)
    pool, pool_idx, recs, rec_idx = bytearray(), {}, [(0, 0, 0)], {}
    entries = [0] * NCP
    expand = 1.0
    for cp in range(NCP):
        c = classes[cp]
        rec = 0
        if c in (WORD, IDEO, MARK):
            outs = [fold(cp, m) for m in (1, 2, 3)]
            for m, o in zip((1, 2, 3), outs):
                again = "".join(fold(ord(x), m) if classes[ord(x)] in (WORD, IDEO, MARK) else x for x in o)
                assert again == o, f"U+{cp:04X} mode {m} not idempotent"
            if any(o != chr(cp) for o in outs):
                offs = []
                for o in outs:
                    b = o.encode()
                    assert len(b) < 64
                    expand = max(expand, len(b) / len(chr(cp).encode()))
                    if b not in pool_idx:
                        pool_idx[b] = len(pool)
                        pool.extend(bytes([len(b)]) + b)
                    offs.append(pool_idx[b])
                rec = rec_idx.setdefault(tuple(offs), len(recs))
                if rec == len(recs):
                    recs.append(tuple(offs))
        entries[cp] = c << 13 | rec
    assert len(recs) < 8192 and len(pool) < 65536

    best = None
    for shift in range(4, 10):
        bs = 1 << shift
        blocks, idx = {}, []
        for i in range(0, NCP, bs):
            idx.append(blocks.setdefault(tuple(entries[i:i + bs]), len(blocks)))
        size = 2 * len(idx) + 2 * bs * len(blocks)
        if best is None or size < best[0]:
            best = (size, shift, idx, blocks)
    size, shift, idx, blocks = best
    stage2 = [e for blk in sorted(blocks, key=blocks.get) for e in blk]
    assert len(blocks) < 65536

    w = sys.stdout.write
    w("/*\n * sift_analyze_tables.h - generated by tools/analyze/gen_tables.py from\n"
      f" * Unicode {UNICODE}; do not edit. {len(recs)} fold records, {len(pool)} pool bytes,\n"
      f" * {len(blocks)} blocks of {1 << shift}, {size + 6 * len(recs) + len(pool)} bytes total.\n"
      " *\n * Entry: class << 13 | fold record; record r maps modes case, accent,\n"
      " * case+accent to pool offsets of length-prefixed UTF-8 strings.\n */\n")
    w(f'#define SIFT_UNICODE_VERSION "{UNICODE}"\n#define SIFT_CP_SHIFT {shift}\n')
    w(f"#define SIFT_FOLD_MAX_EXPAND {int(expand + 0.999)}\n\n")

    def arr(decl, vals, per=16):
        w(f"static const {decl}[{len(vals)}] = {{\n")
        for i in range(0, len(vals), per):
            w("\t" + ", ".join(str(v) for v in vals[i:i + per]) + ",\n")
        w("};\n\n")

    arr("uint16 sift_cp_stage1", idx)
    arr("uint16 sift_cp_stage2", stage2)
    arr("uint16 sift_fold_recs", [o for r in recs for o in r], 15)
    arr("uint8 sift_fold_pool", list(pool), 24)


if __name__ == "__main__":
    main(sys.argv[1])
