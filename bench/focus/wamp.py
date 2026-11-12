#!/usr/bin/env python3
"""wamp.py: write amplification of a sift_col index at small size, as a slope over 6 / 12 / 24 / 48 batches.

  wamp.py run CASE CK WORK ROOT OUT   CASE std | wide; CK = batches between checkpoints; WORK holds the base heap (lab.py base);
                                      ROOT = the staged install under test; writes OUT.json, OUT.txt (line 1 = the settings)
  wamp.py table OUT.json...           the cells (case x batches): index WAL by operation class, all-relations WAL, index file; the slope between counts
  wamp.py check EXPECT OUT.json...    each cell against the expectation file, within its tolerance; exit 1 on a miss (a class under BIG B/row is not judged)
  wamp.py expect OUT.json...          the expectation rows of these results (what bench/focus/expect-wamp.tsv holds; give two runs of a cell for its noise)

One run is 48 batches of INSERT / UPDATE / soft DELETE, a few reads, VACUUM every 6th batch, with the background workers on (promotion,
merges, the rewrite that the WAL budget allows, relocation). Every statement waits until no worker runs and the WAL stood still, so the
maintenance of a statement lands in its own window. The WAL of a window is read with pg_waldump; a record of the index belongs to the
operation whose logged interval holds it (the worker's log lines placed on the WAL by a 5 ms sample of the insert LSN), else to the statement.
The count of 6 / 12 / 24 / 48 batches is the cumulative WAL of the one run up to that batch, per inserted row.
Classes: wbuf = the statements' own writes (write buffer), promotion, merge (overlay merges), rewrite (partition rewrite), move (relocation, shift, shrink),
other (VACUUM, sidecar, worker bookkeeping).
"""
import calendar, collections, datetime, json, os, re, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lab

BATCHES, COUNTS = 48, (6, 12, 24, 48)
CLASSES = ("wbuf", "promotion", "merge", "rewrite", "move", "other")
TOL, TOL_CLASS, FLOOR = 0.15, 0.30, 3.0
BIG = 100.0  # B/row: a class of the split is judged by check only when its expectation is this large (the totals idx, all and file always are)
LOGL = re.compile(r"^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3}) UTC \[(\d+)\] LOG:  (sift: .*)$")
OPL = re.compile(r'sift: (?P<what>.+?) index "facts_sift" in (?P<ms>\d+) ms(?:, (?P<docs>\d+) documents)?(?:, (?P<bytes>\d+) bytes)?$')
REC = re.compile(r"rmgr: (\S+)\s+len \(rec/tot\):\s*(\d+)/\s*(\d+).*lsn: ([0-9A-F]+)/([0-9A-F]+)")
BLK = re.compile(r"blkref #\d+: rel (\d+)/(\d+)/(\d+)(?: fork (\w+))? blk (\d+)(.*)")
HOLE = re.compile(r"hole: offset: \d+, length: (\d+)")
CLEN = re.compile(r"compression method: \w+, length: (\d+)")
LEAF = ("promotion", "merge", "relocation", "shift", "shrink", "sidecar")   # the steps of a worker; the other log lines span them
NAMES = (("promoted a sealed", "promotion"), ("promoted (", "promotion-outer"), ("merged", "merge"), ("relocated", "relocation"),
         ("shifted", "shift"), ("shrunk", "shrink"), ("rewrote", "rewrote"))


def opname(what):
    for pre, name in NAMES:
        if what.startswith(pre):
            return name
    if "net pre-aggregates" in what:
        return "sidecar"
    return "worker" if "worker finished" in what else what


def group(op, kind, full):
    """the class of a record of the index: the operation that owns it, else the statement"""
    if op == "rewrote" or (op == "merge" and full):
        return "rewrite"
    if op == "merge":
        return "merge"
    if op in ("promotion", "promotion-outer"):
        return "promotion"
    if op in ("relocation", "shift", "shrink"):
        return "move"
    return "wbuf" if not op and kind in ("insert", "update", "delete") else "other"


def epoch(day):
    dt = datetime.datetime.strptime(day, "%Y-%m-%d %H:%M:%S.%f")
    return calendar.timegm(dt.timetuple()) + dt.microsecond / 1e6


def walrecs(cl, a, b):
    """[(lsn, record bytes, total bytes with images, [(filenode, image bytes)])] of the WAL from a to b"""
    out = subprocess.run(["pg_waldump", "-b", "-p", cl.data + "/pg_wal", "-s", a, "-e", b], capture_output=True, text=True, env=dict(os.environ, LC_ALL="C")).stdout
    recs = []
    for l in out.splitlines():
        m = REC.match(l)
        if m:
            recs.append((int(m.group(4), 16) << 32 | int(m.group(5), 16), int(m.group(2)), int(m.group(3)), []))
            continue
        m = BLK.search(l)
        if m and recs:
            img = 0
            if "FPW" in m.group(6):
                h, c = HOLE.search(m.group(6)), CLEN.search(m.group(6))
                img = int(c.group(1)) if c else 8192 - (int(h.group(1)) if h else 0)
            recs[-1][3].append((int(m.group(3)), img))
    return recs


class Wamp(lab.Lab):
    def log_ops(s, off):
        with open(s.cl.log, "rb") as f:
            f.seek(off)
            text = f.read().decode(errors="replace")
        ops = []
        for l in text.splitlines():
            m = LOGL.match(l)
            mm = m and OPL.match(m.group(3))
            if mm:
                ops.append({"name": opname(mm.group("what")), "t1": epoch(m.group(1)), "ms": int(mm.group("ms")), "pid": int(m.group(2))})
        return ops

    def window(s, kind, fn):
        """the WAL of the statement and of the maintenance it started: ({class: bytes of the index}, all bytes, {op: count})"""
        cl = s.cl
        off, a = os.path.getsize(cl.log), cl.psql("select pg_current_wal_lsn()")
        fn(); s.idle(kind)
        e = cl.psql("select pg_current_wal_lsn()")
        node = int(s.ctl.ask("select pg_relation_filenode('facts_sift'::regclass)")[0])
        ops, prev = s.log_ops(off), {}
        for o in ops:
            o["lo"], o["hi"] = s.sam.before(o["t1"] - (o["ms"] + 1) / 1000), s.sam.after(o["t1"])
        # a step owns the WAL from the end of the worker's previous step (its pages go out in a burst just before its log line; "relocated" logs 0 ms)
        for o in sorted((o for o in ops if o["name"] in LEAF), key=lambda o: o["t1"]):
            o["olo"], o["ohi"] = prev.get(o["pid"], o["lo"]), o["hi"]
            prev[o["pid"]] = o["hi"]
        full = any(o["name"] == "rewrote" for o in ops)
        idx, allw = collections.Counter(), 0
        for lsn, rec, tot, refs in walrecs(cl, a, e):
            allw += tot
            mine = [(rec / len(refs) + img) for fnode, img in refs if fnode == node]
            if not mine:
                continue
            op = next((o["name"] for o in ops if o["name"] in LEAF and o["olo"] < lsn <= o["ohi"]), None)
            if op is None:
                wide = [o for o in ops if o["name"] not in LEAF and o["lo"] <= lsn <= o["hi"]]
                op = min(wide, key=lambda o: o["hi"] - o["lo"])["name"] if wide else ""
            idx[group(op, kind, full)] += sum(mine)
        return idx, allw, collections.Counter(o["name"] for o in ops)

    def go(s, ck):
        s.setup()
        s.rep(f"# case={s.case} rows={lab.ROWS} n={lab.N} batches={BATCHES} ck={ck}")
        res = {"settings": s.settings, "case": s.case, "ck": ck, "n": lab.N, "rows": lab.ROWS, "size0": s.size0, "batches": []}
        w, rd = s.sessions()
        for b in range(1, BATCHES + 1):
            idx, allw, ev = collections.Counter(), 0, collections.Counter()

            def ops(kind, fn):
                nonlocal allw
                i, a, e = s.window(kind, fn)
                idx.update(i); ev.update(e); allw += a
            s.batch(w, rd, b, ops)
            if b % ck == 0:
                s.cl.psql("checkpoint")
            res["batches"].append({"b": b, "idx": dict(idx), "all": allw, "size": s.index_size(), "ops": dict(ev)})
            s.rep(f"batch {b:3d}: index WAL {sum(idx.values()) / lab.N:7.1f} B/row ({', '.join(f'{k} {v / lab.N:.0f}' for k, v in idx.most_common())}) "
                  f"all {allw / lab.N:.0f} file {res['batches'][-1]['size'] / 1e6:.1f} MB ops {dict(ev)} {s.cl.status()}")
        json.dump(res, open(s.out + ".json", "w"))
        w.close(); rd.close()


def cells(res):
    """{count: {metric: value}}: B per inserted row up to the count, the index file in MB"""
    out = {}
    for c in COUNTS:
        rows = [x for x in res["batches"] if x["b"] <= c]
        if len(rows) < c:
            continue
        n = c * res["n"]
        cls = {k: sum(x["idx"].get(k, 0) for x in rows) / n for k in CLASSES}
        out[c] = dict(cls, idx=sum(cls.values()), all=sum(x["all"] for x in rows) / n, file=rows[-1]["size"] / 1e6,
                      events=sum(v for x in rows for k, v in x["ops"].items() if k in ("merge", "rewrote", "relocation", "shift", "shrink")))
    return out


def label(res):
    return f"{res['case']}-ck{res['ck']}"


def table(results):
    for r in results:
        print(r.get("settings", ""))
    print(f"{'case':10s} {'batches':>7s} {'idx B/row':>9s} " + " ".join(f"{k:>9s}" for k in CLASSES) + f" {'all B/row':>10s} {'file MB':>8s} {'events':>6s}")
    for r in results:
        cs = cells(r)
        for c, m in cs.items():
            print(f"{label(r):10s} {c:7d} {m['idx']:9.1f} " + " ".join(f"{m[k]:9.1f}" for k in CLASSES) + f" {m['all']:10.0f} {m['file']:8.1f} {m['events']:6d}")
        print(f"{'':10s} marginal B/row between counts (idx, all): " +
              "; ".join(f"{a}->{b}: {(cs[b]['idx'] * b - cs[a]['idx'] * a) / (b - a):.0f}, {(cs[b]['all'] * b - cs[a]['all'] * a) / (b - a):.0f}"
                        for a, b in zip(COUNTS, COUNTS[1:]) if a in cs and b in cs))


def values(results):
    """{(cell, batches, metric): [value of each run]}"""
    out = collections.defaultdict(list)
    for r in results:
        for c, m in cells(r).items():
            for k in ("idx",) + CLASSES + ("all", "file"):
                out[(label(r), c, k)].append(m[k])
    return out


def expect(results):
    """the expectation rows: the mean of the runs, and the tolerance: 15 percent (30 on a class), twice the spread between the runs, 3 B/row on a class"""
    for (cell, c, k), v in sorted(values(results).items()):
        m = sum(v) / len(v)
        tol = max((TOL_CLASS if k in CLASSES else TOL) * m, 2 * (max(v) - min(v)), 0 if k in ("file", "all") else FLOOR)
        print(f"{cell}\t{c}\t{k}\t{m:.1f}\t{tol:.1f}")


def check(path, results):
    with open(path) as f:
        rows = [l.split() for l in f if l.strip() and not l.startswith("#")]
    want = {(cell, int(c), k): (float(v), float(tol)) for cell, c, k, v, tol in rows}
    got, bad = values(results), 0
    for key, (w, tol) in want.items():
        if key not in got or (key[2] in CLASSES and w < BIG):
            continue
        v = sum(got[key]) / len(got[key])
        if abs(v - w) > tol:
            bad += 1
            print(f"FAIL {key[0]} batches={key[1]} {key[2]}: {v:.1f}, expected {w:.1f} +- {tol:.1f} ({100 * (v - w) / max(abs(w), 1e-9):+.0f}%)")
    judged = sum(k in got and (k[2] not in CLASSES or want[k][0] >= BIG) for k in want)
    print(f"{'FAIL' if bad else 'ok'}: {judged} of {len(want)} expectations judged, {bad} outside the tolerance")
    return bad


def main(a):
    if a[0] == "run":
        case, ck, work, root, out = a[1], int(a[2]), a[3], a[4], a[5]
        r = Wamp(case, work, root, out)
        try:
            r.go(ck)
            r.close()
        except BaseException:
            r.kill()
            raise
    else:
        res = [json.load(open(p)) for p in (a[2:] if a[0] == "check" else a[1:])]
        if a[0] == "table":
            table(res)
        elif a[0] == "expect":
            print("# B per inserted row up to the batch count (file: MB); tolerance: 15 percent, twice the spread of the runs, or 3 B/row on a class; classes under 100 B/row are not judged (the split between wbuf, promotion and merge moves by a fifth from run to run: the totals judge them)")
            print(res[0]["settings"] + f"; rows={res[0]['rows']}; n={res[0]['n']}; runs={len(res) // 3}")
            print("# cell\tbatches\tmetric\tvalue\ttolerance")
            expect(res)
        elif a[0] == "check":
            sys.exit(1 if check(a[1], res) else 0)
        else:
            sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:]) if len(sys.argv) > 2 else sys.exit(__doc__)
