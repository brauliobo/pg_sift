#!/usr/bin/env python3
"""The memory slope gate: what an operation holds against the rows of its table, measured at 3 sizes in fresh backends (test/mem-slope.tsv).

PostgreSQL's sorts, hashes and aggregates honour work_mem and its maintenance honours maintenance_work_mem, so the memory of a statement does not follow the table.
Every measurement is a backend of its own, on tables of N, 2N and 4N rows with work_mem and maintenance_work_mem at 1 MB: the SETUP of the case runs in a session that
disconnects (what the operation reads is what a fresh backend finds), the measured backend connects, runs the WARM statements the case declares, returns what the C
library can give back (sift._test_malloc_stats(true)) and has the backend sample its own anonymous memory every 100 microseconds (sift._test_rss_peak_start: a timer signal in the backend, so
no other process decides which part of a statement is seen); the peak of that memory (the minimum of RUNS runs) above that baseline is the cost of the operation at that size. The slope in bytes a row is the difference of the peaks of the largest and the smallest size
over the difference of the rows, so no size depends on the one measured before it and no case on the one that ran before it (--shard and --reverse give the same slopes).
A case passes when the slope is at most its ceiling. Status `clean`: the ceiling is the noise (CLEAN) and three runs stayed under it with margin. Status `open`: a case that
grows with the data today has the largest of three runs plus 15 percent as its ceiling (a defect to fix, and its row goes to clean with the fix); a case whose slope is far under
its open ceiling fails too, so that a fix is not left unrecorded. Status `noise`: the slope moves from run to run with where the allocator places its blocks (or has no sign);
the ceiling is the largest of three runs plus 15 percent and there is no floor. An open or noise row says why in its REASON column.

  scripts/mem-slope.py run [--rows N] [--only RE] [--shard I/N] [--reverse]   one SLOPE line a case: SLOPE name kind bytes/row ceiling status PASS|FAIL, and TIME name seconds
  scripts/mem-slope.py list [--only RE] [--shard I/N]                          the names of the cases of a shard (a case is in shard I when crc32(name) % N == I-1)
  scripts/mem-slope.py determinism [--rows N] --only RE                        the cases run forward, forward again and in reverse: no slope of the memory scopes of sift (exact bytes, not sampled) differs by more than max(2 B/row, 15%)
  scripts/mem-slope.py compare FILE FILE                                       the SLOPE lines of two runs, the same rule
  scripts/mem-slope.py selftest

Kind `plateau`: a case whose memory rises to a bound within the first sizes and holds it (the VACUUM of a sift_table rises 80 B/row to 100,000 rows and then stays at 10.3 to 10.9 MB), measured by the slope between the
second and the third size: the slope over all three is where the bend falls, which moves with the allocator and the host (15 to 20 B/row on x86, 26 on arm64, for 4 B/row beyond the bend).

TSV columns: NAME KIND CEILING STATUS SETUP OPERATION [WARM] [REASON]; {f} {d} the tables of the case, {m} {s} a copy it may change, {n} its rows. Reads PGHOST, PGPORT,
PGUSER, PGDATABASE like psql, MEM_SLOPE_MWM (maintenance_work_mem, 1MB), MEM_SLOPE_RUNS (2), MEM_SLOPE_SHARD (I/N) and MEM_SLOPE_TSV (another table of cases); needs Linux (/proc) and an installed pg_sift and
pg_sift_test. Kind `leak` repeats a statement in one backend and asserts that the memory its contexts hold (pg_backend_memory_contexts) does not follow the number of statements.
"""
import contextlib, io, os, re, subprocess, sys, time, zlib

CLEAN = 12                 # bytes a row: the noise of a 150,000-row difference is 1.8 MB
LEAK_KB = 512              # kB the contexts of a backend may hold more after 60 repeats than after 10
VARIANCE_ROWS, VARIANCE_PCT = 2.0, 0.15   # two runs of a case differ by at most max(2 B/row, 15%)
RUNS = int(os.environ.get("MEM_SLOPE_RUNS", "2"))
ASYNC_OFF = "set sift.async_maintenance='off';\n"
SETTINGS = ASYNC_OFF + f"set work_mem='1MB'; set maintenance_work_mem='{os.environ.get('MEM_SLOPE_MWM', '1MB')}'; set max_parallel_workers_per_gather=0; set max_parallel_maintenance_workers=0;\n"
TSV = os.environ.get("MEM_SLOPE_TSV") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "mem-slope.tsv")
STATUSES, KINDS = ("clean", "open", "noise"), ("slope", "plateau", "leak")
os.environ["LC_ALL"] = "C"


def psql():
    return subprocess.Popen(["psql", "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)


def script(sql):
    p = psql()
    out, _ = p.communicate(sql)
    return out


def prepare(sql):
    """a session of its own that disconnects: what the next backend finds is what this one left in the database, not in its memory"""
    out = script(SETTINGS + sql)
    err = next((l for l in out.splitlines() if "ERROR" in l), None)
    if err:
        raise RuntimeError("setup: " + err.strip())


def send(p, sql, marker):
    """sql then a marker line: what psql printed in between, and the first error"""
    p.stdin.write(sql + f"\n\\echo {marker}\n")
    out = []
    for l in iter(p.stdout.readline, ""):
        if marker in l:
            break
        out.append(l)
    return out, next((l.strip() for l in out if "ERROR" in l), None)


PEAK_STOP = "select 'PEAK', sift._test_rss_peak_stop();"


def num_of(out, tag):
    return int(next(l for l in out if l.startswith(tag + "|")).strip().split("|")[1])


def measure_mb(setup, warm, op):
    """setup, then warm (`=`: op itself), in a session of its own, so that the server holds what it holds after those whatever ran before; then a new backend:
    SETTINGS, no transparent huge pages, what malloc can return, op; the peak anonymous memory above what it held before op, in MB. The backend samples its own memory
    (sift._test_rss_peak_start): a reader outside it takes its samples when the runner schedules it, and a statement of 20 ms may fall between two of them"""
    prepare(setup + "\n" + (op if warm.strip() == "=" else warm))
    p = psql()
    out, err = send(p, SETTINGS + f"select sift._test_thp_disable();\nselect sift._test_malloc_stats(true);\n{PEAK_STOP}\nselect 'BASE', sift._test_rss_peak_start();", "WARMDONE")
    if err:
        raise RuntimeError("warm: " + err)
    base = num_of(out, "BASE")
    _, err = send(p, op, "OPDONE")
    out, hw_err = send(p, PEAK_STOP + "\nselect 'SCOPES', coalesce(sum(peak), 0) from sift._test_mem_highwater();", "HWDONE")
    p.stdin.write("\\q\n")
    p.wait()
    if err or hw_err:
        raise RuntimeError("op: " + (err or hw_err))
    return (num_of(out, "PEAK") - base) / 1024.0, num_of(out, "SCOPES") / 1048576.0


def build_tables(n):
    script(ASYNC_OFF + f"""
drop table if exists f{n}, d{n} cascade;
create table f{n} as
select g as id, g as k_hi, g % 5000 as k_mid, g % 10 as k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] as cat,
       'name' || (g::bigint * 7919) % 1000000 as txt, ((g::bigint * 7919) % 29500)::numeric / 100 as amount, (g::bigint * 7919 % 29500)::float8 / 100 as fl,
       timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second' as ts,
       (array['wireless','portable','compact','premium','rugged','slim','smart','classic'])[1 + g % 8] || ' ' ||
       (array['charger','speaker','headphones','keyboard','lamp','monitor','camera'])[1 + (g / 8) % 7] || ' w' || (g % 20000) as title
from generate_series(1, {n}) g;
alter table f{n} add primary key (id);
create index f{n}_col on f{n} using sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts);
create index f{n}_bm on f{n} using sift (title);
vacuum analyze f{n};
create table d{n} as select g as id, g % 7 as cls, 'dim' || g as label from generate_series(1, {n}) g;
alter table d{n} add primary key (id);
create index d{n}_col on d{n} using sift_col (id, cls);
vacuum analyze d{n};
""")


def copy_table(n):
    script(ASYNC_OFF + f"""
drop table if exists m{n}, s{n} cascade;
create table m{n} as select * from f{n};
alter table m{n} add primary key (id);
create index m{n}_col on m{n} using sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts);
create index m{n}_bm on m{n} using sift (title);
vacuum analyze m{n};
""")


def read_cases(only, shard=None):
    cases = []
    for l in open(TSV, encoding="utf-8").read().split("\n"):
        if not l.strip() or l.startswith("#"):
            continue
        p = (l.split("\t") + ["", ""])[:8]
        name, kind, ceiling, status, setup, op, warm, reason = p[0], p[1], int(p[2]), p[3], *(x.replace("\\n", "\n") for x in p[4:7]), p[7]
        if (only is None or re.search(only, name)) and in_shard(name, shard):
            cases.append((name, kind, ceiling, status, setup, op, warm, reason))
    return cases


def parse_shard(spec):
    if not spec:
        return None
    i, n = (int(x) for x in spec.split("/"))
    assert 1 <= i <= n, f"shard {spec}"
    return i, n


def in_shard(name, shard):
    return shard is None or zlib.crc32(name.encode()) % shard[1] == shard[0] - 1


def verdict(slope, ceiling, status):
    if status == "open":
        return "PASS" if CLEAN < ceiling and slope <= ceiling and slope > ceiling * 0.4 else "FAIL"
    return "PASS" if slope <= ceiling else "FAIL"


def uses_copy(*sql):
    return any(k in s for s in sql for k in ("{m}", "{s}"))


def slope_case(sizes, name, setup, warm, op, first=0):
    peaks, highs, exact = [], [], []
    for n in sizes:
        fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", s=f"s{n}", n=n)
        runs = []
        for _ in range(RUNS):
            if uses_copy(setup, warm, op):
                copy_table(n)
            runs.append(measure_mb(setup.removeprefix("@other ").format(**fmt), warm if warm == "=" else warm.format(**fmt), op.format(**fmt)))
        peaks.append(min(r[0] for r in runs))
        highs.append(max(r[0] for r in runs))
        exact.append(min(r[1] for r in runs))
    per_row = lambda v: (v[-1] - v[first]) * 1048576.0 / (sizes[-1] - sizes[first])
    return per_row(peaks), peaks, highs, per_row(exact)


def leak_case(n, name, op):
    fmt = dict(f=f"f{n}", d=f"d{n}", n=n)
    sql = op.format(**fmt).rstrip(";")
    out = script(SETTINGS + f"""create temp table leakres(i int, kb bigint);
do $$ declare i int; begin for i in 1..60 loop execute $q${sql}$q$; if i in (10, 60) then insert into leakres select i, sum(total_bytes)/1024 from pg_backend_memory_contexts; end if; end loop; end $$;
select string_agg(i || ':' || kb, ' ' order by i) from leakres;""")
    m = re.search(r"10:(\d+) 60:(\d+)", out)
    if not m:
        raise RuntimeError("leak: " + out.strip()[-200:])
    return int(m.group(2)) - int(m.group(1))


def run_case(sizes, name, kind, ceiling, status, setup, op, warm):
    """one SLOPE line, and whether the case passed"""
    if kind == "leak":
        grown = leak_case(sizes[1], name, op)
        ok = grown <= LEAK_KB
        return ok, f"SLOPE {name} leak {grown} kB over 50 repeats, at most {LEAK_KB} kB {'PASS' if ok else 'FAIL'}", None, None, None
    slope, peaks, highs, exact = slope_case(sizes, name, setup, warm, op, 1 if kind == "plateau" else 0)
    v = verdict(slope, ceiling, status)
    shown = '/'.join(f'{p:.1f}' for p in peaks)
    return v == "PASS", f"SLOPE {name} slope {slope:.0f} B/row (peaks {shown} MB at {'/'.join(str(s) for s in sizes)} rows{', slope from ' + str(sizes[1]) if kind == 'plateau' else ''}), ceiling {ceiling} [{status}], memory scopes {exact:.0f} B/row {v}", slope, [f"{p:.1f}" if p == h else f"{p:.1f}-{h:.1f}" for p, h in zip(peaks, highs)], exact


def run_cases(sizes, cases):
    bad = 0
    for name, kind, ceiling, status, setup, op, warm, _ in cases:
        t0 = time.time()
        try:
            ok, line, *_ = run_case(sizes, name, kind, ceiling, status, setup, op, warm)
        except RuntimeError as e:
            ok, line = False, f"SLOPE {name} {kind} ERROR {e}"
        print(line, flush=True)
        print(f"TIME {name} {time.time() - t0:.1f}", flush=True)
        bad += not ok
    return bad


def sizes_of(rows):
    return [rows, rows * 2, rows * 4]


def build_all(sizes):
    for n in sizes:
        build_tables(n)


def run(rows, only, shard, reverse):
    cases = read_cases(only, shard)
    if not os.path.exists("/proc/self/status"):
        print("SKIP needs /proc")
        return 0
    sizes = sizes_of(rows)
    build_all(sizes)
    return 1 if run_cases(sizes, cases[::-1] if reverse else cases) else 0


def spread_ok(a, b):
    return abs(a - b) <= max(VARIANCE_ROWS, VARIANCE_PCT * max(abs(a), abs(b)))


def slopes_of(text):
    return {m[1]: float(m[2]) for m in re.finditer(r"^SLOPE (\S+) slope (-?\d+) B/row", text, re.M)}


def compare(a_text, b_text, what="compare"):
    a, b = slopes_of(a_text), slopes_of(b_text)
    bad = 0
    for name in sorted(a.keys() | b.keys()):
        ok = name in a and name in b and spread_ok(a[name], b[name])
        print(f"SLOPE {name} {what} {a.get(name)} B/row against {b.get(name)} B/row, at most max({VARIANCE_ROWS:.0f} B/row, {VARIANCE_PCT:.0%}) apart {'PASS' if ok else 'FAIL'}")
        bad += not ok
    return 1 if bad else 0


def determinism(rows, only):
    """the same cases forward, forward again and in reverse, in fresh backends: what a slope depends on is the case, not the order or the run"""
    cases = [c for c in read_cases(only) if c[1] == "slope"]
    sizes = sizes_of(rows)
    build_all(sizes)
    runs = {k: {} for k in ("first", "again", "reverse")}
    for tag, order in (("first", cases), ("again", cases), ("reverse", cases[::-1])):
        for c in order:
            runs[tag][c[0]] = run_case(sizes, *c[:7])[2:]
    bad = 0
    for name, *_ in cases:
        exact = [runs[t][name][2] for t in runs]
        sampled = "/".join(f"{runs[t][name][0]:.0f}" for t in runs)
        peaks = "; ".join("/".join(runs[t][name][1]) for t in runs)
        ok = all(spread_ok(exact[0], e) for e in exact[1:])
        print(f"SLOPE {name} determinism {'/'.join(f'{e:.0f}' for e in exact)} B/row of memory scopes (first, again, reverse), at most max({VARIANCE_ROWS:.0f} B/row, {VARIANCE_PCT:.0%}) apart; "
              f"sampled peaks {sampled} B/row ({peaks} MB) {'PASS' if ok else 'FAIL'}", flush=True)
        bad += not ok
    return 1 if bad else 0


def selftest():
    assert verdict(5, CLEAN, "clean") == "PASS"
    assert verdict(CLEAN + 1, CLEAN, "clean") == "FAIL"
    assert verdict(90, 140, "open") == "PASS"
    assert verdict(160, 140, "open") == "FAIL"
    assert verdict(10, 140, "open") == "FAIL"
    assert verdict(-80, 64, "noise") == "PASS" and verdict(65, 64, "noise") == "FAIL"
    assert spread_ok(13, 11) and not spread_ok(13, 10) and spread_ok(200, 235) and not spread_ok(200, 240)
    with contextlib.redirect_stdout(io.StringIO()):
        assert compare("SLOPE a slope 10 B/row", "SLOPE a slope 12 B/row") == 0 and compare("SLOPE a slope 10 B/row", "SLOPE a slope 40 B/row") == 1
    cases = read_cases(None)
    names = [c[0] for c in cases]
    assert len(names) == len(set(names)), "a case name twice"
    for name, kind, ceiling, status, setup, op, warm, reason in cases:
        assert kind in KINDS and status in STATUSES, name
        assert (status == "clean") == (ceiling <= CLEAN), f"{name}: a clean row has a ceiling of at most {CLEAN}, any other has more"
        assert (status == "clean") == (not reason), f"{name}: an open or noise row says why in its REASON column, a clean row has none"
        assert op.strip() and "@other" not in warm + op, name
        assert not re.search(r"\{(?!f\}|d\}|m\}|s\}|n\})", setup + warm + op), f"{name}: placeholders are {{f}} {{d}} {{m}} {{s}} {{n}}"
        assert not re.search(r"(?i)\bset (work_mem|maintenance_work_mem)", setup + warm + op), f"{name}: the gate sets the memory settings"
    for k in (1, 2, 3, 4):
        assert sorted(c[0] for i in range(1, k + 1) for c in read_cases(None, (i, k))) == sorted(names), f"{k} shards are the cases, each once"
    print("mem-slope selftest: ok")
    return 0


def arg(name, default=None):
    return sys.argv[sys.argv.index(name) + 1] if name in sys.argv else default


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    only, shard = arg("--only"), parse_shard(arg("--shard", os.environ.get("MEM_SLOPE_SHARD")))
    rows = int(arg("--rows", 50000))
    if cmd == "selftest":
        sys.exit(selftest())
    if cmd == "list":
        print("\n".join(c[0] for c in read_cases(only, shard)))
        sys.exit(0)
    if cmd == "compare":
        sys.exit(compare(open(sys.argv[2]).read(), open(sys.argv[3]).read()))
    if cmd == "determinism":
        sys.exit(determinism(rows, only))
    sys.exit(run(rows, only, shard, "--reverse" in sys.argv))
