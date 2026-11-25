#!/usr/bin/env bash
# Complexity anti-patterns of the C sources that a lexical scan can decide (needs no PostgreSQL and no build), the family of the O(N^2) findings of the 6.0 sweep (Spec 66):
#   repalloc-additive    repalloc whose size adds to the old one (n + 1, n + sizeof) with no geometric growth beside it: a copy of the array per element
#   qsort-in-loop        qsort, qsort_arg or pg_qsort inside a loop: the same data sorted once per outer round
#   hash-seq-hot         hash_seq_search nested in another loop, or in a function that is not a teardown, a statistic or an invalidation: a table walk per row
#   memmove-in-loop      memmove of a dynamic array inside a loop: an insertion or a delete that shifts the tail per element
#   manifest-unbounded   sift_manifest_read_range(rel, lo, NULL): the whole tail of the manifest, a read of every partition per operation
#   mem-alloc-hot        MemoryContextMemAllocated in a scan, slot, fetch or per-row function, or in a loop without a stride: a walk of the context per call
#   spinlock-loop        a for or while loop (bound over 8) between SpinLockAcquire and SpinLockRelease: work proportional to the data under a spinlock
#   foreach-node-raw     foreach_node(Node, ...): castNode(Node, ...) has no T_Node, so it crashes under --enable-cassert (no row of the allow list is accepted)
#   bit-by-bit-u64       a 64-iteration loop testing one bit of a mask per pass: walk the set bits with sift_ctz64
#   bsearch-in-seq-loop  bsearch or sift_dv_find per element of a loop that walks its input in order: a two-pointer cursor is O(R + D)
# A site that is bounded some other way is counted in scripts/complexity.allow: RULE, PATH, COUNT, REASON, a tab between them. A file over its COUNT fails, and so does
# a file under it (the row is lowered or deleted when the code is fixed), so the list shrinks with the code. One site that is bounded for a reason that fits a line says so
# in a comment on the line or the line above: `check-complexity: allow RULE: why`.
#
#   scripts/check-complexity.sh              every C source of src, sqlite and mariadb
#   scripts/check-complexity.sh --diff [BASE]  only the sources that differ from BASE (default HEAD, staged and unstaged): the pre-commit hook
#   scripts/check-complexity.sh --list       scripts/complexity.allow as the tree would need it (reasons kept): `--list > scripts/complexity.allow` refreshes it
#   scripts/check-complexity.sh --selftest   a mutant and a clean source for every rule, the ratchet in both directions, the zero row of foreach-node-raw, --diff
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$ROOT" "$0" "$@" <<'PY'
import contextlib, hashlib, io, json, os, re, subprocess, sys, tempfile

HINTS = {
    "repalloc-additive": "grow by a factor (cap = cap ? cap * 2 : 8, SIFT_REGROW) instead of by one element: n appends cost O(n^2) copies",
    "qsort-in-loop": "sort once outside the loop (or merge sorted runs): a sort per round is O(M * N log N)",
    "hash-seq-hot": "keep an intrusive list or an index of the entries you need: a table walk per call is O(N * H)",
    "memmove-in-loop": "collect, then compact in one pass (two cursors): a shift per element is O(N^2)",
    "manifest-unbounded": "read the range the operation needs (lo and hi): an open tail reads every partition",
    "mem-alloc-hot": "read the context size once per batch or every 2^k calls (a stride): MemoryContextMemAllocated walks every block",
    "spinlock-loop": "copy what is needed under the lock and loop after SpinLockRelease, or use an LWLock: a spinlock holder must stay O(1)",
    "foreach-node-raw": "use foreach_ptr(Node, ...) instead of foreach_node(Node, ...): castNode(Node, ...) has no T_Node and crashes under --enable-cassert",
    "bit-by-bit-u64": "walk the set bits with while (w) { int b = sift_ctz64(w); w &= w - 1; }",
    "bsearch-in-seq-loop": "use a two-pointer cursor (SiftDvCursor): O(R + D) instead of an O(R log D) search per row",
}
WHAT = {
    "repalloc-additive": "additive repalloc", "qsort-in-loop": "sort inside a loop", "hash-seq-hot": "hash_seq_search in a hot path",
    "memmove-in-loop": "memmove of a dynamic array inside a loop", "manifest-unbounded": "unbounded manifest read",
    "mem-alloc-hot": "MemoryContextMemAllocated in a hot path", "spinlock-loop": "loop under a spinlock",
    "foreach-node-raw": "foreach_node(Node, ...)", "bit-by-bit-u64": "bit-by-bit loop over a 64-bit mask",
    "bsearch-in-seq-loop": "binary search inside a sequential row loop",
}
NEVER_ALLOWED = {"foreach-node-raw"}
DEFAULT_REASON = {
    "repalloc-additive": "a bounded array or a sentinel slot; Spec 66 ON-RES tracks the growth sites",
    "qsort-in-loop": "one sort of a fresh temporary array per outer bucket",
    "hash-seq-hot": "a bounded table walked once per call",
    "memmove-in-loop": "a bounded array shifted in a cold path",
    "manifest-unbounded": "maintenance or inspection of a whole relation",
    "mem-alloc-hot": "a census or a budget check; the call is not per row",
    "spinlock-loop": "a fixed number of slots of a shared table",
    "bit-by-bit-u64": "a bounded loop outside the hot path",
    "bsearch-in-seq-loop": "an unsorted or random-order probe list",
}
HEADER = (
    "# scripts/complexity.allow - baseline ratchet of scripts/check-complexity.sh: the sites of each rule a file has today.\n"
    "# Columns (tab-separated): RULE\tPATH\tCOUNT\tREASON\n"
    "#   - foreach-node-raw never has a row (a hard zero).\n"
    "#   - A new site, or a COUNT above the row, fails. A fixed site lowers the actual count under the row and fails too until the row is lowered or deleted,\n"
    "#     so the list shrinks to zero. A new row is not the fix: the site gets the geometric growth, the cursor or the single sort.\n"
    "#   - Rows are sorted by RULE, then PATH (LC_ALL=C). Refresh: scripts/check-complexity.sh --list > scripts/complexity.allow\n"
)

STRIP = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|^[ \t]*#(?:[^\n]*\\\n)*[^\n]*', re.S | re.M)
NOT_NL = re.compile(r'[^\n]')
TRIGGER = re.compile(r'repalloc|qsort|hash_seq_search|memmove|sift_manifest_read_range|MemoryContextMemAllocated|SpinLockAcquire|foreach_node|bsearch|sift_dv_find'
                     r'|1ULL\s*<<|UINT64CONST\(1\)\s*<<|>>\s*\w+\)\s*&\s*1\b')
LOOPKW = re.compile(r'\b(for|while|do)\b')
STMT = re.compile(r'[;{(]')
IF_AT = re.compile(r'\s*if\b')
ELSE_AT = re.compile(r'\s*else\b')
WHILE_AT = re.compile(r'\s*while\b')
BRACE = re.compile(r'[{};}]')
SPIN = re.compile(r'\bSpinLockAcquire\s*\(')
SPIN_TOKEN = re.compile(r'[{}]|\bSpinLockRelease\s*\(')
NODE_ARG = re.compile(r'\(\s*Node\s*,')
NAMED = re.compile(r'\b[a-z_]\w*\b')
SIZEOF = re.compile(r'sizeof\s*\([^)]*\)')
BIT_TRIGGER = re.compile(r'1ULL\s*<<|UINT64CONST\(1\)\s*<<|>>\s*\w+\)\s*&\s*1\b|\(uint64\)\s*1\s*<<|UINT64_C\(1\)\s*<<|1UL\s*<<')
SITE = re.compile(r'\b(repalloc(?:0|_huge|_extended)?|qsort_arg|qsort|pg_qsort|hash_seq_search|memmove|MemoryContextMemAllocated|foreach_node'
                  r'|sift_dv_find|bsearch|sift_manifest_read_range)\s*\(')
BRACKETS = re.compile(r'[()\[\]{},]')
FN_NAME = re.compile(r'(\w+)\s*\(')
ALLOW_COMMENT = re.compile(r'check-complexity:\s*allow\s+([a-z0-9-]+)')
SKIP_FILE = re.compile(r'[_/]test\.c$|/test/|/spv\.h$')

GEOMETRIC = re.compile(r'\*=\s*2|<<=\s*1|\*\s*2\b|\b2\s*\*|<<\s*1\b|next_pow2|nextpower2|SIFT_REGROW|\bcap\w*|\w*cap\b|alloc\w*')
GROWTH_CONTEXT = re.compile(r'\*=\s*2|<<=\s*1|=\s*\w+\s*\*\s*2\b|\*\s*2\s*:|SIFT_REGROW|next_pow2|pg_nextpower2')
CONST_STEP = re.compile(r'[^+\s]\s*\+\s*\d+\b|\b\d+\s*\+\s*[^+=\s]')
SENTINEL = re.compile(r'off|run|hll|estart')
PLUS_ONE = re.compile(r'\+\s*1\b')
VAR_STEP = re.compile(r'[^+\s]\s*\+\s*[^+=\s]')
DYNAMIC_ARRAY = re.compile(r'groups|gran|entries|parts|wait|ext|slab|dim|order|dir|win|\bb\[|leaves')
TAIL_SHIFT = re.compile(r'\(\s*\w[\w>.\-\[\]]*\s*-\s*[\w(]|--')
TEARDOWN = re.compile(r'free|destroy|reset|end|close|drop|clear|inval|evict_all|drain|dump|stat|explain|cleanup|fini|finish|flush|shutdown')
HOT_FILE = re.compile(r'scan|slot|fetch|gcache|gbuf|rowbuf|granule|codec')
HOT_FN = re.compile(r'next|row|tuple|batch|slot|fetch|offer|add|put|evict')
MEM_ALLOWED_FILE = re.compile(r'core/(?:memcensus|memtrack|cachebudget)\.c$|sqlite/host/host\.c$')
STRIDE = re.compile(r'%|[&]\s*\(?\s*\w*stride')
SEQ_HEADER = re.compile(r'\w\s*\+\+|\+=\s*1\b')
SEQ_BOUND = re.compile(r'<\s*[\w>.\-\[\]()]*?(?:nrows|nrow|nr|n|cnt|count|ndels|len|nitems|nent|nents)\s*(?:;|$)')
BIT_LOOP = re.compile(r';\s*(\w+)\s*<\s*(?:64|BITS_PER_WORD|SIFT_SKIP_SPAN|SIFT_VEC_BATCH|V6_BATCH_SIZE)\b[^;]*;')
SMALL_BOUND = re.compile(r'<=?\s*(\d+)\s*;')


def blank(m):
    s = m.group(0)
    if s[0] in "\"'":
        return s[0] + " " * (len(s) - 2) + s[-1]
    return " " * len(s) if "\n" not in s else NOT_NL.sub(" ", s)


def clean_text(raw):
    return STRIP.sub(blank, raw)


def match_close(text, open_pos):
    depth = 0
    for m in BRACKETS.finditer(text, open_pos):
        c = m.group(0)
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
            if depth == 0:
                return m.start()
    return len(text) - 1


def call_args(text, open_pos):
    end = match_close(text, open_pos)
    args, depth, start = [], 0, open_pos + 1
    for m in BRACKETS.finditer(text, open_pos + 1, end):
        c = m.group(0)
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif depth == 0:
            args.append(text[start:m.start()])
            start = m.end()
    args.append(text[start:end])
    return [" ".join(a.split()) for a in args], end


class Loop:
    __slots__ = ("kind", "header", "start", "end", "body_pos")

    def __init__(self, kind, header, start, end, body_pos):
        self.kind, self.header, self.start, self.end, self.body_pos = kind, header, start, end, body_pos


def stmt_end(text, p):
    first_if = IF_AT.match(text, p) is not None
    while True:
        m = STMT.search(text, p)
        if not m:
            return len(text)
        p = match_close(text, m.start()) + 1 if m.group(0) in "({" else m.end()
        if m.group(0) == "(":
            continue
        e = ELSE_AT.match(text, p) if first_if else None
        if not e:
            return p
        p = e.end()


class Scan:
    def __init__(self, rel, raw):
        self.rel, self.raw, self.text = rel, raw, clean_text(raw)
        self.found = []
        self.loop_list = None
        self.fn_spans = None

    def loops(self):
        if self.loop_list is None:
            text, skip, out = self.text, set(), []
            for m in LOOPKW.finditer(text):
                if m.start() in skip:
                    continue
                kind = m.group(1)
                if kind == "do":
                    end = stmt_end(text, m.end())
                    tail = WHILE_AT.match(text, end)
                    if tail:
                        skip.add(tail.end() - 5)
                    out.append(Loop("do", "", m.start(), end, m.end()))
                    continue
                op = text.find("(", m.end() - 1)
                cl = match_close(text, op)
                out.append(Loop(kind, " ".join(text[op + 1:cl].split()), m.start(), stmt_end(text, cl + 1), cl + 1))
            self.loop_list = out
        return self.loop_list

    def functions(self):
        if self.fn_spans is None:
            text, depth, hdr, out, open_pos = self.text, 0, 0, [], 0
            for m in BRACE.finditer(text):
                c = m.group(0)
                if c == "{":
                    if depth == 0:
                        open_pos = m.start()
                        f = FN_NAME.search(text, hdr, open_pos)
                        out.append([open_pos, len(text), f.group(1) if f else ""])
                    depth += 1
                elif c == "}":
                    depth = max(0, depth - 1)
                    if depth == 0:
                        out[-1][1] = m.start()
                        hdr = m.end()
                elif depth == 0:
                    hdr = m.end()
            self.fn_spans = out
        return self.fn_spans

    def declares(self, pos):
        col0 = self.text[self.text.rfind("\n", 0, pos) + 1]
        return col0 not in " \t\n" and not any(lo <= pos < hi for lo, hi, _ in self.functions())

    def fn_at(self, pos):
        for lo, hi, name in self.functions():
            if lo <= pos < hi:
                return name
        return ""

    def containing(self, pos):
        return [lp for lp in self.loops() if lp.start <= pos < lp.end]

    def add(self, rule, pos, detail):
        self.found.append((rule, pos, detail))

    def prev_lines(self, pos, n):
        start = pos
        for _ in range(n):
            start = self.text.rfind("\n", 0, start)
            if start < 0:
                return self.text[:pos]
        return self.text[start:pos]

    def site(self, name, pos, open_pos):
        text = self.text
        if self.declares(pos):
            return
        if name == "foreach_node":
            if NODE_ARG.match(text, open_pos):
                self.add("foreach-node-raw", pos, WHAT["foreach-node-raw"])
        elif name.startswith("repalloc"):
            self.repalloc(pos, open_pos)
        elif name in ("qsort", "qsort_arg", "pg_qsort"):
            if self.containing(pos):
                self.add("qsort-in-loop", pos, "%s() inside a loop" % name)
        elif name == "hash_seq_search":
            if len(self.containing(pos)) >= 2 or not TEARDOWN.search(self.fn_at(pos)):
                self.add("hash-seq-hot", pos, "hash_seq_search() in a hot path")
        elif name == "memmove":
            self.memmove(pos, open_pos)
        elif name == "sift_manifest_read_range":
            args, _ = call_args(text, open_pos)
            if args and args[-1] == "NULL":
                self.add("manifest-unbounded", pos, "sift_manifest_read_range(..., NULL) reads the whole tail")
        elif name == "MemoryContextMemAllocated":
            self.mem_alloc(pos)
        elif name in ("sift_dv_find", "bsearch"):
            if any(lp.kind == "for" and SEQ_HEADER.search(lp.header) and SEQ_BOUND.search(lp.header) for lp in self.containing(pos)):
                self.add("bsearch-in-seq-loop", pos, "%s() inside a sequential row loop" % name)

    def repalloc(self, pos, open_pos):
        args, _ = call_args(self.text, open_pos)
        size = args[1] if len(args) > 1 else ""
        in_loop = bool(self.containing(pos))
        if not (CONST_STEP.search(size) or in_loop and VAR_STEP.search(size)) or GEOMETRIC.search(size):
            return
        if SENTINEL.search(args[0]) and PLUS_ONE.search(size):
            return
        if not in_loop and ("sizeof" not in size or GROWTH_CONTEXT.search(self.prev_lines(pos, 8))):
            return
        self.add("repalloc-additive", pos, "repalloc() grows by an additive step (%s)" % size)

    def memmove(self, pos, open_pos):
        args, _ = call_args(self.text, open_pos)
        if len(args) < 3 or not self.containing(pos):
            return
        dest, size = args[0], args[2]
        if not NAMED.search(SIZEOF.sub("", size)):
            return
        if DYNAMIC_ARRAY.search(dest) or TAIL_SHIFT.search(size):
            self.add("memmove-in-loop", pos, "memmove() of a dynamic array inside a loop")

    def mem_alloc(self, pos):
        if MEM_ALLOWED_FILE.search(self.rel):
            return
        if not (HOT_FILE.search(self.rel) or HOT_FN.search(self.fn_at(pos))):
            if not self.containing(pos) or STRIDE.search(self.prev_lines(pos, 2) + self.text[pos:pos + 80].split("\n")[0]):
                return
        self.add("mem-alloc-hot", pos, "MemoryContextMemAllocated() in a hot path")

    def spin_regions(self):
        text, regions = self.text, []
        for a in SPIN.finditer(text):
            depth, susp, start = 0, None, a.end()
            for m in SPIN_TOKEN.finditer(text, a.end()):
                c = m.group(0)
                if c == "{":
                    depth += 1
                elif c == "}":
                    depth -= 1
                    if depth < 0:
                        if susp is None:
                            regions.append((start, m.start()))
                        break
                    if susp is not None and depth < susp:
                        start, susp = m.start(), None
                elif depth == 0:
                    if susp is None:
                        regions.append((start, m.start()))
                    break
                elif susp is None:
                    regions.append((start, m.start()))
                    susp = depth
            else:
                if susp is None:
                    regions.append((start, len(text)))
        return regions

    def spin_loops(self):
        regions = self.spin_regions()
        for lp in self.loops():
            if lp.kind == "do" or not any(lo <= lp.start < hi for lo, hi in regions):
                continue
            small = SMALL_BOUND.search(lp.header + ";") if lp.kind == "for" else None
            if not (small and int(small.group(1)) <= 8) and lp.header != "0":
                self.add("spinlock-loop", lp.start, "%s loop under a spinlock" % lp.kind)

    def bit_loops(self):
        for lp in self.loops():
            m = BIT_LOOP.search(lp.header + ";") if lp.kind == "for" else None
            if m:
                body = "\n".join(self.text[lp.body_pos:lp.body_pos + 600].split("\n")[:7])
                v = re.escape(m.group(1))
                tests = r'&\s*\(?\s*(?:1ULL|UINT64CONST\(1\)|\(uint64\)\s*1|UINT64_C\(1\)|1UL)\s*<<\s*%s\b|>>\s*%s\s*\)\s*&\s*1\b' % (v, v)
                if re.search(tests, body):
                    self.add("bit-by-bit-u64", lp.start, "loop to 64 testing one bit per pass")

    def run(self):
        text = self.text
        for m in SITE.finditer(text):
            self.site(m.group(1), m.start(), m.end() - 1)
        if "SpinLockAcquire" in text:
            self.spin_loops()
        if BIT_TRIGGER.search(text):
            self.bit_loops()
        return self.sites()

    def sites(self):
        out, line, last = [], 1, 0
        lines = [""] + self.raw.split("\n")
        for rule, pos, detail in sorted(self.found, key=lambda f: f[1]):
            line += self.text.count("\n", last, pos)
            last = pos
            if rule not in NEVER_ALLOWED and rule in (a for n in (line, line - 1) for a in ALLOW_COMMENT.findall(lines[n])):
                continue
            out.append((rule, self.rel, line, "%s in %s():" % (detail, self.fn_at(pos))))
        return out


def scan_source(rel, raw):
    return Scan(rel, raw).run() if TRIGGER.search(raw) else []


def tracked(root):
    out = subprocess.run(["git", "ls-files", "--", "src/*.c", "src/*.h", "sqlite/*.c", "sqlite/*.h", "mariadb/*.c", "mariadb/*.h"], cwd=root, capture_output=True, text=True, check=True)
    return [f for f in out.stdout.split() if not SKIP_FILE.search(f)]


def changed(root, base):
    out = subprocess.run(["git", "diff", "--name-only", "--diff-filter=ACMR", base, "--", "src", "sqlite", "mariadb"], cwd=root, capture_output=True, text=True, check=True)
    return [f for f in out.stdout.split() if f.endswith((".c", ".h")) and not SKIP_FILE.search(f) and os.path.exists(os.path.join(root, f))]


def scan_files(root, files, cache=None):
    found = []
    for rel in files:
        path = os.path.join(root, rel)
        st = os.stat(path)
        stamp = [st.st_size, st.st_mtime_ns]
        hit = cache.get(rel) if cache is not None else None
        if hit is None or hit[0] != stamp:
            with open(path, errors="replace") as fh:
                hit = [stamp, scan_source(rel, fh.read())]
            if cache is not None:
                cache[rel] = hit
        found.extend(tuple(s) for s in hit[1])
    return found


class Cache:
    """The sites of each file under its size and modification time, per box (a CI run, or SIFT_LINT_NOCACHE=1, reads every file): a run after an edit scans the files that changed"""

    def __init__(self, script):
        base = os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")
        self.path = os.path.join(base, "pg_sift", "complexity.json")
        with open(script, "rb") as fh:
            self.version = hashlib.sha1(fh.read()).hexdigest()
        self.on = not (os.environ.get("CI") or os.environ.get("SIFT_LINT_NOCACHE"))
        self.files = {}
        try:
            with open(self.path) as fh:
                saved = json.load(fh)
            if self.on and saved["version"] == self.version:
                self.files = saved["files"]
        except (OSError, ValueError, KeyError):
            pass

    def store(self):
        if not self.on:
            return
        os.makedirs(os.path.dirname(self.path), exist_ok=True)
        tmp = "%s.%d" % (self.path, os.getpid())
        with open(tmp, "w") as fh:
            json.dump({"version": self.version, "files": self.files}, fh)
        os.replace(tmp, self.path)


def read_allow(root):
    path = os.path.join(root, "scripts", "complexity.allow")
    rows, reasons = {}, {}
    if not os.path.exists(path):
        return rows, reasons
    with open(path) as fh:
        for n, line in enumerate(fh.read().split("\n"), 1):
            if not line.strip() or line.startswith("#"):
                continue
            p = line.split("\t")
            if len(p) != 4 or not p[2].isdigit() or not p[3].strip() or p[0] not in HINTS:
                print("complexity.allow:%d: RULE, PATH, COUNT and the reason, a tab between them" % n)
                sys.exit(1)
            if p[0] in NEVER_ALLOWED:
                print("complexity.allow:%d: %s never has a row: fix the site (%s)" % (n, p[0], HINTS[p[0]]))
                sys.exit(1)
            rows[(p[0], p[1])] = (int(p[2]), n)
            reasons[(p[0], p[1])] = p[3]
    return rows, reasons


def counts(found):
    c = {}
    for rule, rel, _, _ in found:
        c[(rule, rel)] = c.get((rule, rel), 0) + 1
    return c


def check(root, files, cache=None):
    rows, _ = read_allow(root)
    found = scan_files(root, files, cache)
    c, scanned = counts(found), set(files)
    bad, stale = [], []
    for rule, rel, line, detail in sorted(found, key=lambda s: (s[1], s[2], s[0])):
        have, allow = c[(rule, rel)], rows.get((rule, rel), (0, 0))[0]
        if have > allow:
            note = "never allowed" if rule in NEVER_ALLOWED else "file has %d site(s), scripts/complexity.allow allows %d" % (have, allow)
            bad.append("%s:%d: [%s] %s\n    %s.\n    (%s)" % (rel, line, rule, detail, HINTS[rule], note))
    for (rule, rel), (allow, n) in sorted(rows.items()):
        if rel in scanned and c.get((rule, rel), 0) < allow:
            stale.append("scripts/complexity.allow:%d: [%s] %s has %d site(s), the row allows %d: lower or delete the row" % (n, rule, rel, c.get((rule, rel), 0), allow))
    return bad, stale, found


def listing(root, files, cache=None):
    _, reasons = read_allow(root)
    c = counts(scan_files(root, files, cache))
    out = [HEADER.rstrip("\n")]
    for (rule, rel), n in sorted(c.items()):
        if rule not in NEVER_ALLOWED:
            out.append("%s\t%s\t%d\t%s" % (rule, rel, n, reasons.get((rule, rel), DEFAULT_REASON[rule])))
    return "\n".join(out) + "\n"


MUTANTS = {
    "repalloc-additive": ("src/m/ra.c", "void f(int *p, int n)\n{\n\tfor (int i = 0; i < n; i++)\n\t\tp = repalloc(p, (i + 1) * sizeof(int));\n}\n"),
    "qsort-in-loop": ("src/m/qs.c", "void f(int **b, int n)\n{\n\twhile (n--)\n\t\tqsort(b[n], 10, 4, cmp);\n}\n"),
    "hash-seq-hot": ("src/m/hs.c", "void f(HTAB *h)\n{\n\tHASH_SEQ_STATUS st;\n\n\thash_seq_init(&st, h);\n\twhile ((e = hash_seq_search(&st)) != NULL)\n\t\tuse(e);\n}\n"),
    "memmove-in-loop": ("src/m/mm.c", "void f(int *groups, int n)\n{\n\tfor (int i = 0; i < n; i++)\n\t\tmemmove(&groups[i + 1], &groups[i], (n - i) * sizeof(int));\n}\n"),
    "manifest-unbounded": ("src/m/mu.c", "void f(Relation rel, const uint8 *lo)\n{\n\tmr = sift_manifest_read_range(rel,\n\t\t\t\t\t\t\t\t  lo, NULL);\n}\n"),
    "mem-alloc-hot": ("src/m/slot_ma.c", "void f(MemoryContext c)\n{\n\tx = MemoryContextMemAllocated(c, true);\n}\n"),
    "spinlock-loop": ("src/m/sl.c", "void f(S *s, int n)\n{\n\tSpinLockAcquire(&s->lock);\n\tfor (int i = 0; i < n; i++)\n\t\ts->v[i] = 0;\n\tSpinLockRelease(&s->lock);\n}\n"),
    "foreach-node-raw": ("src/m/fn.c", "void f(List *l)\n{\n\tforeach_node(Node, n, l)\n\t\tuse(n);\n}\n"),
    "bit-by-bit-u64": ("src/m/bb.c", "void f(uint64 mask)\n{\n\tfor (int i = 0; i < 64 && i < n; i++)\n\t{\n\t\tif (mask & (UINT64CONST(1) << i))\n\t\t\tuse(i);\n\t}\n}\n"),
    "bsearch-in-seq-loop": ("src/m/bs.c", "void f(int *keys, int nrows, int *dv, int nd)\n{\n\tfor (int r = 0; r < nrows; r++)\n\t\tif (bsearch(&keys[r], dv, nd, 4, cmp))\n\t\t\tuse(r);\n}\n"),
}
CLEAN = {
    "repalloc-additive": ("src/c/ra.c", "void f(int *p, int n, int cap)\n{\n\tfor (int i = 0; i < n; i++)\n\t{\n\t\tif (i >= cap)\n\t\t{\n\t\t\tcap *= 2;\n\t\t\tp = repalloc(p, cap * sizeof(int));\n\t\t}\n\t}\n"
                         "\tp = repalloc(p, (cap + 1) * sizeof(int));\n\tp = repalloc(p, strlen(s) + 1);\n\tp = repalloc(p, (n + m) * sizeof(int));\n\te->eoff = repalloc(e->eoff, (m + 1) * sizeof(uint32));\n}\n"),
    "qsort-in-loop": ("src/c/qs.c", "void f(int *b, int n)\n{\n\tqsort(b, n, 4, cmp);\n\tfor (int i = 0; i < n; i++)\n\t\tuse(b[i]); /* qsort( in a comment */\n}\n"),
    "hash-seq-hot": ("src/c/hs.c", "extern void *hash_seq_search(HASH_SEQ_STATUS *status);\n\nvoid *\nhash_seq_search(HASH_SEQ_STATUS *s)\n{\n\treturn s;\n}\n\nvoid tbl_free(HTAB *h)\n{\n\twhile ((e = hash_seq_search(&st)) != NULL)\n\t\tuse(e);\n}\n"),
    "memmove-in-loop": ("src/c/mm.c", "void f(int *groups, int n)\n{\n\tmemmove(&groups[1], &groups[0], (n - 1) * sizeof(int));\n\tfor (int i = 0; i < n; i++)\n\t\tmemmove(buf, tmp, 7 * sizeof(uint32));\n}\n"),
    "manifest-unbounded": ("src/c/mu.c", "void f(Relation rel, const uint8 *lo, const uint8 *hi)\n{\n\tmr = sift_manifest_read_range(rel, lo, hi);\n\tmr = sift_manifest_read_range(rel, NULL, hi);\n}\n"),
    "mem-alloc-hot": ("src/c/ma.c", "void f(MemoryContext c, int n)\n{\n\tx = MemoryContextMemAllocated(c, true);\n\tfor (int i = 0; i < n; i++)\n\t\tif (i % 64 == 0)\n\t\t\tx = MemoryContextMemAllocated(c, true);\n}\n"),
    "spinlock-loop": ("src/c/sl.c", "void f(S *s, int n)\n{\n\tSpinLockAcquire(&s->lock);\n\tfor (int i = 0; i < 2; i++)\n\t\ts->v[i] = 0;\n\tif (n)\n\t{\n\t\tSpinLockRelease(&s->lock);\n\t\tfor (int i = 0; i < n; i++)\n\t\t\tuse(i);\n\t\treturn;\n\t}\n"
                      "\tSpinLockRelease(&s->lock);\n\tfor (int i = 0; i < n; i++)\n\t\tuse(i);\n}\n"),
    "foreach-node-raw": ("src/c/fn.c", "void f(List *l)\n{\n\tforeach_ptr(Node, n, l)\n\t\tuse(n);\n\tforeach_node(OpExpr, e, l)\n\t\tuse(e);\n}\n"),
    "bit-by-bit-u64": ("src/c/bb.c", "void f(uint64 w, uint64 *match)\n{\n\twhile (w)\n\t{\n\t\tint b = sift_ctz64(w);\n\n\t\tw &= w - 1;\n\t\tuse(b);\n\t}\n\tfor (int i = 0; i < 64; i++)\n\t\tmatch[i >> 6] |= UINT64CONST(1) << (i & 63);\n}\n"),
    "bsearch-in-seq-loop": ("src/c/bs.c", "void f(int *keys, int nrows, int *dv, int nd)\n{\n\tif (bsearch(&keys[0], dv, nd, 4, cmp))\n\t\tuse(0);\n\tfor (int r = nrows; r > 0; r = next(r))\n\t\tif (bsearch(&keys[r], dv, nd, 4, cmp))\n\t\t\tuse(r);\n}\n"),
}


def rules_of(rel, text):
    return sorted({s[0] for s in scan_source(rel, text)})


def fail(msg):
    print("check-complexity: selftest: " + msg)
    sys.exit(1)


def git(d, *a):
    subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@example.com", *a], cwd=d, check=True, capture_output=True)


def selftest():
    for rule, (rel, text) in MUTANTS.items():
        got = rules_of(rel, text)
        if got != [rule]:
            fail("the mutant of %s was reported as %s" % (rule, got))
        if len(scan_source(rel, text)) != 1:
            fail("the mutant of %s has %d sites, want 1" % (rule, len(scan_source(rel, text))))
    for rule, (rel, text) in CLEAN.items():
        got = rules_of(rel, text)
        if got:
            fail("the clean source of %s was reported as %s" % (rule, got))
    rel, text = MUTANTS["qsort-in-loop"]
    if scan_source(rel, text.replace("\t\tqsort", "\t\t/* check-complexity: allow qsort-in-loop: one bucket */\n\t\tqsort")):
        fail("an inline allow comment did not clear the site")
    rel, text = MUTANTS["foreach-node-raw"]
    if not scan_source(rel, text.replace("\tforeach_node", "\t/* check-complexity: allow foreach-node-raw: x */\n\tforeach_node")):
        fail("an inline allow comment cleared foreach-node-raw")
    nested = "void f(int n)\n{\n\tfor (int i = 0; i < n; i++)\n\t{\n\t\tif (x)\n\t\t\tqsort(a, n, 4, cmp);\n\t}\n\tqsort(a, n, 4, cmp);\n\tdo\n\t{\n\t\tn--;\n\t} while (n > 0);\n\tqsort(a, n, 4, cmp);\n}\n"
    if [s[2] for s in scan_source("src/m/n.c", nested)] != [6]:
        fail("a loop leaked past its closing brace or a do-while tail: %s" % scan_source("src/m/n.c", nested))
    unbraced = "void f(int n)\n{\n\tfor (int i = 0; i < n; i++)\n\t\tif (x)\n\t\t\tqsort(a, n, 4, cmp);\n\tqsort(a, n, 4, cmp);\n}\n"
    if [s[2] for s in scan_source("src/m/u.c", unbraced)] != [5]:
        fail("a loop without braces did not end at its statement: %s" % scan_source("src/m/u.c", unbraced))
    with tempfile.TemporaryDirectory() as d:
        os.makedirs(os.path.join(d, "scripts"))
        subprocess.run(["git", "init", "-q"], cwd=d, check=True)
        for rel, text in list(MUTANTS.values()) + list(CLEAN.values()):
            path = os.path.join(d, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as fh:
                fh.write(text)
        git(d, "add", ".")
        git(d, "commit", "-qm", "base")
        files = tracked(d)
        allow = os.path.join(d, "scripts", "complexity.allow")
        bad, stale, _ = check(d, files)
        if len(bad) != len(MUTANTS) or stale:
            fail("without an allow list %d of %d mutants failed the check (stale %s)" % (len(bad), len(MUTANTS), stale))
        if "allows 0" not in bad[0] or HINTS[re.search(r"\[([a-z0-9-]+)\]", bad[0]).group(1)] not in bad[0]:
            fail("the failure text lacks the count or the hint: %s" % bad[0])
        with open(allow, "w") as fh:
            fh.write(listing(d, files))
        bad, stale, _ = check(d, files)
        rows, _ = read_allow(d)
        if len(rows) != len(MUTANTS) - 1 or len(bad) != 1 or "foreach-node-raw" not in bad[0] or stale:
            fail("the baseline of --list did not clear every rule but foreach-node-raw: %s %s %d" % (bad, stale, len(rows)))
        with open(allow, "a") as fh:
            fh.write("foreach-node-raw\tsrc/m/fn.c\t1\ttry\n")
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                read_allow(d)
            fail("a foreach-node-raw row was accepted")
        except SystemExit as e:
            if e.code != 1:
                fail("a foreach-node-raw row ended with the wrong status")
        with open(allow) as fh:
            lines = [ln for ln in fh.read().split("\n") if not ln.startswith("foreach-node-raw")]
        os.remove(os.path.join(d, "src/m/fn.c"))
        git(d, "rm", "-q", "--cached", "src/m/fn.c")
        files = tracked(d)
        with open(allow, "w") as fh:
            fh.write("\n".join(lines))
        bad, stale, _ = check(d, files)
        if bad or stale:
            fail("the baseline does not hold without the foreach-node-raw mutant: %s %s" % (bad, stale))
        qs = os.path.join(d, "src/m/qs.c")
        with open(qs, "a") as fh:
            fh.write("void g(int n)\n{\n\twhile (n--)\n\t\tqsort(a, 4, 4, cmp);\n}\n")
        bad, stale, _ = check(d, files)
        if len(bad) != 2 or "has 2 site(s)" not in bad[0] or stale:
            fail("a site added over a row did not fail the ratchet: %s %s" % (bad, stale))
        git(d, "checkout", "-q", "--", "src/m/qs.c")
        with open(qs, "w") as fh:
            fh.write("void f(int **b, int n)\n{\n\tqsort(b[0], 10, 4, cmp);\n}\n")
        bad, stale, _ = check(d, files)
        if bad or len(stale) != 1 or "lower or delete the row" not in stale[0]:
            fail("a fixed site did not fail as a stale row: %s %s" % (bad, stale))
        if changed(d, "HEAD") != ["src/m/qs.c"]:
            fail("--diff does not list the one edited source: %s" % changed(d, "HEAD"))
        bad, stale, _ = check(d, changed(d, "HEAD"))
        if bad or len(stale) != 1:
            fail("--diff does not hold the touched file to its row: %s %s" % (bad, stale))
        git(d, "checkout", "-q", "--", "src/m/qs.c")
        with open(allow, "a") as fh:
            fh.write("qsort-in-loop\tsrc/gone.c\t1\treason\n")
        if check(d, tracked(d))[1] != []:
            fail("a row of a file that is not scanned must not be judged by a partial run")
        cache = {}
        before = scan_files(d, ["src/m/qs.c"], cache)
        with open(qs, "w") as fh:
            fh.write("void g(int n)\n{\n\twhile (n--)\n\t\tqsort(a, 4, 4, cmp);\n\twhile (n++)\n\t\tqsort(a, 4, 4, cmp);\n}\n")
        if scan_files(d, ["src/m/qs.c"], cache) == before or len(scan_files(d, ["src/m/qs.c"], cache)) != 2:
            fail("a cached result outlived an edit of its file")
    print("check-complexity: selftest ok, %d rules, mutant and clean source of each, ratchet up and down, zero row, inline allow, cache, --diff" % len(MUTANTS))


def main():
    root, script, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    if args[:1] == ["--selftest"]:
        selftest()
        return 0
    cache = Cache(script)
    if args[:1] == ["--list"]:
        sys.stdout.write(listing(root, tracked(root), cache.files))
        cache.store()
        return 0
    files = changed(root, args[1] if len(args) > 1 else "HEAD") if args[:1] == ["--diff"] else tracked(root)
    bad, stale, _ = check(root, files, cache.files)
    cache.store()
    if bad or stale:
        print("check-complexity: %d complexity regression(s) found. Fix the site at its cause (the hint of each); only a site that is bounded elsewhere is a row of scripts/complexity.allow, with that bound as its reason:" % len(bad))
        print("\n".join(bad + stale))
        return 1
    print("check-complexity: ok, %d files" % len(files))
    return 0


sys.exit(main())
PY
