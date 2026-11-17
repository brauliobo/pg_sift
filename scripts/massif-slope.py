#!/usr/bin/env python3
"""Massif on the slope cases: the allocation sites that make an operation hold more memory on more rows (docs/MEMORY-ANALYSIS.md).

  scripts/massif-slope.py run --data DIR --sock DIR --port N --out DIR [--rows N] [--only RE]
  scripts/massif-slope.py selftest

Run by scripts/valgrind.sh --massif (the server and valgrind of that lane). For each case of CASES and each of two sizes (--rows, and four times that), a data directory
holds the tables of scripts/mem-slope.py; a copy of it is started natively for the setup of the case, then under `valgrind --tool=massif`, one backend runs the operation
(work_mem and maintenance_work_mem at 1 MB, as the slope gate), and the peak snapshot of that backend is read: the bytes of every allocation site, the site being the
innermost pg_sift frame of the stack (a palloc of an extension is one with its caller, the allocators of PostgreSQL are --alloc-fn). Written to --out:
  NAME.ROWS.massif.out, NAME.ROWS.txt (ms_print)   the raw profile and its text
  growth_NAME.csv                                   case,site,small_kb,large_kb,growth_kb: the peak at the large size minus the peak at the small, by site, largest first
"""
import csv, importlib.util, os, re, shutil, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOCATORS = ["palloc", "palloc0", "palloc_extended", "repalloc", "repalloc_huge", "MemoryContextAlloc", "MemoryContextAllocZero", "MemoryContextAllocHuge",
              "MemoryContextAllocExtended", "AllocSetAlloc", "AllocSetAllocLarge", "AllocSetAllocFromNewBlock", "AllocSetRealloc", "GenerationAlloc", "SlabAlloc",
              "pnstrdup", "pstrdup", "psprintf", "datumCopy"]
SIFT_FRAME = re.compile(r"/src/(agg|bm25|col|core|table)/")
CASES = ["merge", "lookup_join_label", "count_distinct_text", "table_update_all", "scoped_cic_3"]
EXTRA = {   # a case that is not in test/mem-slope.tsv: name -> (setup, operation); {m} a copy of the table, {n} its rows
    "scoped_cic_3": ("drop table if exists p{n}; create table p{n} (member bigint not null, tenant int not null);"
                     "insert into p{n} select g, t from generate_series(1, {n}) g, generate_series(1, 3) t;"
                     "create index p{n}_j on p{n} (member, tenant);",
                     "set maintenance_work_mem = '4MB';\n"
                     "create index concurrently {m}_sc on {m} using sift_col (id, k_mid, amount) with (scope = 'tenant: id -> p{n}(member, tenant)');"),
}
NODE = re.compile(r"^( *)n(\d+): (\d+) (.*)$")


def load_slope():
    spec = importlib.util.spec_from_file_location("mem_slope", os.path.join(HERE, "mem-slope.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def parse_peak(text):
    """massif.out text -> {site: bytes} of the peak snapshot"""
    lines = text.splitlines()
    start = next((i for i, l in enumerate(lines) if l.strip() == "heap_tree=peak"), None)
    return {} if start is None else attribute(lines[start + 1:])


def attribute(lines):
    nodes = []
    for l in lines:
        if l.startswith("snapshot="):
            break
        m = NODE.match(l)
        if m:
            nodes.append((len(m.group(1)), int(m.group(3)), re.sub(r"^0x[0-9A-Fa-f]+: ", "", m.group(4))))
    sites = {}
    path = []
    for i, (depth, size, label) in enumerate(nodes):
        del path[depth:]
        path.append(label)
        kids = 0
        for d2, s2, _ in nodes[i + 1:]:
            if d2 <= depth:
                break
            if d2 == depth + 1:
                kids += s2
        own = size - kids
        if own > 0 and depth > 0:
            site = next((f for f in path[1:] if SIFT_FRAME.search(f)), None) or "(postgres) " + (path[1] if len(path) > 1 else path[0])
            site = re.sub(r"\s+", " ", site).replace(",", ";")
            sites[site] = sites.get(site, 0) + own
    return sites


def growth(small, large):
    rows = [(s, small.get(s, 0) / 1024.0, large.get(s, 0) / 1024.0) for s in set(small) | set(large)]
    return sorted(((s, a, b, b - a) for s, a, b in rows), key=lambda r: -r[3])


def pg_ctl(data, *args):
    return subprocess.run(["pg_ctl", "-D", data, *args], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def ready(sock, port, secs):
    for _ in range(secs):
        if subprocess.run(["pg_isready", "-q", "-h", sock, "-p", str(port)]).returncode == 0:
            return True
        time.sleep(1)
    return False


def native(data, fn):
    pg_ctl(data, "-w", "-t", "300", "start")
    try:
        fn()
    finally:
        pg_ctl(data, "-m", "fast", "-w", "-t", "300", "stop")


def massif_server(data, out, name):
    vg = ["valgrind", "--quiet", "--tool=massif", "--trace-children=yes", f"--massif-out-file={out}/{name}.%p.massif", "--fullpath-after=", "--time-unit=B",
          "--threshold=0.2", "--depth=30", "--max-snapshots=200", "--peak-inaccuracy=0.5"] + [f"--alloc-fn={a}" for a in ALLOCATORS]
    return subprocess.Popen(["nice", *vg, "postgres", "-D", data], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)


def run_case(ms, args, name, setup, op, n, tag):
    work = f"{args.data}.{tag}"
    shutil.rmtree(work, ignore_errors=True)
    subprocess.run(["cp", "-a", args.data, work], check=True)
    fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", s=f"s{n}", n=n)

    def prepare():
        if "{m}" in setup + op or "{s}" in setup + op:
            ms.copy_table(n)
        ms.script(setup.format(**fmt))
    native(work, prepare)
    srv = massif_server(work, args.out, f"{name}.{n}")
    try:
        if not ready(args.sock, args.port, 1800):
            raise RuntimeError("the server did not start under massif")
        p = ms.psql()
        p.stdin.write("select 'PID', pg_backend_pid();\n")
        line = p.stdout.readline()
        while "PID" not in line:
            line = p.stdout.readline()
        pid = int(line.strip().split("|")[1])
        p.stdin.write(ms.SETTINGS + op.format(**fmt) + "\n\\echo OPDONE\n")
        err = None
        for l in iter(p.stdout.readline, ""):
            if "OPDONE" in l:
                break
            err = err or ("ERROR" in l and l.strip() or None)
        p.stdin.write("\\q\n")
        p.wait()
        if err:
            raise RuntimeError("operation: " + err)
    finally:
        pg_ctl(work, "-m", "fast", "-w", "-t", "1800", "stop")
        srv.wait(timeout=600)
    out = os.path.join(args.out, f"{name}.{n}.{pid}.massif")
    final = os.path.join(args.out, f"{name}.{n}.massif.out")
    for _ in range(60):
        if os.path.exists(out) and os.path.getsize(out):
            break
        time.sleep(1)
    shutil.move(out, final)
    for stale in os.listdir(args.out):
        if stale.startswith(f"{name}.{n}.") and stale.endswith(".massif"):
            os.unlink(os.path.join(args.out, stale))
    txt = subprocess.run(["ms_print", final], capture_output=True, text=True).stdout
    open(os.path.join(args.out, f"{name}.{n}.txt"), "w").write(txt)
    shutil.rmtree(work, ignore_errors=True)
    return parse_peak(open(final, errors="replace").read())


def run(args):
    ms = load_slope()
    os.environ.update(PGHOST=args.sock, PGPORT=str(args.port), PGUSER="postgres", PGDATABASE="postgres")
    os.makedirs(args.out, exist_ok=True)
    sizes = [args.rows, args.rows * 4]
    cases = {c[0]: c for c in ms.read_cases(None)}
    todo = [c for c in CASES if re.search(args.only or "", c)]
    native(args.data, lambda: (ms.script("create extension if not exists pg_sift;"), [ms.build_tables(n) for n in sizes]))
    for name in todo:
        setup, op = EXTRA[name] if name in EXTRA else (cases[name][4], cases[name][5])
        peaks = [run_case(ms, args, name, setup, op, n, f"{name}{n}") for n in sizes]
        rows = growth(*peaks)
        with open(os.path.join(args.out, f"growth_{name}.csv"), "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["case", "site", "small_kb", "large_kb", "growth_kb"])
            for site, a, b, g in rows:
                w.writerow([name, site, f"{a:.1f}", f"{b:.1f}", f"{g:.1f}"])
        print(f"massif {name}: peak {sum(peaks[0].values()) / 1048576:.1f} MB at {sizes[0]} rows, {sum(peaks[1].values()) / 1048576:.1f} MB at {sizes[1]};"
              f" top site {rows[0][0] if rows else '-'} +{rows[0][3] / 1024 if rows else 0:.1f} MB", flush=True)
    return 0


def selftest():
    text = """snapshot=3
#-----------
time=100
mem_heap_B=3000
heap_tree=peak
n2: 3000 (heap allocation functions) malloc/new/new[], --alloc-fns, etc.
 n1: 2000 0x1: AllocSetAllocLarge (aset.c:1)
  n1: 2000 0x2: seg_buf (/w/src/col/seg_write/add.c:77)
   n0: 2000 0x3: ExecutorRun (execMain.c:300)
 n0: 1000 0x4: lookup_load (/w/src/agg/lookup/load.c:12)
snapshot=4
heap_tree=empty
"""
    sites = attribute(text.splitlines()[5:])
    assert sites == {"seg_buf (/w/src/col/seg_write/add.c:77)": 2000, "lookup_load (/w/src/agg/lookup/load.c:12)": 1000}, sites
    assert parse_peak(text) == sites
    assert growth({"a": 1024}, {"a": 4096, "b": 1024})[0][:1] == ("a",)
    print("massif-slope selftest: ok")
    return 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["selftest"]:
        sys.exit(selftest())
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["run"])
    ap.add_argument("--data", required=True)
    ap.add_argument("--sock", required=True)
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--rows", type=int, default=50000)
    ap.add_argument("--only")
    sys.exit(run(ap.parse_args()))
