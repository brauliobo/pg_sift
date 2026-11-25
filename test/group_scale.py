#!/usr/bin/env python3
"""The scale-growth gate of the groups of SiftAggV6 that spill (test/group-scale.tsv): what a GROUP BY holds in memory and writes to temp files at 1x, 2x and 4x rows, work_mem at 1 MB.

Every measurement is a backend of its own on a table of N, 2N and 4N rows: the statement runs once in a session that disconnects (the warm run), then in the measured backend, which samples
its own anonymous memory (sift._test_rss_peak_start) and counts the bytes of its memory scopes (sift._test_mem_highwater). A case passes when, across the three sizes,
  alpha_mem  = peak(4x) / peak(1x)                        <= 1.15
  beta_exact = the slope of the bytes of the scopes       <= 2 B/row
  beta_rss   = the slope of the resident peak             <= 8 B/row
  temp_bytes / input_bytes (the heap of the table)        <= 2.5 at every size
and its rows are the rows of PostgreSQL's own aggregate (sift.enable_agg = off), by md5, with no file left in pg_ls_tmpdir() after the statement.

  scripts: test/group_scale.py run [--rows N] [--only RE] [--runs R]    one RESULT line a case, then the table
           test/group_scale.py list [--only RE]

TSV columns: NAME FORMAT OPERATION [DIGEST] [SET]: FORMAT 2 or 1 is the format of the sift_col index of the tables ({t}, and {j} for the one of jsonb documents); DIGEST is the SQL text of a row r of OPERATION
that the parity is taken on (default r::text); SET is a statement the session of the case runs first.
Reads PGHOST, PGPORT, PGUSER, PGDATABASE like psql; needs Linux (/proc) and an installed pg_sift and pg_sift_test.
"""
import importlib.util, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("mem_slope", os.path.join(HERE, "..", "scripts", "mem-slope.py"))
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

ALPHA_MEM, BETA_EXACT, BETA_RSS, TEMP_RATIO = 1.15, 2.0, 8.0, 2.5
RUNS = int(os.environ.get("GROUP_SCALE_RUNS", "3"))
TSV = os.environ.get("GROUP_SCALE_TSV") or os.path.join(HERE, "group-scale.tsv")
SETTINGS = ("set sift.async_maintenance='off'; set work_mem='1MB'; set hash_mem_multiplier=2; set max_parallel_workers_per_gather=0; "
            "set max_parallel_maintenance_workers=0;\n")
EXTRA = ""


def build(n, fmt):
    t = f"t{fmt}_{n}"
    ms.script(ms.BUILD_SETTINGS + f"""set sift.test_create_format1 = {'off' if fmt == 2 else 'on'};
drop table if exists {t} cascade;
create table {t} as select g as id, g as k_hi, g::numeric + 0.123456789 as k_num, md5(g::text)::uuid as k_uuid, (g || ' seconds')::interval as k_iv,
  g % 5000 as k_mid, g % 10 as k_lo, repeat(md5(g::text), 4) as txt, (g::bigint * 7919 % 1000003)::float8 / 7 as fl, g % 100 as amt from generate_series(1, {n}) g;
alter table {t} add primary key (id);
create index {t}_col on {t} using sift_col (k_hi, k_num, k_uuid, k_iv, k_mid, k_lo, txt, fl, amt);
vacuum analyze {t};
drop table if exists s{fmt}_{n} cascade;
create table s{fmt}_{n} as select g as id, 1 as k_st, g % 100 as amt from generate_series(1, {n}) g;
analyze s{fmt}_{n};
update s{fmt}_{n} set k_st = id;
alter table s{fmt}_{n} add primary key (id);
create index s{fmt}_{n}_col on s{fmt}_{n} using sift_col (id, k_st, amt);
vacuum s{fmt}_{n};
drop table if exists j{fmt}_{n} cascade;
create table j{fmt}_{n} as select g as id, g % 20 as cat, jsonb_build_array(jsonb_build_object('category', 'c' || (g % 20), 'price', (g::bigint * 7919 % 1000003)::float8 / 7)) as items from generate_series(1, {n}) g;
alter table j{fmt}_{n} add primary key (id);
create index j{fmt}_{n}_col on j{fmt}_{n} using sift_col (id, cat, items) with (paths = 'items[].category text, items[].price float8');
vacuum analyze j{fmt}_{n};
""")
    return t


def temp_bytes(p):
    out, err = ms.send(p, "select pg_stat_force_next_flush();\nselect 'TEMP', temp_bytes from pg_stat_database where datname = current_database();", "TEMPDONE")
    return ms.num_of(out, "TEMP")


def measure(t, op):
    """one backend: the warm run in a session of its own, then the measured one: (rss peak MB, scopes MB, seconds, temp bytes, files left in pg_ls_tmpdir(), error)"""
    ms.prepare(EXTRA + "\\o /dev/null\n" + op + ";\n\\o\n")
    p = ms.psql()
    out, err = ms.send(p, SETTINGS + EXTRA + "select sift._test_thp_disable();\nselect sift._test_malloc_stats(true);\n" + ms.PEAK_STOP + "\nselect 'BASE', sift._test_rss_peak_start();", "WARMDONE")
    if err:
        raise RuntimeError("warm: " + err)
    base = ms.num_of(out, "BASE")
    t0 = temp_bytes(p)
    s0 = time.time()
    _, op_err = ms.send(p, "\\o /dev/null\n" + op + ";\n\\o", "OPDONE")
    secs = time.time() - s0
    out, _ = ms.send(p, ms.PEAK_STOP + "\nselect 'SCOPE', scope, peak from sift._test_mem_highwater() where parent is null;\nselect 'LEFT', count(*) from pg_ls_tmpdir();", "HWDONE")
    t1 = temp_bytes(p)
    p.stdin.write("\\q\n")
    p.wait()
    scopes = sum(int(l.strip().split("|")[2]) for l in out if l.startswith("SCOPE|"))
    return (ms.num_of(out, "PEAK") - base) / 1024.0, scopes / 1048576.0, secs, t1 - t0, ms.num_of(out, "LEFT"), op_err


def digest(op, dig, agg_on):
    r = ms.script(SETTINGS + EXTRA + f"set sift.enable_agg = {'on' if agg_on else 'off'};\nselect count(*), md5(string_agg(d, ',' order by d)) from (select {dig} as d from ({op}) r) q;")
    m = re.search(r"^(\d+\|[0-9a-f]{32})$", r, re.M)
    return m.group(1) if m else "ERROR " + r.strip()[-160:]


def read_cases(only):
    cases = []
    for l in open(TSV, encoding="utf-8").read().split("\n"):
        if l.strip() and not l.startswith("#"):
            p = (l.split("\t") + [""] * 5)[:5]
            if only is None or re.search(only, p[0]):
                cases.append((p[0], int(p[1]), p[2], p[3] or "r::text", p[4]))
    return cases


def slope(v, sizes):
    return (v[-1] - v[0]) * 1048576.0 / (sizes[-1] - sizes[0])


def run_case(sizes, tables, name, fmt, op, dig):
    peaks, exact, temps, secs, left, errs = [], [], [], [], 0, []
    for n in sizes:
        o = op.format(t=tables[(fmt, n)], j="j" + tables[(fmt, n)][1:], s="s" + tables[(fmt, n)][1:])
        runs = [measure(tables[(fmt, n)], o) for _ in range(RUNS)]
        peaks.append(min(r[0] for r in runs))
        exact.append(min(r[1] for r in runs))
        secs.append(min(r[2] for r in runs))
        temps.append(max(r[3] for r in runs))
        left += sum(r[4] for r in runs)
        errs += [r[5] for r in runs if r[5]]
    o = op.format(t=tables[(fmt, sizes[1])], j="j" + tables[(fmt, sizes[1])][1:], s="s" + tables[(fmt, sizes[1])][1:])
    parity = digest(o, dig, True) == digest(o, dig, False) and not digest(o, dig, True).startswith("ERROR")
    heap = [int(ms.script(f"select pg_relation_size('{tables[(fmt, n)]}');").strip().splitlines()[-1]) for n in sizes]
    alpha = peaks[-1] / max(peaks[0], 0.001)
    bex, brss = slope(exact, sizes), slope(peaks, sizes)
    ratio = max(t / h for t, h in zip(temps, heap))
    ok = alpha <= ALPHA_MEM and bex <= BETA_EXACT and brss <= BETA_RSS and ratio <= TEMP_RATIO and parity and left == 0 and not errs
    why = [w for w, bad in (("alpha_mem", alpha > ALPHA_MEM), ("beta_exact", bex > BETA_EXACT), ("beta_rss", brss > BETA_RSS), ("temp_ratio", ratio > TEMP_RATIO), ("parity", not parity),
                            ("tmpdir_left", left != 0), ("error: " + (errs[0].replace("\n", " ")[:90] if errs else ""), bool(errs))) if bad]
    return ok, (f"{name} | {' / '.join(f'{p:.1f}' for p in peaks)} MB | {alpha:.2f} | {bex:.1f} | {brss:.1f} | {max(temps) / 1048576:.1f} MB ({ratio:.2f}x) | "
                f"{secs[-1] / max(secs[0], 0.001):.1f} | {'PASS' if ok else 'FAIL ' + ','.join(why)}")


def run(rows, only):
    if not os.path.exists("/proc/self/status"):
        print("SKIP needs /proc")
        return 0
    sizes = [rows, rows * 2, rows * 4]
    cases = read_cases(only)
    tables = {(f, n): build(n, f) for f in sorted({c[1] for c in cases}) for n in sizes}
    print(f"case | peak_1x/2x/4x ({'/'.join(map(str, sizes))} rows) | alpha_mem | beta_exact B/row | beta_rss B/row | temp_bytes (max over sizes, x heap) | alpha_time | verdict", flush=True)
    bad = 0
    global EXTRA
    for name, fmt, op, dig, extra in cases:
        EXTRA = extra.strip().rstrip(";") + ";\n" if extra.strip() else ""
        try:
            ok, line = run_case(sizes, tables, name, fmt, op, dig)
        except RuntimeError as e:
            ok, line = False, f"{name} | ERROR {e}"
        print(line, flush=True)
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "run"
    only = ms.arg("--only")
    if "--runs" in sys.argv:
        RUNS = int(ms.arg("--runs"))
    if cmd == "list":
        print("\n".join(c[0] for c in read_cases(only)))
        sys.exit(0)
    sys.exit(run(int(ms.arg("--rows", 50000)), only))
