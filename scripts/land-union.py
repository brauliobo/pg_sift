#!/usr/bin/env python3
"""land-union.py [--sorted] FILE: resolve the conflict hunks of FILE when both sides only ADD whole lines of a list:
`VAR += names` lines of a Makefile (TESTING_REGRESS += ...), or rows of a tsv/allow file. Both sides are kept, once each.
Afterwards each list entry is there once (dedupe: names of a `VAR +=` line already in an earlier line of that VAR, identical rows of a tsv), order kept.
A hunk with any other Makefile line (an assignment `=`, a recipe, a deletion) is left alone: exit 1, the owner merges the tip.
FILE may hold the diff3 style (`|||||||` section of the merge base): a hunk whose base section is not empty is a change of existing lines, not an addition: exit 1.
--sorted: the lines of the file after its leading `#` and `=` lines are put in byte order again (for a list the file keeps sorted).

land-union.py --changelog RESULT MERGE_BASE HEAD TIP: check the CHANGELOG.md that a merge of the tip left (the files are the result, and the versions of the merge base,
the PR head and the tip). Exit 1 with the reason when conflict markers are left, the merge duplicated a line of `## Unreleased`, the entries outside `## Unreleased` differ from the
tip's, or an entry the PR added is not inside `## Unreleased`."""
import re, sys
from collections import Counter


def dedupe(lines, is_make):
    """each list entry once, the first one kept, order kept: a `VAR += names` line of a Makefile loses the names an earlier line of the same VAR has (a line left empty goes);
    any other list line is dropped when an identical one came before (comments, `=` rows and blank lines are not entries)"""
    entry = re.compile(r"^([A-Z][A-Z0-9_]*) \+= (\S.*)$")
    seen, names, out = set(), {}, []
    for line in lines:
        m = entry.match(line) if is_make and not line.endswith("\\") else None
        if m and not all(re.fullmatch(r"[\w%.-]+", n) for n in m[2].split()):
            m = None  # a make expression ($(call ..)) is not a list of names: left as it is
        if m:
            have = names.setdefault(m[1], set())
            kept = [n for n in m[2].split() if n not in have]
            have.update(kept)
            if kept:
                out.append(f"{m[1]} += {' '.join(kept)}" if kept != m[2].split() else line)
        elif is_make and not m or not line.strip() or line[0] in "#=" or line not in seen:
            seen.add(line)
            out.append(line)
    return out


def union(path, sort=False):
    is_make = path.endswith("Makefile") or path.endswith(".mk")
    entry = re.compile(r"^[A-Z][A-Z0-9_]* \+= \S.*$") if is_make else re.compile(r".+")
    out, side, ours, theirs, base = [], None, [], [], []
    for line in open(path).read().split("\n"):
        if line.startswith("<<<<<<< "):
            side, ours, theirs, base = "o", [], [], []
        elif line.startswith("||||||| ") and side == "o":
            side = "b"
        elif line.startswith("=======") and side in ("o", "b"):
            if side == "b" and base:
                return 1
            side = "t"
        elif line.startswith(">>>>>>> ") and side == "t":
            both = [l for l in ours + theirs if l.strip()]
            if not all(entry.match(l) for l in both):
                return 1
            seen = set()
            for l in both:
                if l not in seen:
                    seen.add(l)
                    out.append(l)
            side = None
        elif side == "b":
            if line.strip():
                base.append(line)
        elif side == "o":
            ours.append(line)
        elif side == "t":
            theirs.append(line)
        else:
            out.append(line)
    if side is not None:
        return 1
    out = dedupe(out, is_make)
    if sort:
        keep = 0
        while keep < len(out) and out[keep][:1] in ("#", "="):
            keep += 1
        end = len(out) - 1 if out and out[-1] == "" else len(out)
        out = out[:keep] + sorted(out[keep:end], key=lambda l: l.encode()) + out[end:]
    open(path, "w").write("\n".join(out))
    return 0


def read(path):
    return open(path).read().split("\n")


def block(lines):
    """the half-open range of the lines under `## Unreleased`, or None"""
    if "## Unreleased" not in lines:
        return None
    s = lines.index("## Unreleased") + 1
    e = next((i for i in range(s, len(lines)) if lines[i].startswith("## ")), len(lines))
    return s, e


def changelog(result, mb, head, tip):
    lines, tips = read(result), read(tip)
    b, tb = block(lines), block(tips)
    if b is None or tb is None:
        return "no `## Unreleased` section"
    if any(l.startswith(("<<<<<<< ", ">>>>>>> ")) or l == "=======" for l in lines):
        return "conflict markers are left"
    inside = lines[b[0]:b[1]]
    # a line twice that neither the tip nor the PR head already had twice: what this merge introduced (a duplicate the tip carries is not the PR's to fix)
    hl = read(head)
    hb = block(hl) or (0, 0)
    seen = Counter(l for l in inside if l.strip())
    dup = [l for l, n in seen.items() if n > 1 and n > max(1, tips[tb[0]:tb[1]].count(l), hl[hb[0]:hb[1]].count(l))]
    if dup:
        return "a line of Unreleased is duplicated: " + dup[0][:100]
    entries = lambda ls: Counter(l for l in ls if l.startswith("- "))
    if entries(lines[:b[0]] + lines[b[1]:]) != entries(tips[:tb[0]] + tips[tb[1]:]):
        return "the entries outside Unreleased differ from the tip's"
    base = set(read(mb))
    lost = [l for l in read(head) if l.startswith("- ") and l not in base and l not in set(inside)]
    if lost:
        return "an entry of the PR is not inside Unreleased: " + lost[0][:100]
    return None


if __name__ == "__main__":
    if sys.argv[1] == "--changelog":
        why = changelog(*sys.argv[2:6])
        if why:
            print("CHANGELOG.md: " + why)
        sys.exit(1 if why else 0)
    sort = sys.argv[1] == "--sorted"
    sys.exit(union(sys.argv[-1], sort))
