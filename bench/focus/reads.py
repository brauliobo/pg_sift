#!/usr/bin/env python3
"""reads.py: the read cost of a churned sift_col index at small size, per class, warm and cold.

  reads.py run CASE WORK ROOT OUT    CASE std | wide; WORK holds the base heap (lab.py base); ROOT = the staged install under test; writes OUT.json, OUT.txt
  reads.py table OUT.json...         per class: mean, p90, phase-balanced median of the warm instructions, blocks, cold figures (runs pooled)
  env: FOCUS_READ_BATCHES (54), FOCUS_READ_FROM (13: the first batch measured), FOCUS_COLD_EVERY (7)

The churn is the one of wamp.py (INSERT / UPDATE / soft DELETE, VACUUM every 6th batch, workers on, every statement waited for). From the first measured
batch on, each batch ends with the ten classes of lab.Q on the churned index: instructions of the backend (perf_event_open), shared blocks hit / read, plan.
The key window of a measured batch moves by delta(b) = b * 53 % 400, the same for every build, so no run depends on where a partition cut falls against fixed keys.
Warm = the second run in a fresh backend. Cold = the first statement of a fresh backend after the relations were evicted, on every 7th batch (7 is coprime
with the 6 batches of the write-buffer cycle, so the cold samples meet every phase). Phase = the batch number mod 6; the batches with a VACUUM (phase 0) are a phase of their own.
"""
import collections, json, os, re, statistics, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lab

BATCHES = int(os.environ.get("FOCUS_READ_BATCHES", "54"))
FROM = int(os.environ.get("FOCUS_READ_FROM", "13"))
COLD_EVERY = int(os.environ.get("FOCUS_COLD_EVERY", "7"))
SAMPLE_GRACE = 0.1
EVICT = ("select pg_buffercache_evict_relation(c.oid) from pg_class c where relname in ('facts', 'facts_sift', 'facts_pkey') "
         "or relname like 'pg_toast_%'")
STATE = "select buffer_rows || '|' || segments || '|' || dirty_segments || '|' || dead_rows || '|' || index_bytes from sift.col_index_status"


def parse(txt):
    m = re.search(r"Buffers: shared (hit=(\d+))?\s*(read=(\d+))?", txt)
    ex = re.search(r"Execution Time: ([\d.]+)", txt)
    return {"hit": int(m.group(2) or 0) if m else 0, "read": int(m.group(4) or 0) if m else 0, "ms": float(ex.group(1)) if ex else -1.0,
            "plan": "sift" if re.search(r"Sift|sift", txt) else "other"}


class Reads(lab.Lab):
    def sample(s, b, q, cold):
        """instructions, blocks and plan of EXPLAIN (ANALYZE, BUFFERS) of class q in a fresh backend"""
        sql = lab.rotate(lab.Q[q], lab.delta(b))
        se = lab.Sess(s.cl, ["set max_parallel_workers_per_gather = 0", "set sift.enable_agg = on"], err=s.err)
        if q == "pointx":
            se.ask("set enable_indexscan = off"); se.ask("set enable_bitmapscan = off")
        se.ask(EVICT) if cold else se.ask(sql)
        out, ins = se.counted("explain (analyze, buffers, timing off, summary on) " + sql)
        se.close()
        return dict(parse("\n".join(out)), b=b, q=q, mode="cold" if cold else "warm", instr=ins)

    def go(s):
        s.setup()
        s.cl.psql("create extension if not exists pg_buffercache")
        s.rep(f"# case={s.case} rows={lab.ROWS} n={lab.N} batches={BATCHES} from={FROM} cold_every={COLD_EVERY}")
        res = {"settings": s.settings, "case": s.case, "n": lab.N, "rows": lab.ROWS, "from": FROM, "samples": [], "state": {}}
        w, rd = s.sessions()

        def run(kind, fn):
            fn(); s.idle(kind)
        for b in range(1, BATCHES + 1):
            s.batch(w, rd, b, run)
            if b < FROM:
                s.rep(f"batch {b:3d} {s.cl.status()}")
                continue
            cold = (b - FROM) % COLD_EVERY == 0
            if cold:
                s.cl.psql("checkpoint")
            res["state"][b] = s.ctl.ask(STATE)[0]
            for c in ((True, False) if cold else (False,)):
                for q in lab.Q:
                    res["samples"].append(s.sample(b, q, c))
                    s.idle(grace=SAMPLE_GRACE)   # a read can ask for maintenance: it ends before the next class, so every build meets the same state
            warm = {x["q"]: x["instr"] for x in res["samples"] if x["b"] == b and x["mode"] == "warm"}
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
    x = [d for d in samples if d["q"] == q and d["mode"] == mode and d["instr"] > 0]
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


def table(paths):
    for p in paths:
        print(json.load(open(p)).get("settings", ""))
    samples = load(paths)
    print(f"runs={len(paths)}; instructions in millions, blocks per statement; n = samples; other = plans that are not sift")
    print(f"{'class':8s} {'mode':4s} {'n':>3s} {'mean':>8s} {'no spikes':>9s} {'p90':>8s} {'phase-bal med':>13s} {'spikes':>6s} {'hit':>7s} {'read':>6s} {'other':>5s}")
    for q in lab.Q:
        for mode in ("warm", "cold"):
            s = stats(samples, q, mode)
            if s:
                print(f"{q:8s} {mode:4s} {s['n']:3d} {s['mean'] / 1e6:8.2f} {s['tmean'] / 1e6:9.2f} {s['p90'] / 1e6:8.2f} {s['pbm'] / 1e6:13.2f} {s['spikes']:6d} {s['hit']:7.0f} {s['read']:6.0f} {s['other']:5d}")


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
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:]) if len(sys.argv) > 2 else sys.exit(__doc__)
