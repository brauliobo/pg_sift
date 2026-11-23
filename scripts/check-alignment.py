#!/usr/bin/env python3
"""No type with an alignment above MAXIMUM_ALIGNOF (8) is allocated by palloc, repalloc or a plain context allocation.

PostgreSQL hands out chunks aligned to 8 bytes. A type that holds a 16-byte integer, a SIMD vector or an aligned(N) member needs more: the compiler stores into it with aligned moves
(movaps), which fault on a chunk that is only 8-aligned, and -fsanitize=alignment reports the store. The fix is the aligned allocator (sift_palloc0_aligned, palloc_aligned,
MemoryContextAllocAligned), not a cast or a copy. The over-aligned types are found from their definitions: an explicit alignment, a 16-byte integer or vector member, or a by-value
member (or array) of such a type. A typedef with aligned(N) sets the alignment of the scalar it names (it may lower it); a struct is never lowered, except by packed.

  A  palloc*/MemoryContextAlloc*/sift_mem_alloc* with sizeof(T) or sizeof(*p) of an over-aligned T, or palloc_array(T, n)
  B  the same through the pointer the result is assigned to, or the cast it gets
  C  repalloc* of an over-aligned type (allocate an aligned chunk, copy, pfree)
  D  MAXALIGN/TYPEALIGN(MAXIMUM_ALIGNOF, ...) offset math cast to an over-aligned type

  scripts/check-alignment.py            check src/ (or the source directory given)
  scripts/check-alignment.py --selftest
A site that is aligned by other means says `check-alignment: allow <why>` in a comment on its line or the line above.
"""
import bisect
import multiprocessing
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "../bench/steps"))
import tmpfs  # noqa: E402,F401  (tempfile on tmpfs)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAXALIGN = 8
MACROS = {"MAXIMUM_ALIGNOF": 8, "PG_CACHE_LINE_SIZE": 128, "SIFT_SIMD_ALIGN": 64, "SIFT_VEC_ALIGN": 64, "CC_VB": 32, "SIFT_PAR_SHM_ALIGN": 256}
SCALARS = {"__int128": 16, "__uint128_t": 16, "__int128_t": 16, "long double": 16, "PGIOAlignedBlock": 4096, "PGAlignedXLogBlock": 4096}
SIMD = re.compile(r"^(__m(128|256|512)\w*|(u?int|float)(8|16|32|64)x\d+_t)$")
QUALIFIERS = {"const", "volatile", "struct", "union", "enum", "signed", "unsigned", "long", "short", "register", "static", "extern", "inline", "restrict"}
PLAIN = ("palloc", "palloc0", "palloc_extended", "palloc_array", "palloc0_array", "MemoryContextAlloc", "MemoryContextAllocZero", "MemoryContextAllocHuge",
         "MemoryContextAllocExtended", "sift_mem_alloc", "sift_mem_alloc0")
REALLOC = ("repalloc", "repalloc0", "repalloc_huge", "repalloc_array", "sift_mem_realloc")
CALL = re.compile(r"\b(%s)\s*\(" % "|".join(sorted(PLAIN + REALLOC, key=len, reverse=True)))
NEST = r"(?:[^()]|\((?:[^()]|\([^()]*\))*\))*"
ATTR = re.compile(r"__attribute__\s*\(\((%s)\)\)|pg_attribute_(aligned|packed)\s*\(([^()]*)\)|\b_?[Aa]lignas\s*\(([^()]*)\)" % NEST)
ALIGNED = re.compile(r"\baligned\s*\(([^()]*)\)")
STRUCT_OPEN = re.compile(r"\b(struct|union)\b((?:\s+\w+|\s*__attribute__\s*\(\([^{};]*?\)\)|\s*pg_attribute_\w+\s*\([^(){};]*\)|\s*_Alignas\s*\([^(){};]*\))*)\s*\{")
TYPEDEF = re.compile(r"\btypedef\b([^;{}]*);")
BODY = re.compile(r"\)[ \t]*\n\{.*?\n\}[ \t]*\n", re.S)
BODY_START = re.compile(r"^\{", re.M)
SKIP = re.compile(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)
OFFSET_CAST = re.compile(r"\b(?:MAXALIGN|TYPEALIGN\s*\(\s*MAXIMUM_ALIGNOF)\b[^;]*?\(\s*(?:struct\s+)?(\w+)\s*\*\s*\)|\(\s*(?:struct\s+)?(\w+)\s*\*\s*\)[^;]*?\b(?:MAXALIGN|TYPEALIGN\s*\(\s*MAXIMUM_ALIGNOF)\b")
ALLOW = "check-alignment: allow"
SEPARATORS = {}
SRC = TYPES = MENTIONS = None


def strip(text):
    """comments and string literals blanked, the line breaks kept"""
    def blank(m):
        t = m.group(0)
        return "\n" * t.count("\n") if t[0] == "/" else ("''" if t[0] == "'" else '""')
    return SKIP.sub(blank, text)


class Types:
    """what the definitions say: the members of each struct, its explicit alignment, the typedef aliases"""

    def __init__(self):
        self.defs = {}
        self.unresolved = set()
        self.memo = {}
        self.why = {}

    def define(self, name, members=(), explicit=None, packed=False, alias=None, override=None):
        d = self.defs.setdefault(name, {"members": [], "explicit": None, "packed": False, "alias": None, "override": None})
        d["members"] += members
        d["explicit"] = max(d["explicit"] or 0, explicit or 0) or None
        d["packed"] = d["packed"] or packed
        d["alias"], d["override"] = alias or d["alias"], override or d["override"]

    def merge(self, defs, unresolved):
        for name, d in defs.items():
            self.define(name, **d)
        self.unresolved |= unresolved

    def value(self, expr):
        """an alignment expression, None for a name MACROS does not give"""
        missing = [n for n in re.findall(r"[A-Za-z_]\w*", expr) if n not in MACROS]
        self.unresolved.update(missing)
        if missing or not re.fullmatch(r"[\w\s()*+<>-]+", expr):
            return None
        return int(eval(re.sub(r"[A-Za-z_]\w*", lambda m: str(MACROS[m.group(0)]), expr), {"__builtins__": {}}))

    def align(self, name, stack=()):
        if name in self.memo:
            return self.memo[name]
        d = self.defs.get(name)
        if name in SCALARS or SIMD.match(name):
            a, why = SCALARS.get(name, 16), name
        elif d is None or name in stack:
            return 1
        elif d["override"]:
            a, why = d["override"], "typedef aligned(%d)" % d["override"]
        elif d["alias"]:
            a = self.align(d["alias"], stack + (name,))
            why = self.why.get(d["alias"])
        elif d["packed"] and not d["explicit"]:
            a, why = 1, None
        else:
            a, why = d["explicit"] or 1, "aligned(%d)" % (d["explicit"] or 1)
            for t, e in d["members"]:
                m, via = self.align(t, stack + (name,)), "%s -> %s" % (t, self.why.get(t))
                if e and e >= m:
                    m, via = e, "member aligned(%d)" % e
                if m > a:
                    a, why = m, via
        self.memo[name] = a
        if a > MAXALIGN:
            self.why[name] = why
        return a

    def over(self, name):
        return self.align(name) > MAXALIGN


def balanced(text, i):
    """the index after the brace that closes the one at text[i]"""
    depth = 0
    for m in re.finditer(r"[{}]", text[i:]):
        depth += 1 if m.group(0) == "{" else -1
        if depth == 0:
            return i + m.end()
    return len(text)


def split_top(text, sep):
    """text cut at the sep that are not inside brackets"""
    if sep not in SEPARATORS:
        SEPARATORS[sep] = re.compile(r"[({\[\]})]|" + re.escape(sep))
    out, depth, last = [], 0, 0
    for m in SEPARATORS[sep].finditer(text):
        c = m.group(0)
        if c == sep and depth == 0:
            out.append(text[last:m.start()])
            last = m.end()
        elif c in "({[":
            depth += 1
        elif c in ")}]":
            depth -= 1
    out.append(text[last:])
    return out


def attributes(types, text):
    """(the text without its attributes, the explicit alignment, packed)"""
    if "aligned" not in text and "packed" not in text and "lignas" not in text:
        return text, None, False
    found = {"aligned": None, "packed": False}

    def take(m):
        if m.group(4) is not None:
            n = types.value(m.group(4).strip())
        elif m.group(2) == "aligned":
            n = types.value(m.group(3).strip())
        elif m.group(2) == "packed":
            n = None
            found["packed"] = True
        else:
            a = ALIGNED.search(m.group(1))
            n = types.value(a.group(1).strip()) if a else None
            found["packed"] = found["packed"] or re.search(r"\bpacked\b", m.group(1)) is not None
        found["aligned"] = max(found["aligned"] or 0, n or 0) or None
        return " "
    return ATTR.sub(take, text), found["aligned"], found["packed"]


def specifier(idents):
    """the type a declaration names: its words without the qualifiers"""
    if "__int128" in idents:
        return "__int128"
    if "long" in idents and "double" in idents:
        return "long double"
    spec = [t for t in idents if t not in QUALIFIERS]
    return spec[-1] if spec else "int"


def members(types, body):
    """the (type, explicit alignment) of the by-value members of a struct body (a pointer member holds no alignment)"""
    found = []
    for decl in split_top(body, ";"):
        decl = decl.strip()
        if "{" in decl:
            head, _, rest = decl.partition("{")
            inner, trailer = rest[:rest.rfind("}")], rest[rest.rfind("}") + 1:]
            tag = re.search(r"\b(?:struct|union)\s+(\w+)\s*$", attributes(types, head)[0])
            by_value = not trailer.strip() or any("*" not in d and "(" not in d for d in split_top(trailer, ","))
            inner_members = members(types, inner)
            if tag:
                types.define(tag.group(1), members=inner_members)
            if by_value:
                found += [(tag.group(1), None)] if tag else inner_members
        elif decl:
            decl, explicit, _ = attributes(types, decl)
            first = re.sub(r"\[[^\]]*\]", "", split_top(decl, ",")[0])
            idents = re.findall(r"[A-Za-z_]\w*", first)
            if ":" in first or "*" in first or "(" in first or len(idents) < 2:
                continue
            found.append((specifier(idents[:-1]), explicit))
    return found


def collect(types, text):
    """the struct, union and typedef definitions of a file (the text without function bodies)"""
    for m in STRUCT_OPEN.finditer(text):
        close = balanced(text, m.end() - 1)
        end = text.find(";", close)
        head, h_explicit, h_packed = attributes(types, m.group(2))
        trailer, t_explicit, t_packed = attributes(types, text[close:end] if end >= 0 else "")
        names = re.findall(r"\w+", head)[:1]
        if re.search(r"\btypedef\s*$", text[max(0, m.start() - 12):m.start()]):
            names += [d.group(1) for d in (re.match(r"\s*(\w+)\s*(?:\[.*)?$", t) for t in split_top(trailer, ",")) if d]
        mem = members(types, text[m.end():close - 1])
        for n in names:
            types.define(n, members=mem, explicit=max(h_explicit or 0, t_explicit or 0) or None, packed=h_packed or t_packed)
    for m in TYPEDEF.finditer(text):
        stmt, explicit, _ = attributes(types, m.group(1))
        idents = re.findall(r"[A-Za-z_]\w*", stmt)
        if "(" not in stmt and "*" not in stmt and len(idents) > 1 and specifier(idents[:-1]) != idents[-1]:
            types.define(idents[-1], alias=specifier(idents[:-1]), override=explicit)


def read(rel):
    with open(os.path.join(SRC, rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def parse_file(rel):
    types = Types()
    collect(types, BODY.sub("\n", strip(read(rel))))
    return types.defs, types.unresolved


def argument_list(text, at):
    """the arguments of the call whose opening parenthesis is text[at]"""
    depth = 0
    for i in range(at, len(text)):
        depth += text[i] == "("
        depth -= text[i] == ")"
        if depth == 0:
            return split_top(text[at + 1:i], ",")
    return []


def message(rel, line, fn, t, types):
    if fn in REALLOC:
        what = "%s of over-aligned type %s (align %d via %s): the chunk is aligned only when it came from an aligned allocator; allocate with palloc_aligned / sift_palloc0_aligned, memcpy and pfree" % (fn, t, types.align(t), types.why[t])
    else:
        what = "unaligned %s for over-aligned type %s (align %d via %s): PostgreSQL palloc only guarantees MAXIMUM_ALIGNOF (8); use MemoryContextAllocAligned / palloc_aligned / sift_palloc0_aligned" % (fn, t, types.align(t), types.why[t])
    return "%s:%d: %s, or allow with `%s <why>`" % (rel, line, what, ALLOW)


def check_file(rel):
    """the violations of a file that names an over-aligned type"""
    raw = read(rel)
    if not MENTIONS.search(raw):
        return []
    types, text, bad = TYPES, strip(raw), []
    lines = raw.split("\n")
    starts = [m.start() for m in BODY_START.finditer(text)]

    def line_of(pos):
        return text.count("\n", 0, pos) + 1

    def allowed(line):
        return ALLOW in lines[line - 1] or (line > 1 and ALLOW in lines[line - 2])

    def pointers(pos):
        """the over-aligned pointer variables the function that holds pos declares (parameters and locals)"""
        k = bisect.bisect_right(starts, pos)
        if not k:
            return {}
        sig = max(text.rfind(";", 0, starts[k - 1]), text.rfind("}", 0, starts[k - 1])) + 1
        found = {}
        for m in re.finditer(r"\b(?:struct\s+)?([A-Za-z_]\w*)\s*(\*+)\s*([A-Za-z_]\w*)\b", text[sig:pos]):
            if m.group(2) == "*" and types.over(m.group(1)):
                found[m.group(3)] = m.group(1)
        return found

    for m in CALL.finditer(text):
        fn = m.group(1)
        args = argument_list(text, m.end() - 1)
        joined = " ".join(args)
        before = text[max(text.rfind(c, 0, m.start()) for c in ";{}") + 1:m.start()].strip()
        hit = None
        for t in re.findall(r"\bsizeof\s*\(\s*(?:struct\s+)?(\w+)\s*\)", joined):
            hit = t if types.over(t) else hit
        if fn.endswith("_array"):
            for a in args:
                hit = re.sub(r"^struct\s+", "", a.strip()) if types.over(re.sub(r"^struct\s+", "", a.strip())) else hit
        if not hit:
            sized = re.findall(r"\bsizeof\s*\(?\s*\*\s*(\w+)\s*\)?", joined)
            cast = re.search(r"\(\s*(?:struct\s+)?(\w+)\s*\*\s*\)\s*$", before)
            lhs = re.search(r"(?<![.>\w])([A-Za-z_]\w*)\s*=\s*(?:\(\s*[\w\s]+?\*+\s*\)\s*)?$", before)
            local = pointers(m.start()) if sized or lhs else {}
            if sized and sized[-1] in local:
                hit = local[sized[-1]]
            elif cast and types.over(cast.group(1)):
                hit = cast.group(1)
            elif lhs and lhs.group(1) in local and fn not in REALLOC:
                hit = local[lhs.group(1)]
        if hit and not allowed(line_of(m.start())):
            bad.append(message(rel, line_of(m.start()), fn, hit, types))
    if "MAXALIGN" in text or "TYPEALIGN" in text:
        for m in OFFSET_CAST.finditer(text):
            t, line = m.group(1) or m.group(2), line_of(m.start())
            if types.over(t) and not allowed(line):
                bad.append("%s:%d: MAXALIGN offset math cast to over-aligned type %s (align %d via %s): align the offset to _Alignof(%s) (SIFT_PAR_SHM_ALIGN for the shared blocks), or allow with `%s <why>`"
                           % (rel, line, t, types.align(t), types.why[t], t, ALLOW))
    return bad


def prefix_regex(words):
    """one regex for the words, a trie so that the scan of a file tests a word only where its first letters match"""
    trie = {}
    for w in words:
        node = trie
        for c in w:
            node = node.setdefault(c, {})
        node[""] = {}

    def rx(node):
        alts = [re.escape(c) + rx(sub) for c, sub in sorted(node.items()) if c]
        body = alts[0] if len(alts) == 1 else "(?:" + "|".join(alts) + ")"
        return "(?:%s)?" % body if "" in node and alts else body
    return re.compile(r"\b" + rx(trie) + r"\b")


def over_files(fn, rels):
    """fn of every file, in worker processes when there are many (a fork: the workers read the types the parent built)"""
    workers = min(8, os.cpu_count() or 1)
    if workers < 2 or len(rels) < 64:
        return list(map(fn, rels))
    with multiprocessing.get_context("fork").Pool(workers) as pool:
        return pool.map(fn, rels, chunksize=max(1, len(rels) // (workers * 4)))


def check(src):
    global SRC, TYPES, MENTIONS
    SRC = src
    rels = sorted(os.path.relpath(os.path.join(d, n), src) for d, _, names in os.walk(src) for n in names if n.endswith((".c", ".h")))
    TYPES = types = Types()
    for defs, unresolved in over_files(parse_file, rels):
        types.merge(defs, unresolved)
    MENTIONS = prefix_regex([n for n in list(types.defs) + list(SCALARS) if types.over(n)])
    bad = [b for found in over_files(check_file, rels) for b in found]
    return bad + ["an alignment names %s, which MACROS in scripts/check-alignment.py does not give a value: add it" % n for n in sorted(types.unresolved)]


CASES = [
    ("direct aligned(64) struct with palloc", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(void)\n{\n\tS *x;\n\tx = palloc(sizeof(S));\n}\n", ["a.c:5"]),
    ("transitive embedding", "typedef __int128 I;\ntypedef struct In { I v; } In;\ntypedef struct Out { In a; int n; } Out;\nvoid f(MemoryContext cx)\n{\n\tvoid *p = MemoryContextAllocZero(cx, sizeof(Out));\n}\n", ["a.c:6"]),
    ("pointer member does not propagate", "typedef struct S { char c; } __attribute__((aligned(64))) S;\ntypedef struct HasPtr { S *p; } HasPtr;\nvoid f(void)\n{\n\tvoid *p = palloc(sizeof(HasPtr));\n}\n", []),
    ("sizeof of the variable", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(void)\n{\n\tS *x = palloc(sizeof(*x));\n}\n", ["a.c:4"]),
    ("repalloc", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(void *x, int n)\n{\n\tx = repalloc(x, n * sizeof(S));\n}\n", ["a.c:4"]),
    ("aligned allocators", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(MemoryContext cx)\n{\n\tvoid *a = palloc_aligned(sizeof(S), 64, 0);\n\tvoid *b = MemoryContextAllocAligned(cx, sizeof(S), _Alignof(S), 0);\n\tS *c = sift_palloc0_aligned(S, 2);\n}\n", []),
    ("allow comment", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(void)\n{\n\tvoid *a = palloc(sizeof(S));\t/* check-alignment: allow one chunk, aligned by hand */\n\t/* check-alignment: allow the line above */\n\tvoid *b = palloc(sizeof(S));\n}\n", []),
    ("cast of the result", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(void)\n{\n\tvoid *p = (S *) palloc(100);\n}\n", ["a.c:4"]),
    ("pointer assigned", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(MemoryContext cx, int n)\n{\n\tS *s;\n\n\ts = MemoryContextAlloc(cx, n);\n}\n", ["a.c:6"]),
    ("a member of the same name is not the variable", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(char **rows, int n)\n{\n\tS *s = NULL;\n\n\trows[0] = palloc(n);\n\tx.s = palloc(n);\n}\n", []),
    ("typedef that lowers the alignment", "typedef __int128 L __attribute__((aligned(8)));\ntypedef struct P { L v; } P;\nvoid f(void)\n{\n\tvoid *p = palloc(sizeof(P));\n}\n", []),
    ("macro alignment, array member, pointer array", "typedef struct Q { char a[4]; } __attribute__((aligned(2 * PG_CACHE_LINE_SIZE))) Q;\ntypedef struct R { Q q[2]; } R;\nvoid f(void)\n{\n\tR **pp = palloc(sizeof(R *) * 4);\n\tvoid *r = palloc0(sizeof(R) * 4);\n}\n", ["a.c:6"]),
    ("array dimension macro is not a type", "typedef struct S { char c; } __attribute__((aligned(64))) S;\ntypedef struct W { S s[SOME_MAX]; } W;\nvoid f(void)\n{\n\tvoid *p = palloc(sizeof(W));\n}\n", ["a.c:5"]),
    ("array macro", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(void)\n{\n\tvoid *a = palloc0_array(S, 4);\n}\n", ["a.c:4"]),
    ("offset math", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(char *base, long off)\n{\n\tS *s = (S *) (base + MAXALIGN(off));\n}\n", ["a.c:4"]),
    ("comments and strings are ignored", "typedef struct S { char c; } __attribute__((aligned(64))) S;\nvoid f(void)\n{\n\t/* palloc(sizeof(S)) */\n\tconst char *m = \"palloc(sizeof(S))\";\n}\n", []),
    ("anonymous union member", "typedef struct U { union { __int128 w; char b[16]; }; int n; } U;\nvoid f(void)\n{\n\tvoid *p = palloc(sizeof(U));\n}\n", ["a.c:4"]),
    ("struct with an aligned tag, no typedef", "struct T { int a; } __attribute__((aligned(32)));\nvoid f(void)\n{\n\tvoid *p = palloc(sizeof(struct T));\n}\n", ["a.c:4"]),
    ("packed lowers a struct", "typedef struct K { __int128 v; } __attribute__((packed)) K;\nvoid f(void)\n{\n\tvoid *p = palloc(sizeof(K));\n}\n", []),
    ("a macro nobody gave a value is refused", "typedef struct S { char c; } __attribute__((aligned(NOBODY))) S;\n", ["an alignment names NOBODY"]),
]


def selftest():
    for name, text, want in CASES:
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "a.c"), "w") as f:
                f.write(text)
            got = [b if b.startswith("an alignment") else ":".join(b.split(":")[:2]) for b in check(d)]
            assert [g[:len(w)] for g, w in zip(got, want)] == want and len(got) == len(want), "%s: wanted %s, got %s" % (name, want, got)
    print("check-alignment selftest: ok (%d cases)" % len(CASES))


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        selftest()
        sys.exit(0)
    paths = [a for a in sys.argv[1:] if not a.startswith("--")]
    bad = check(paths[0] if paths else os.path.join(ROOT, "src"))
    for b in bad:
        print(b)
    if bad:
        sys.exit(1)
    print("check-alignment: no over-aligned type is allocated by a plain palloc, repalloc or MAXALIGN offset")
