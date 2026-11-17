#!/usr/bin/env python3
"""Ranks the code paths that accumulate memory, from the artifacts of a run (docs/MEMORY-ANALYSIS.md).

  scripts/mem-report.py report DIR... [--baseline PREV.json] [--out report.md] [--json report.json] [--top N]
  scripts/mem-report.py gate DIR...        valgrind logs only: exit 1 for an error or a definite leak with a pg_sift frame (scripts/valgrind.sh)
  scripts/mem-report.py selftest

DIR is searched for the three kinds of artifact:
  *.siftmem      SIFTMEM lines of the memory census (src/core/memcensus.c, collected by scripts/leakcheck.sh leak_harvest)
  vg/*.log, valgrind*.log   valgrind memcheck logs (scripts/valgrind.sh): errors between the VALGRINDERROR markers, leak records
  growth*.csv    allocation sites of massif, case,site,small_kb,large_kb,growth_kb (scripts/massif-slope.sh)
The report is markdown: statements by retained growth and by peak (a statement is its text with the literals removed), tests, valgrind
findings with a pg_sift frame, the sites that grow with the data, and, with --baseline, what grew against the JSON of a previous run.
"""
import argparse, collections, glob, json, os, re, sys

TOP = 15
NOTES = 10
REGRESS_KB = 256          # growth of a path against the baseline that is flagged (and at least REGRESS_PCT)
REGRESS_PCT = 25
SIFT_FRAME = re.compile(r"(/src/(?:agg|bm25|col|core|table)/(?!memcensus\.c)|pg_sift\.so)")
KV = re.compile(r'(\w+)=("[^"]*"|\S+)')


def template(q):
    q = re.sub(r"'[^']*'", "'?'", q.lower())
    q = re.sub(r"\b\d+(\.\d+)?\b", "N", q)
    return re.sub(r"\s+", " ", q).strip()[:80]


def parse_siftmem(path):
    rows = []
    label = os.path.basename(path).split(".")[0]
    for line in open(path, errors="replace"):
        i = line.find("SIFTMEM ")
        if i < 0:
            continue
        kind, _, rest = line[i + 8:].partition(" ")
        kv = {k: v.strip('"') for k, v in KV.findall(rest)}
        for k, v in kv.items():
            if re.fullmatch(r"-?\d+", v):
                kv[k] = int(v)
        kv["kind"], kv["label"] = kind, label
        rows.append(kv)
    return rows


def census(rows):
    paths = collections.defaultdict(lambda: dict(n=0, d_top=0, d_anon=0, max_anon=0, max_hwm=0, sift=0, ms=0, ctx={}, tests=set()))
    tests = collections.defaultdict(lambda: dict(stmts=0, d_top=0, max_anon=0, max_hwm=0, backends=0))
    for r in rows:
        test = r.get("db") if r.get("db") not in (None, "postgres", "") else r["label"]
        if r["kind"] == "stmt":
            p, t = paths[template(r.get("q", ""))], tests[test]
            p["n"] += 1
            p["d_top"] += max(r.get("d_kept_kb", 0), 0)
            p["d_anon"] += max(r.get("d_anon_kb", 0), 0)
            p["max_anon"] = max(p["max_anon"], r.get("d_anon_kb", 0))
            p["max_hwm"] = max(p["max_hwm"], r.get("d_hwm_kb", 0))
            p["sift"] = max(p["sift"], r.get("sift_kb", 0))
            p["ms"] += r.get("ms", 0)
            p["tests"].add(test)
            for c in r.get("ctx", "-").split(","):
                name, _, rest = c.partition(":")
                if rest:
                    p["ctx"][name] = max(p["ctx"].get(name, 0), int(rest.split(":")[1]))
            t["stmts"] += 1
            t["d_top"] += max(r.get("d_kept_kb", 0), 0)
            t["max_anon"] = max(t["max_anon"], r.get("d_anon_kb", 0))
            t["max_hwm"] = max(t["max_hwm"], r.get("d_hwm_kb", 0))
        elif r["kind"] == "exit_peak":
            tests[test]["backends"] += 1
            tests[test]["max_anon"] = max(tests[test]["max_anon"], r.get("max_anon_kb", 0))
    for p in paths.values():
        p["tests"] = len(p["tests"])
        p["top_ctx"] = ", ".join(f"{k} {v} kB" for k, v in sorted(p["ctx"].items(), key=lambda kv: -kv[1])[:3])
        del p["ctx"]
    return paths, tests


def parse_valgrind(path):
    """the blocks between VALGRINDERROR-BEGIN and -END (errors and leak records), each with the first pg_sift frame"""
    found, block, lines = [], None, open(path, errors="replace").read().splitlines()
    for l in lines:
        l = re.sub(r"^==\d+== ", "", l)
        if "VALGRINDERROR-BEGIN" in l:
            block = []
        elif "VALGRINDERROR-END" in l:
            if block:
                found.append(block)
            block = None
        elif block is not None:
            block.append(l)
    out = []
    for b in found:
        frames = [f for f in b if re.search(r"\b(at|by) 0x", f)]
        sift = next((f.strip() for f in frames if SIFT_FRAME.search(f)), None)
        out.append(dict(kind=b[0].strip()[:100], sift_frame=sift, file=os.path.basename(path), leak=bool(re.search(r"lost in loss record", b[0])),
                        definite="definitely lost" in b[0]))
    return out


def gate(dirs):
    """exit 1 for every valgrind error and every definite leak with a pg_sift frame; the other leaks with a pg_sift frame are listed (the first NOTES)"""
    vg = [v for d in dirs for f in sorted(glob.glob(os.path.join(d, "*.log"))) for v in parse_valgrind(f)]
    bad = [v for v in vg if not v["leak"] or (v["definite"] and v["sift_frame"])]
    notes = [v for v in vg if v["leak"] and v not in bad and v["sift_frame"]]
    for v in bad + notes[:NOTES]:
        print(f"valgrind: {'FAIL' if v in bad else 'note'}: {v['kind']}; first pg_sift frame: {v['sift_frame'] or '-'} ({v['file']})")
    print(f"valgrind: {len(vg)} records, {len(bad)} failing, {len(notes)} leaks with a pg_sift frame to read in the report")
    return 1 if bad else 0


def parse_growth(path):
    import csv
    return [dict(case=r["case"], site=r["site"], small_kb=float(r["small_kb"]), large_kb=float(r["large_kb"]), growth_kb=float(r["growth_kb"]))
            for r in csv.DictReader(open(path))]


def collect(dirs):
    rows, vg, growth = [], [], []
    for d in dirs:
        for pat, fn, sink in (("**/*.siftmem", parse_siftmem, rows), ("**/vg/*.log", parse_valgrind, vg), ("**/valgrind*.log", parse_valgrind, vg),
                              ("**/growth*.csv", parse_growth, growth)):
            for f in sorted(set(glob.glob(os.path.join(d, pat), recursive=True))):
                sink.extend(fn(f))
    return rows, vg, growth


def build(dirs):
    rows, vg, growth = collect(dirs)
    paths, tests = census(rows)
    vg_sift = collections.Counter((v["kind"], v["sift_frame"]) for v in vg if v["sift_frame"])
    return dict(paths=paths, tests=tests, valgrind=dict(errors=len(vg), with_sift_frame=[dict(kind=k, frame=f, count=c) for (k, f), c in vg_sift.most_common()]),
                growth=sorted(growth, key=lambda g: -g["growth_kb"]), statements=sum(1 for r in rows if r["kind"] == "stmt"))


def regressions(cur, prev):
    out = []
    for k, p in cur["paths"].items():
        q = prev.get("paths", {}).get(k)
        before = q["d_top"] if q else 0
        if p["d_top"] - before >= REGRESS_KB and (before == 0 or 100 * (p["d_top"] - before) >= REGRESS_PCT * before):
            out.append(dict(path=k, before_kb=before, now_kb=p["d_top"]))
    return sorted(out, key=lambda r: r["before_kb"] - r["now_kb"])


def table(head, rows):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] + ["| " + " | ".join(str(c).replace("|", "/") for c in r) + " |" for r in rows])


def markdown(rep, regress, top):
    m = ["# Memory analysis report", "", f"{rep['statements']} statements over the census threshold, {len(rep['tests'])} tests/databases,"
         f" {rep['valgrind']['errors']} valgrind findings, {len(rep['growth'])} growth sites.", ""]
    by_top = [kv for kv in sorted(rep["paths"].items(), key=lambda kv: -kv[1]["d_top"]) if kv[1]["sift"] > 0][:top]
    m += ["## Statements by retained growth (memory the backend keeps across statements: long-lived contexts, kB summed over the run; statements that touched a sift context)", "",
          table(["#", "statement", "runs", "retained kB", "peak anon kB", "sift ctx kB", "largest sift contexts", "tests"],
                [(i + 1, f"`{k}`", p["n"], p["d_top"], p["max_anon"], p["sift"], p["top_ctx"], p["tests"]) for i, (k, p) in enumerate(by_top)]), ""]
    by_peak = sorted(rep["paths"].items(), key=lambda kv: -max(kv[1]["max_anon"], kv[1]["max_hwm"]))[:top]
    m += ["## Statements by peak (largest single-statement growth of anonymous / peak resident memory, kB)", "",
          table(["#", "statement", "runs", "peak anon kB", "peak rss kB", "ms (sum)"],
                [(i + 1, f"`{k}`", p["n"], p["max_anon"], p["max_hwm"], p["ms"]) for i, (k, p) in enumerate(by_peak)]), ""]
    by_test = sorted(rep["tests"].items(), key=lambda kv: -kv[1]["max_anon"])[:top]
    m += ["## Tests by backend peak", "", table(["test / database", "statements", "retained kB", "max anon kB", "backends"],
          [(k, t["stmts"], t["d_top"], t["max_anon"], t["backends"]) for k, t in by_test]), ""]
    vg = rep["valgrind"]["with_sift_frame"]
    m += ["## Valgrind findings with a pg_sift frame", "", table(["finding", "first pg_sift frame", "count"], [(v["kind"], v["frame"], v["count"]) for v in vg]) if vg else "none.", ""]
    if rep["growth"]:
        m += ["## Allocation sites that grow with the data (massif, peak at the large size minus the small)", "",
              table(["case", "site", "small kB", "large kB", "growth kB"], [(g["case"], g["site"][:90], g["small_kb"], g["large_kb"], g["growth_kb"]) for g in rep["growth"][:top]]), ""]
    if regress is not None:
        m += ["## Against the baseline", "", table(["statement", "before kB", "now kB"], [(f"`{r['path']}`", r["before_kb"], r["now_kb"]) for r in regress]) if regress else "no path grew.", ""]
    return "\n".join(m)


def selftest():
    import tempfile
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR", "/dev/shm")) as d:
        os.makedirs(f"{d}/vg")
        open(f"{d}/t1.1.siftmem", "w").write(
            '2026-01-01 [7] LOG:  SIFTMEM stmt pid=7 db=case_a ms=5 anon_kb=9000 d_anon_kb=4000 d_hwm_kb=4096 top_kb=900 d_top_kb=300 kept_kb=900 d_kept_kb=300 cache_kb=100 d_cache_kb=0 sift_kb=250 ctx=sift_scan:2:200 q="select sift.merge(\'t_col\'::regclass, true)"\n'
            '2026-01-01 [7] LOG:  SIFTMEM stmt pid=7 db=case_a ms=5 anon_kb=9000 d_anon_kb=10 d_hwm_kb=0 top_kb=900 d_top_kb=-50 kept_kb=900 d_kept_kb=-50 cache_kb=100 d_cache_kb=0 sift_kb=0 ctx=- q="select 1"\n'
            '2026-01-01 [7] LOG:  SIFTMEM exit_peak pid=7 db=case_a statements=2 max_anon_kb=9000 max_hwm_kb=12000\n')
        open(f"{d}/vg/1.log", "w").write(
            "==1== VALGRINDERROR-BEGIN\n==1== Invalid read of size 4\n==1==    at 0x1: memcpy (vg.c:1)\n==1==    by 0x2: merge_run (/w/src/col/maint/merge.c:10)\n==1== VALGRINDERROR-END\n"
            "==1== VALGRINDERROR-BEGIN\n==1== 100 bytes in 1 blocks are definitely lost in loss record 5 of 9\n==1==    at 0x3: malloc (vg.c:2)\n==1==    by 0x4: pg_strdup (x.c:3)\n==1== VALGRINDERROR-END\n")
        open(f"{d}/growth_merge.csv", "w").write("case,site,small_kb,large_kb,growth_kb\nmerge,seg_write (src/col/seg_write/add.c:40),100,900,800\n")
        rep = build([d])
        assert rep["statements"] == 2 and rep["valgrind"]["errors"] == 2, rep["valgrind"]
        assert rep["valgrind"]["with_sift_frame"][0]["frame"].endswith("merge.c:10)"), rep["valgrind"]
        path = "select sift.merge('t_col'::regclass, true)"
        assert template(path) == "select sift.merge('?'::regclass, true)" and rep["paths"][template(path)]["d_top"] == 300
        assert rep["tests"]["case_a"]["max_anon"] == 9000 and rep["growth"][0]["growth_kb"] == 800
        prev = json.loads(json.dumps(rep))
        prev["paths"][template(path)]["d_top"] = 10
        assert regressions(rep, prev) and not regressions(rep, json.loads(json.dumps(rep)))
        assert gate([f"{d}/vg"]) == 1
        md = markdown(rep, regressions(rep, prev), TOP)
        assert "sift_scan 200 kB" in md and "merge.c:10" in md and "seg_write" in md, md
    print("mem-report selftest: ok")


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("dirs", nargs="+")
    r.add_argument("--baseline")
    r.add_argument("--out")
    r.add_argument("--json")
    r.add_argument("--top", type=int, default=TOP)
    sub.add_parser("selftest")
    sub.add_parser("gate").add_argument("dirs", nargs="+")
    a = ap.parse_args()
    if a.cmd == "gate":
        sys.exit(gate(a.dirs))
    if a.cmd == "selftest":
        return selftest()
    rep = build(a.dirs)
    regress = regressions(rep, json.load(open(a.baseline))) if a.baseline else None
    md = markdown(rep, regress, a.top)
    open(a.out, "w").write(md + "\n") if a.out else print(md)
    if a.json:
        json.dump(rep, open(a.json, "w"), indent=1, default=list)


if __name__ == "__main__":
    main()
