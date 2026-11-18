#!/usr/bin/env bash
# Diagnostic of two slope rows on a major: the plan of the hash aggregate at three sizes and the exact peaks of the memory scopes of a scoped build.
. "$(dirname "$0")/lib.bash"
REPO=$(cd "$(dirname "$0")/../.." && pwd)

node_init dg 36 "autovacuum = off
max_parallel_workers_per_gather = 0
shared_buffers = 64MB
max_wal_size = 1GB
sift.col_auto_merge = off"
node_start dg || bail "start failed"
sql dg "CREATE EXTENSION pg_sift" "CREATE EXTENSION pg_sift_test" >/dev/null || bail "extension failed"
plan 1
export PGHOST=$(dir dg)/sock PGPORT=$(port dg) PGUSER=postgres PGDATABASE=postgres
python3 -I - "$REPO" "$(dir dg)" <<'PY' > "$(dir dg)/diag.out" 2>&1
import importlib.util, sys, re, subprocess, os, glob
spec = importlib.util.spec_from_file_location("ms", sys.argv[1] + "/scripts/mem-slope.py")
ms = importlib.util.module_from_spec(spec); spec.loader.exec_module(ms)
sizes = [50000, 100000, 200000]
ms.build_all(sizes)
print("SERVER", ms.script("select version();").strip())
for n in sizes:
    out = ms.script(ms.SETTINGS + f"explain (analyze, costs on, summary off) select k_hi, count(*), sum(amount) from f{n} group by 1;")
    print("PLAN", n, " | ".join(l.strip() for l in out.splitlines() if re.search(r"Aggregate|Batches|Planned|Memory|Disk|rows=", l)))
cases = {c[0]: c for c in ms.read_cases(r"^(group_unique_int|scoped_cic)$")}
log = glob.glob(sys.argv[2] + "/postgres.log") + glob.glob(sys.argv[2] + "/*.log")
logf = log[0]
for name in ("group_unique_int", "scoped_cic"):
    c = cases[name]
    for n in sizes:
        fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", s=f"s{n}", n=n)
        for run in range(3):
            if ms.uses_copy(c[4], c[6], c[5]): ms.copy_table(n)
            pre = os.path.getsize(logf)
            mb = ms.measure_mb(c[4].format(**fmt), c[6] if c[6] == "=" else c[6].format(**fmt), c[5].format(**fmt))
            with open(logf, "rb") as fh:
                fh.seek(pre); new = fh.read().decode(errors="replace")
            scopes = " ".join(f"{m[0]}={int(m[1])//1024}k" for m in re.findall(r"SIFTMEM_SCOPE name=(\S+) peak=(\d+)", new))
            print("MEASURE", name, n, run, f"{mb:.1f} MB", scopes)
PY
cat "$(dir dg)/diag.out" | sed 's/^/# /'
not_ok "diagnostic output above"
finish
