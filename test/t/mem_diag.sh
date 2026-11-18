#!/usr/bin/env bash
# Diagnostic of the resident peak of two slope cases: the sampled anonymous peak against the kernel's high-water mark of one backend, run after run.
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
python3 -I - "$REPO" <<'PY' > "$(dir dg)/diag.out" 2>&1
import importlib.util, sys, re, os, threading
spec = importlib.util.spec_from_file_location("ms", sys.argv[1] + "/scripts/mem-slope.py")
ms = importlib.util.module_from_spec(spec); spec.loader.exec_module(ms)
sizes = [30000, 60000, 120000]
ms.build_all(sizes)
print("SERVER", ms.script("select version();").strip())

def status(pid):
    d = {}
    for l in open(f"/proc/{pid}/status"):
        k, _, v = l.partition(":")
        if k in ("VmRSS", "VmHWM", "RssAnon", "RssFile", "RssShmem"): d[k] = int(v.split()[0])
    return d

def run(setup, op):
    ms.prepare(setup)
    p = ms.psql()
    out, _ = ms.send(p, "select 'PID', pg_backend_pid();", "PIDDONE")
    pid = int(next(l for l in out if "PID" in l).strip().split("|")[1])
    ms.send(p, ms.SETTINGS + "select sift._test_thp_disable();\nselect sift._test_malloc_stats(true);", "W")
    statm = os.open(f"/proc/{pid}/statm", os.O_RDONLY)
    b = status(pid)
    open(f"/proc/{pid}/clear_refs", "w").write("5")
    b2 = status(pid)
    base = ms.anon_kb_fd(statm)
    peak, stop = [base], threading.Event()
    def sample():
        while not stop.is_set(): peak[0] = max(peak[0], ms.anon_kb_fd(statm))
    t = threading.Thread(target=sample); t.start()
    ms.send(p, op, "OPDONE"); stop.set(); t.join()
    e = status(pid)
    p.stdin.write("\\q\n"); p.wait(); os.close(statm)
    return dict(sampled=peak[0] - base, hwm=e["VmHWM"] - b2["VmRSS"], anon_end=e["RssAnon"] - b["RssAnon"], file_end=e["RssFile"] - b["RssFile"], shm_end=e["RssShmem"] - b["RssShmem"], hwm_reset=b2["VmHWM"] - b2["VmRSS"])

cases = {"group_mid": ("", "select k_mid, count(*), sum(amount) from f{n} group by 1;"), "table_scan": ("create table s{n} using sift_table as select * from f{n} order by id;", "set sift.col_dict_shared_cache_kb = 0; select count(*), sum(length(txt)) from s{n};")}
for name, (setup, op) in cases.items():
    for n in sizes:
        res = [run(setup.format(n=n), op.format(n=n)) for _ in range(int(os.environ.get("REPS", "12")))]
        for key in ("sampled", "hwm", "anon_end", "file_end", "shm_end", "hwm_reset"):
            vals = sorted(r[key] for r in res)
            print("RUNS", name, n, key, "kB:", " ".join(str(v) for v in vals))
PY
sed 's/^/# /' "$(dir dg)/diag.out"
not_ok "diagnostic output above"
finish
