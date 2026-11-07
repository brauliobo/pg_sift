#!/usr/bin/env python3
"""run.py [--only GLOB] [--update] [--no-plans] [--keys N] [--out DIR]   (scripts/corpus.sh starts the cluster and loads test/corpus/lab.sql)

Every statement of test/corpus/cases.sql runs in a NEW backend (psql \\connect) twice, with sift off (the native planner) and on, and the sorted rows must be equal. In the
backend of the "on" run the plan is then explained: the Custom Scan (Sift...) nodes it holds and the verdict of sift.explain_why per arm (used, or the slug of the refusal). That
is the plan slug of the statement; test/corpus/plans.tsv pins it, so a statement that stops being engaged (or starts) fails until the file is updated in the same change
(--update rewrites it: read the diff). Libpq environment as for psql (PGHOST, PGPORT, PGUSER, PGDATABASE). Exit 1 on a difference, an error or a plan that is not the pinned one.
"""
import fnmatch, hashlib, os, re, subprocess, sys, tempfile, threading

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = "set statement_timeout='90s'; set extra_float_digits=-5; set max_parallel_workers_per_gather=0; set jit=off; set work_mem='256MB'; set sift.col_auto_merge=off;"
OFF = ("set sift.enable_agg=off; set sift.enable_col_scan=off; set sift.enable_scan=off; set sift.enable_topn=off; set sift.enable_count=off;")
ON = "set sift.enable_agg=on;" + os.environ.get("CORPUS_ON_SET", "")


def parse(path):
    cases = []
    for blk in re.split(r"(?m)^-- @case ", open(path).read())[1:]:
        head, _, rest = blk.partition("\n")
        slug, fam, *note = [x.strip() for x in head.split("|")]
        pre, txn, body = [], False, []
        for line in rest.splitlines():
            if line.startswith("-- @pre "):
                pre.append(line[8:])
            elif line.startswith("-- @txn"):
                txn = True
            elif line.startswith("--") and not body:
                continue
            else:
                body.append(line)
        cases.append(dict(slug=slug, fam=fam, pre=pre, txn=txn, sql="\n".join(body).strip().rstrip(";").strip()))
    return cases


def keys(slug):
    h = hashlib.sha256(slug.encode()).digest()
    return 1 + int(2000 * (h[0] / 255) ** 2.5), 1 + int(2000 * (h[1] / 255) ** 2.5)


def statement(c):
    k, k2 = keys(c["slug"])
    return c["sql"].replace("@K@", str(k)).replace("@K2@", str(k2))


def script(cases, mode, out):
    """one psql script: a new connection for each case; the rows of case i in out/MODE.i, the plan of the on run in out/plan.i, an error as a line @@ERR"""
    s = ["\\set ON_ERROR_STOP off"]
    for i, c in enumerate(cases):
        st = statement(c)
        s += ["\\connect", BASE + (OFF if mode == "off" else ON)] + [q if q.rstrip().endswith(";") else q + ";" for q in c["pre"]]
        s.append("\\o %s/%s.%d" % (out, mode, i))
        s.append("begin;" if c["txn"] else "")
        s.append(st + ";")
        s += ["\\if :ERROR", "\\o", "\\echo @@ERR %d :LAST_ERROR_MESSAGE" % i, "\\endif", "\\o"]
        if c["txn"]:
            s.append("rollback;")
        if mode == "on":
            s.append("\\o %s/plan.%d" % (out, i))
            q = ("explain (costs off) " + st) if not st.lower().startswith("execute") else None
            if q:
                s.append("begin;" if c["txn"] else "")
                s.append(q + ";")
                if c["txn"]:
                    s.append("rollback;")
                s.append("select string_agg(arm || ':' || case when used then 't' else 'f:' || coalesce(slug, '') end, ',' order by arm) from sift.explain_why($q$%s$q$);" % st)
            else:
                s.append("select 'n/a(prepared)';")
            s.append("\\o")
    return "\n".join(s) + "\n"


def run(cases, mode, out, errs):
    p = subprocess.run(["psql", "-X", "-q", "-At", "-v", "ON_ERROR_STOP=0"], input=script(cases, mode, out), capture_output=True, text=True)
    for m in re.finditer(r"@@ERR (\d+) (.*)", p.stdout):
        errs[(mode, int(m.group(1)))] = m.group(2)[:160]
    if p.returncode not in (0, 2) and not errs:
        errs[(mode, -1)] = p.stderr.strip()[:300]


def rows(path):
    if not os.path.exists(path):
        return []
    return sorted(re.sub(r"\s+$", "", l) for l in open(path).read().splitlines() if l.strip())


def plan_of(path):
    t = open(path).read() if os.path.exists(path) else ""
    nodes = sorted(set(re.findall(r"Custom Scan \((Sift\w*)\)", t)))
    why = ""
    for l in t.splitlines():
        if re.match(r"^[a-z_]+:[tf]", l) or l.startswith("n/a"):
            why = l.strip()
    return ",".join(nodes) or "-", why or "-"


def main(a):
    only = a[a.index("--only") + 1] if "--only" in a else "*"
    update, noplans = "--update" in a, "--no-plans" in a
    cases = [c for c in parse(os.path.join(HERE, "cases.sql")) if fnmatch.fnmatch(c["slug"], only)]
    out = a[a.index("--out") + 1] if "--out" in a else tempfile.mkdtemp(prefix="corpus.", dir=os.environ.get("TMPDIR", "/tmp"))
    os.makedirs(out, exist_ok=True)
    errs = {}
    th = [threading.Thread(target=run, args=(cases, m, out, errs)) for m in ("off", "on")]
    [t.start() for t in th]
    [t.join() for t in th]
    pinned = {}
    pf = os.path.join(HERE, "plans.tsv")
    if os.path.exists(pf):
        for l in open(pf).read().splitlines():
            if l and not l.startswith("#"):
                s, n, w = l.split("\t")
                pinned[s] = (n, w)
    bad, plans = [], {}
    for i, c in enumerate(cases):
        s = c["slug"]
        if (("off", i) in errs) or (("on", i) in errs):
            bad.append("%s: ERROR off=%s on=%s" % (s, errs.get(("off", i), "-"), errs.get(("on", i), "-")))
            continue
        ro, rn = rows(os.path.join(out, "off.%d" % i)), rows(os.path.join(out, "on.%d" % i))
        if ro != rn:
            bad.append("%s: ROWS differ (%d off, %d on): %s" % (s, len(ro), len(rn), " || ".join(x[:60] for x in [l for l in rn if l not in set(ro)][:2] + [l for l in ro if l not in set(rn)][:2])))
        plans[s] = plan_of(os.path.join(out, "plan.%d" % i))
        if not noplans and not update and pinned.get(s, ("",))[0] == "~":
            continue
        if not noplans and not update and s in pinned and pinned[s] != plans[s]:
            bad.append("%s: PLAN changed: pinned %s [%s], now %s [%s] (update test/corpus/plans.tsv in the same change if it is meant)" % (s, pinned[s][0], pinned[s][1], plans[s][0], plans[s][1]))
        elif not noplans and not update and s not in pinned:
            bad.append("%s: no pinned plan (scripts/corpus.sh --update records it)" % s)
    if update and only == "*":
        with open(pf, "w") as f:
            f.write("# slug\tSift nodes of the plan\tsift.explain_why per arm (t: used, f:SLUG: refused); pinned by test/corpus/run.py, rewritten by scripts/corpus.sh --update; a node ~ leaves a cost-borderline case unpinned (rows are still compared), --update keeps it\n")
            for c in parse(os.path.join(HERE, "cases.sql")):
                if c["slug"] in plans:
                    f.write("%s\t%s\t%s\n" % (c["slug"], *(("~", "cost-borderline across majors") if pinned.get(c["slug"], ("",))[0] == "~" else plans[c["slug"]])))
    elif update:
        print("corpus: --update with --only changes nothing (the whole corpus is recorded together)")
    engaged = sum(1 for n, w in plans.values() if n != "-")
    for b in bad:
        print("corpus: " + b)
    print("corpus: %d statements, %d engaged, %d problems" % (len(cases), engaged, len(bad)))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
