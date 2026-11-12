#!/usr/bin/env python3
"""reads.py: the read cost of a churned sift_col index at small size, per class, warm and cold.

  reads.py run CASE WORK ROOT OUT    CASE std | wide; WORK holds the base heap (lab.py base); ROOT = the staged install under test; writes OUT.json, OUT.txt
  reads.py table OUT.json...         per class: mean, p90, phase-balanced median of the warm instructions, blocks, cold figures (runs pooled)
  reads.py gap OUT.json...           the churned index against a fresh build of the same table, per class (FOCUS_FRESH=1 runs): absolute instructions and blocks, and what the state says
  env: FOCUS_READ_BATCHES (54), FOCUS_READ_FROM (13: the first batch measured), FOCUS_COLD_EVERY (7), FOCUS_HOLD (0: none), FOCUS_FRESH (0: no fresh builds)

FOCUS_HOLD=B is the concurrent-writer arm (run.sh held): at batch B a second session inserts a batch of rows and keeps its transaction open while the write buffer is promoted
(an inserter in flight at the moment of a promotion, as concurrent writers always have), then commits. The promoted overlay waits for that transaction (docs/COL-HEAPFREE.md); a build that
cannot settle it leaves every segment it is merged into without a stamp, and every read checks their rows in the heap. The state of each batch says the share of the index entries in segments
without a stamp, and the table prints it next to the read cost.

The churn is the one of wamp.py (INSERT / UPDATE / soft DELETE, VACUUM every 6th batch, workers on, every statement waited for). From the first measured
batch on, each batch ends with the ten classes of lab.Q on the churned index: instructions of the backend (perf_event_open), shared blocks hit / read, plan.
The key window of a measured batch moves by delta(b) = b * 53 % 400, the same for every build, so no run depends on where a partition cut falls against fixed keys.
Warm = the second run in a fresh backend. Cold = the first statement of a fresh backend after the relations were evicted, on every 7th batch (7 is coprime
with the 6 batches of the write-buffer cycle, so the cold samples meet every phase). Phase = the batch number mod 6; the batches with a VACUUM (phase 0) are a phase of their own.
"""
import collections, json, os, re, statistics, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lab

BATCHES = int(os.environ.get("FOCUS_READ_BATCHES", "54"))
FROM = int(os.environ.get("FOCUS_READ_FROM", "13"))
COLD_EVERY = int(os.environ.get("FOCUS_COLD_EVERY", "7"))
HOLD = int(os.environ.get("FOCUS_HOLD", "0"))
FRESH = bool(int(os.environ.get("FOCUS_FRESH", "0")))   # on the cold batches the same classes also run on a fresh build of a copy of the table (UNLOGGED, the same heap order, settled)
SAMPLE_GRACE = 0.1
EVICT = ("select pg_buffercache_evict_relation(c.oid) from pg_class c where relname in ('facts', 'facts_sift', 'facts_pkey') "
         "or relname like 'pg_toast_%'")
STATE = ("select buffer_rows || '|' || segments || '|' || dirty_segments || '|' || dead_rows || '|' || index_bytes || '|' || "
         "(select coalesce(sum(entries) filter (where stamp is null), 0) || '/' || coalesce(sum(entries), 0) from sift.col_stamps('facts_sift')) from sift.col_index_status")


def parse(txt):
    m = re.search(r"Buffers: shared (hit=(\d+))?\s*(read=(\d+))?", txt)
    ex = re.search(r"Execution Time: ([\d.]+)", txt)
    return {"hit": int(m.group(2) or 0) if m else 0, "read": int(m.group(4) or 0) if m else 0, "ms": float(ex.group(1)) if ex else -1.0,
            "plan": "sift" if re.search(r"Sift|sift", txt) else "other"}


class Reads(lab.Lab):
    def sample(s, b, q, cold, tab="facts"):
        """instructions, blocks and plan of EXPLAIN (ANALYZE, BUFFERS) of class q in a fresh backend, on the table tab (a copy for the fresh build)"""
        sql = re.sub(r"\bfacts\b", tab, lab.rotate(lab.Q[q], lab.delta(b)))
        se = lab.Sess(s.cl, ["set max_parallel_workers_per_gather = 0", "set sift.enable_agg = on"], err=s.err)
        if q == "pointx":
            se.ask("set enable_indexscan = off"); se.ask("set enable_bitmapscan = off")
        se.ask(EVICT.replace("'facts'", f"'{tab}'").replace("'facts_sift'", f"'{tab}_sift'").replace("'facts_pkey'", f"'{tab}_pkey'")) if cold else se.ask(sql)
        out, ins = se.counted("explain (analyze, buffers, timing off, summary on) " + sql)
        se.close()
        return dict(parse("\n".join(out)), b=b, q=q, mode="cold" if cold else "warm", instr=ins, tab=tab)

    def fresh_copy(s):
        """facts_b: the table as it is now with a freshly built index, settled as the lab's start is (VACUUM until the status of the index stands, no worker running)"""
        cl = s.cl
        cl.psql("drop table if exists facts_b")
        cl.psql("create unlogged table facts_b as select * from facts")
        cl.psql("alter table facts_b add primary key (id)")
        cl.psql(f"create index facts_b_sift on facts_b using sift_col ({lab.index_cols(s.case)}) with ({lab.INDEX_OPTS})")
        prev = None
        for _ in range(10):
            cl.psql("vacuum facts_b")
            for _ in range(600):
                time.sleep(0.2)
                if s.ctl.ask("select (select count(*) from pg_stat_progress_vacuum) || '|' || coalesce((select bool_or(running) from sift.worker_state('facts_b_sift'::regclass)), false)")[0] == "0|false":
                    break
            time.sleep(1.0)
            cur = s.ctl.ask("select segments || '|' || units || '|' || index_bytes from sift.col_index_status where index::text = 'facts_b_sift'")[0]
            if cur == prev:
                break
            prev = cur
        cl.psql("checkpoint")

    def hold(s, w, rd, b, run):
        """the batch b with an inserter in flight at the promotion: its rows are in the write buffer, the buffer is promoted, it commits"""
        a = lab.Sess(s.cl, err=s.err)
        a.ask("begin")
        a.ask(lab.insert_sql(s.case, lab.N, (b * 4 + 3) / 1000).replace("begin; ", "").replace(" commit;", ""))
        s.batch(w, rd, b, run)
        s.ctl.ask("select sift.col_promote('facts_sift')"); s.idle()
        a.ask("commit"); a.close()

    def go(s):
        s.setup()
        s.cl.psql("create extension if not exists pg_buffercache")
        s.rep(f"# case={s.case} rows={lab.ROWS} n={lab.N} batches={BATCHES} from={FROM} cold_every={COLD_EVERY}")
        res = {"settings": s.settings, "case": s.case, "n": lab.N, "rows": lab.ROWS, "from": FROM, "samples": [], "state": {}}
        w, rd = s.sessions()

        def run(kind, fn):
            fn(); s.idle(kind)
        for b in range(1, BATCHES + 1):
            s.hold(w, rd, b, run) if b == HOLD else s.batch(w, rd, b, run)
            if b < FROM:
                s.rep(f"batch {b:3d} {s.cl.status()}")
                continue
            cold = (b - FROM) % COLD_EVERY == 0
            if cold:
                s.cl.psql("checkpoint")
            res["state"][b] = s.ctl.ask(STATE)[0]
            res["hold"] = HOLD
            for c in ((True, False) if cold else (False,)):
                for q in lab.Q:
                    res["samples"].append(s.sample(b, q, c))
                    s.idle(grace=SAMPLE_GRACE)   # a read can ask for maintenance: it ends before the next class, so every build meets the same state
            if FRESH and cold:
                s.fresh_copy()
                res["state"]["fresh%d" % b] = s.ctl.ask(STATE.replace("sift.col_index_status", "sift.col_index_status where index::text = 'facts_b_sift'").replace("'facts_sift'", "'facts_b_sift'"))[0]
                for c in (True, False):
                    for q in lab.Q:
                        res["samples"].append(s.sample(b, q, c, "facts_b"))
                s.cl.psql("drop table facts_b")
            warm = {x["q"]: x["instr"] for x in res["samples"] if x["b"] == b and x["mode"] == "warm" and x["tab"] == "facts"}
            s.rep(f"batch {b:3d} {'cold ' if cold else ''}state={res['state'][b]} warm M instr " + " ".join(f"{q}={v / 1e6:.2f}" for q, v in warm.items()))
        json.dump(res, open(s.out + ".json", "w"))
        w.close(); rd.close()


def phase(b):
    return b % lab.VACUUM_EVERY


def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))]


SPIKE = 3   # a sample above this many times the median of its class is a spike: one-shot, 50-120 M instructions, on every build alike


def stats(samples, q, mode):
    """mean, mean without spikes, p90, phase-balanced median (the median of the medians of the phases) of the instructions; mean blocks hit / read; spikes; n"""
    x = [d for d in samples if d["q"] == q and d["mode"] == mode and d["instr"] > 0 and d.get("tab", "facts") == "facts"]
    if not x:
        return None
    by = collections.defaultdict(list)
    for d in x:
        by[phase(d["b"])].append(d["instr"])
    ins = [d["instr"] for d in x]
    med = statistics.median(ins)
    kept = [i for i in ins if i <= SPIKE * med]
    return {"mean": statistics.fmean(ins), "tmean": statistics.fmean(kept), "p90": pct(ins, 90), "pbm": statistics.median(statistics.median(v) for v in by.values()),
            "hit": statistics.fmean(d["hit"] for d in x), "read": statistics.fmean(d["read"] for d in x), "n": len(x), "spikes": len(ins) - len(kept),
            "other": sum(d["plan"] != "sift" for d in x)}


def load(paths):
    out = []
    for p in paths:
        out += json.load(open(p))["samples"]
    return out


def unstamped(path):
    """the share of the index entries in segments without a stamp, per measured batch after the hold (all measured batches when there was none)"""
    r = json.load(open(path))
    xs = []
    for b, st in sorted(r["state"].items(), key=lambda x: int(x[0])):
        if int(b) > r.get("hold", 0) and st.count("|") >= 5:
            n, t = st.split("|")[5].split("/")
            xs.append(int(n) / max(int(t), 1))
    return xs


def table(paths):
    for p in paths:
        print(json.load(open(p)).get("settings", ""))
        xs = unstamped(p)
        if xs:
            print(f"{p}: entries in segments without a stamp, per measured batch after batch {json.load(open(p)).get('hold', 0)}: mean {100 * statistics.fmean(xs):.1f}%, max {100 * max(xs):.1f}%, last {100 * xs[-1]:.1f}%")
    samples = load(paths)
    print(f"runs={len(paths)}; instructions in millions, blocks per statement; n = samples; other = plans that are not sift")
    print(f"{'class':8s} {'mode':4s} {'n':>3s} {'mean':>8s} {'no spikes':>9s} {'p90':>8s} {'phase-bal med':>13s} {'spikes':>6s} {'hit':>7s} {'read':>6s} {'other':>5s}")
    for q in lab.Q:
        for mode in ("warm", "cold"):
            s = stats(samples, q, mode)
            if s:
                print(f"{q:8s} {mode:4s} {s['n']:3d} {s['mean'] / 1e6:8.2f} {s['tmean'] / 1e6:9.2f} {s['p90'] / 1e6:8.2f} {s['pbm'] / 1e6:13.2f} {s['spikes']:6d} {s['hit']:7.0f} {s['read']:6.0f} {s['other']:5d}")


def gap(paths):
    """per class and mode, over the batches that have a fresh build: the churned index and the fresh one, absolute instructions (millions) and blocks, and the state of the churned index at those batches"""
    rows = collections.defaultdict(lambda: collections.defaultdict(list))
    states = []
    for p in paths:
        r = json.load(open(p))
        fresh_b = sorted({d["b"] for d in r["samples"] if d.get("tab") == "facts_b"})
        for b in fresh_b:
            st = r["state"][str(b)].split("|")
            states.append([int(x) for x in st[:5]] + [int(st[5].split("/")[0]), int(st[5].split("/")[1])])
            for d in r["samples"]:
                if d["b"] == b and d["instr"] > 0:
                    rows[(d["q"], d["mode"])][d.get("tab", "facts")].append(d)
    print(f"runs={len(paths)} batches with a fresh build: {len(states)}; instructions in millions, blocks per statement; gap = churned - fresh")
    if states:
        m = [statistics.fmean(x[i] for x in states) for i in range(7)]
        print(f"churned state at those batches (mean): buffer rows {m[0]:.0f}, segments {m[1]:.1f}, dirty segments {m[2]:.1f}, dead rows {m[3]:.0f}, index {m[4] / 1e6:.1f} MB, entries without a stamp {100 * m[5] / max(m[6], 1):.1f}%")
    print(f"{'class':8s} {'mode':4s} {'n':>3s} {'churned':>8s} {'fresh':>8s} {'gap':>8s} {'gap %':>6s}  {'hit c':>6s} {'hit f':>6s} {'read c':>6s} {'read f':>6s}")
    for q in lab.Q:
        for mode in ("warm", "cold"):
            c, f = rows[(q, mode)]["facts"], rows[(q, mode)]["facts_b"]
            if not c or not f:
                continue
            ci, fi = statistics.fmean(d["instr"] for d in c), statistics.fmean(d["instr"] for d in f)
            print(f"{q:8s} {mode:4s} {len(f):3d} {ci / 1e6:8.2f} {fi / 1e6:8.2f} {(ci - fi) / 1e6:8.2f} {100 * (ci - fi) / fi:6.1f}  "
                  f"{statistics.fmean(d['hit'] for d in c):6.0f} {statistics.fmean(d['hit'] for d in f):6.0f} {statistics.fmean(d['read'] for d in c):6.0f} {statistics.fmean(d['read'] for d in f):6.0f}")


def main(a):
    if a[0] == "run":
        case, work, root, out = a[1:5]
        r = Reads(case, work, root, out)
        try:
            r.go()
            r.close()
        except BaseException:
            r.kill()
            raise
    elif a[0] == "table":
        table(a[1:])
    elif a[0] == "gap":
        gap(a[1:])
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:]) if len(sys.argv) > 2 else sys.exit(__doc__)
