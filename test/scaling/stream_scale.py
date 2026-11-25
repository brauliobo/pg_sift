#!/usr/bin/env python3
"""The scale gate of the streaming, window, ASOF, Grace join and DISTINCT ON nodes of SiftAggV6 (test/scaling/stream_scale.tsv): every case runs at 1x, 2x and 4x rows in a fresh backend that first runs the statement over the 1x tables (what a backend takes once is not the statement's).

A node that holds work_mem and not the table has the same peak at every size and writes no temporary file it need not. For each case the harness reports

  case | peak_1x | peak_2x | peak_4x | a_mem | b_exact | b_rss | temp_bytes | a_time | verdict

peak: the larger of the bytes the memory scopes of sift counted (sift._test_mem_highwater()) and the resident peak of the backend above its start (sift._test_rss_peak_start()), in MB. The statement runs under a cursor that is moved forward 4096 rows at a time, and each time the memory contexts of the backend are summed: exact is the largest growth of those that are not of the source of the scan (the contexts of the column and table layers: the decoded granules, one for each part of the table it opens, follow the parts and not the node), and the resident peak is taken less the largest the source took.
a_mem = (peak_4x + base) / (peak_1x + base): the case's base absorbs what a sampler cannot see; b_exact and b_rss: bytes of a row between 1x and 4x on the scopes (less EXACT_SLACK_MB: the free space a memory context keeps in its blocks after its files and tables of a partition are freed) and on the resident peak.
temp_bytes: what pg_stat_database counts for the statement at 4x; a_time = t_4x / max(t_1x, 50 ms). A case passes when the statement succeeds, a_mem <= 1.15, b_exact <= 2.0, b_rss <= 8.0 B/row,
temp_bytes within the case's rule (`0`, or `max:B` bytes a row of the 1x size), nothing is left in pg_ls_tmpdir() afterwards, and a_time <= 5.5.
Kind `memx`: the resident peak is reported and not judged, the peak is the exact one: the statement reads a sorted table, whose column layer holds the decoded granules of every part it merges in memory outside the memory contexts (it follows the parts of the table, not the node).
Kind `cancel`: the statement runs under a statement_timeout of CANCEL_MS and must stop within CANCEL_SLACK_MS of it, with nothing left in pg_ls_tmpdir().

  test/scaling/stream_scale.py run [--rows N] [--only RE]     the table above, exit 1 when a case fails
  test/scaling/stream_scale.py list

Reads PGHOST, PGPORT, PGUSER like psql; creates the database p95 and the extensions pg_sift and pg_sift_test in it. TSV columns: NAME KIND FIXTURE SETTINGS BASE_MB TEMP CAP_MB OPERATION; {t} {u} the tables of the fixture at the size; CAP_MB (0: none) bounds peak_4x.
"""
import os, re, subprocess, sys, time

ROWS = int(os.environ.get("P95_ROWS", "50000"))
SCALES = (1, 2, 4)
A_MEM, B_EXACT, B_RSS, A_TIME, A_TIME_FLOOR, EXACT_SLACK_MB = 1.15, 2.0, 8.0, 5.5, 0.3, 0.5
CANCEL_MS, CANCEL_SLACK_MS = 400, 700
DB = os.environ.get("P95_DB", "p95")
TSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stream_scale.tsv")
BASE = "set statement_timeout='180s'; set sift.async_maintenance='off'; set max_parallel_workers_per_gather=0; set max_parallel_maintenance_workers=0; set sift.agg_force=on;\n"
FORCE = "set seq_page_cost=10000; set cpu_tuple_cost=100; set random_page_cost=10000; set enable_indexscan=off; set enable_indexonlyscan=off; set enable_bitmapscan=off;\n"
JOIN = "set enable_hashjoin=off; set enable_mergejoin=off; set enable_nestloop=off;\n"
SOURCE = "^sift (cache )?f2 "
os.environ["LC_ALL"] = "C"

FIXTURES = {
    "sg": lambda t, n: f"create table {t} (id int primary key, k int, amount int, txt text) using sift_table;\ninsert into {t} select g, g % 7, g % 1000, 'row ' || g from generate_series(1, {n}) g;",
    "wg": lambda t, n: f"""create table {t} (grp int, id int, ts timestamptz, txt text collate "C", c1 int, primary key (grp, id)) using sift_table;
select sift.table_set_option('{t}', 'key_order', 'grp, id');
insert into {t} select 1, g, timestamptz '2025-01-01' + (g::bigint * 7919 % 100003) * interval '1 second', repeat(md5(g::text), 8), g % 1000 from generate_series(1, {n}) g;""",
    "wn": lambda t, n: f"""create table {t} (g1 int, g2 int, g3 int, g4 int, o1 int, o2 int, o3 int, o4 int, v1 int, v2 int, v3 int, v4 int, v5 int, primary key (g1, g2, g3, g4, o1, o2, o3, o4)) using sift_table;
select sift.table_set_option('{t}', 'key_order', 'g1, g2, g3, g4, o1, o2, o3, o4');
insert into {t} select 1, 1, 1, g % 2, g, 1, 1, 1, g, g, g, g, g from generate_series(1, 10) g;""",
    "jj": lambda t, n: f"""create table {t} (id int primary key, k int, v int) using sift_table;
insert into {t} select g, g % ({n} / 8), g from generate_series(1, {n}) g;
create table {t}_b (id int primary key, k int, v int, pad text) using sift_table;
insert into {t}_b select g, g % ({n} / 8), g, repeat('b', 24) from generate_series(1, {n}) g;""",
    "js": lambda t, n: f"""create table {t} (id int primary key, k int, v int) using sift_table;
insert into {t} select g, 1, g from generate_series(1, {n}) g;
create table {t}_b (id int primary key, k int, v int, pad text) using sift_table;
insert into {t}_b select g, 1, g, repeat('b', 24) from generate_series(1, 40000) g;
insert into {t}_b select 100000 + g, g + 1, g, repeat('b', 24) from generate_series(1, {n}) g;""",
    "jk": lambda t, n: f"""create table {t} (id int primary key, k int, v int) using sift_table;
insert into {t} select g, 1, g from generate_series(1, {n}) g;
create table {t}_b (id int primary key, k int, v int, pad text) using sift_table;
insert into {t}_b select g, 1, g, repeat('b', 24) from generate_series(1, 400000) g;
insert into {t}_b select 100000000 + g, g + 1, g, repeat('b', 24) from generate_series(1, {n}) g;""",
    "af": lambda t, n: f"""create table {t}_d (k int, ts bigint, px int, primary key (k, ts)) using sift_table;
select sift.table_set_option('{t}_d', 'key_order', 'k, ts');
insert into {t}_d select g % 50, g::bigint * 13, g from generate_series(1, {n}) g;
create table {t} (k int, ts bigint, id int, primary key (k, ts)) using sift_table;
select sift.table_set_option('{t}', 'key_order', 'k, ts');
insert into {t} select g % 50, g::bigint * 10 + g % 7, g from generate_series(1, {n}) g;""",
    "fg": lambda t, n: f"""create table {t} (id int primary key, k int, ts bigint, txt text) using sift_table;
insert into {t} select g, g, g * 7919 % 100003, repeat('x', 40) from generate_series(1, {n}) g;""",
}


def psql():
    return subprocess.Popen(["psql", "-X", "-q", "-A", "-t", "-d", DB, "-v", "ON_ERROR_STOP=0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)


def script(sql):
    out, _ = psql().communicate(sql)
    return out


def admin(sql):
    return subprocess.run(["psql", "-X", "-q", "-A", "-t", "-d", "postgres", "-c", sql], capture_output=True, text=True).stdout


def read_cases(only):
    cases = []
    for l in open(TSV, encoding="utf-8").read().split("\n"):
        if not l.strip() or l.startswith("#"):
            continue
        p = l.split("\t")
        c = dict(zip(("name", "kind", "fixture", "settings", "base", "temp", "cap", "op"), p))
        if only is None or re.search(only, c["name"]):
            cases.append(c)
    return cases


def build(rows):
    admin(f"create database {DB}") if DB not in admin("select datname from pg_database") else None
    script("create extension if not exists pg_sift; create extension if not exists pg_sift_test;\n")
    for fx, make in FIXTURES.items():
        for s in (SCALES[:1] if fx == "jk" else SCALES):
            t, n = f"p95_{fx}_{s}", rows * s
            if script(f"select to_regclass('{t}') is not null;").strip() == "t":
                continue
            out = script(BASE + f"set client_min_messages=warning; set sift.test_create_format1=off;\n{make(t, n)}\n"
                         + "".join(f"select sift.table_maintain('{x}') is not null;\nanalyze {x};\n" for x in [t] + ([t + "_b"] if fx in ("jj", "js", "jk") else []) + ([t + "_d"] if fx == "af" else [])))
            err = next((l for l in out.splitlines() if "ERROR" in l), None)
            if err:
                raise RuntimeError(f"fixture {t}: {err}")


def send(p, sql, marker):
    p.stdin.write(sql + f"\n\\echo {marker}\n")
    out = []
    for l in iter(p.stdout.readline, ""):
        if marker in l:
            break
        out.append(l)
    return out


def tagged(out, tag):
    return next((l.strip().split("|")[1:] for l in out if l.startswith(tag + "|")), None)


def sql_of(c, s):
    t, u = f"p95_{c['fixture']}_{s}", f"p95_{c['fixture']}_{s}_b" if c["fixture"] in ("jj", "js", "jk") else f"p95_{c['fixture']}_{s}_d"
    return c["op"].replace("{t}", t).replace("{u}", u).rstrip().rstrip(";")


def sampled(op):
    return f"""do $do$ declare c refcursor; n bigint; node bigint; src bigint; mn bigint := 0; ms bigint := 0; b bigint;
begin
  select coalesce(sum(total_bytes), 0) into b from pg_backend_memory_contexts where name !~ '{SOURCE}';
  open c for execute $op${op}$op$;
  loop
    move forward 1 from c; get diagnostics n = row_count;
    mn := greatest(mn, (select coalesce(sum(total_bytes), 0) from pg_backend_memory_contexts where name !~ '{SOURCE}') - b);
    exit when n = 0;
    move forward 4095 from c; get diagnostics n = row_count;
    select coalesce(sum(total_bytes) filter (where name !~ '{SOURCE}'), 0), coalesce(sum(total_bytes) filter (where name ~ '{SOURCE}'), 0) into node, src from pg_backend_memory_contexts;
    mn := greatest(mn, node - b); ms := greatest(ms, src);
    exit when n = 0;
  end loop;
  close c;
  raise notice 'P95|%|%', mn, ms;
end $do$;"""


def measure(c, s):
    cancel = c["kind"] == "cancel"
    op = sql_of(c, s)
    settings = BASE + ("" if c["fixture"] in ("sg", "fg") else FORCE) + (JOIN if c["fixture"] in ("jj", "js", "jk") else "") + c["settings"] + "\n"
    p = psql()
    if not cancel:
        send(p, settings + "\\o /dev/null\n" + sql_of(c, 1) + ";\n\\o\n", "W0")
    send(p, settings + "select sift._test_thp_disable();\nselect sift._test_malloc_stats(true);\nselect 'STOP', sift._test_rss_peak_stop();", "S0")
    base = int(tagged(send(p, "select 'BASE', sift._test_rss_peak_start();", "S1"), "BASE")[0])
    t0 = tagged(send(p, "select pg_stat_force_next_flush();\nselect 'T0', temp_bytes from pg_stat_database where datname = current_database();", "S2"), "T0")
    start = time.time()
    out = send(p, (f"set statement_timeout={CANCEL_MS};\n" if cancel else "set statement_timeout='180s';\n") + "set client_min_messages=notice;\n" + ("\\o /dev/null\n" + op + ";\n\\o\n" if cancel else sampled(op) + "\n") + "set statement_timeout='180s';", "OP")
    secs = time.time() - start
    err = next((l.strip() for l in out if "ERROR" in l), None)
    tail = send(p, "select 'STOP', sift._test_rss_peak_stop();\nselect pg_stat_force_next_flush();\n"
                "select 'T1', temp_bytes from pg_stat_database where datname = current_database();\nselect 'LEFT', count(*) from pg_ls_tmpdir();", "HW")
    p.stdin.write("\\q\n")
    p.wait()
    sample = next((l.split("P95|")[1].strip().split("|") for l in out if "P95|" in l), ["0", "0"])
    exact, source = int(sample[0]) / 1048576.0, int(sample[1]) / 1048576.0
    rss = max((int(tagged(tail, "STOP")[0]) - base) / 1024.0 - source, 0.0)
    return {"rows": ROWS * s, "peak": exact if c["kind"] == "memx" else max(rss, exact), "rss": rss, "exact": exact, "temp": int(tagged(tail, "T1")[0]) - int(t0[0]), "left": int(tagged(tail, "LEFT")[0]), "secs": secs, "err": err}


def run_case(c):
    runs = [measure(c, s) for s in (SCALES[:1] if c["kind"] == "cancel" else SCALES)]
    base = float(c["base"])
    n1, n4 = runs[0]["rows"], runs[-1]["rows"]
    cap = float(c["cap"])
    a_mem = (runs[-1]["peak"] + base) / (runs[0]["peak"] + base)
    b_exact = max(runs[-1]["exact"] - runs[0]["exact"] - EXACT_SLACK_MB, 0.0) * 1048576.0 / max(n4 - n1, 1)
    b_rss = (runs[-1]["rss"] - runs[0]["rss"]) * 1048576.0 / max(n4 - n1, 1)
    a_time = runs[-1]["secs"] / max(runs[0]["secs"], A_TIME_FLOOR)
    b_exact, b_rss = (b_exact, b_rss) if n4 > n1 else (0.0, 0.0)
    temp = runs[-1]["temp"]
    rule = c["temp"]
    why = []
    if c["kind"] == "cancel":
        late = max(r["secs"] for r in runs) * 1000 - CANCEL_MS
        if late > CANCEL_SLACK_MS:
            why.append(f"cancel took {late:.0f} ms past the timeout")
        if not any(r["err"] and "canceling statement" in r["err"] for r in runs):
            why.append("not cancelled")
    else:
        why += [f"{r['rows']} rows: {r['err']}" for r in runs if r["err"]][:1]
        if cap and runs[-1]["peak"] > cap:
            why.append(f"peak_4x {runs[-1]['peak']:.2f} > cap {cap}")
        if a_mem > A_MEM:
            why.append(f"a_mem {a_mem:.2f} > {A_MEM}")
        if b_exact > B_EXACT:
            why.append(f"b_exact {b_exact:.2f} > {B_EXACT}")
        if b_rss > B_RSS and c["kind"] != "memx":
            why.append(f"b_rss {b_rss:.2f} > {B_RSS}")
        if a_time > A_TIME:
            why.append(f"a_time {a_time:.1f} > {A_TIME}")
        limit = 0 if rule == "0" else int(rule.split(":")[1]) * n4 if rule.startswith("max:") else None
        if limit is not None and temp > limit:
            why.append(f"temp_bytes {temp} > {limit}")
    why += [f"{r['left']} files left in pg_ls_tmpdir() at {r['rows']} rows" for r in runs if r["left"]][:1]
    mb = lambda i: f"{runs[i]['peak']:.2f}" if i < len(runs) else "-"
    line = f"{c['name']} | {mb(0)} | {mb(1)} | {mb(2)} | {a_mem:.2f} | {b_exact:.2f} | {b_rss:.2f} | {temp} | {a_time:.1f} | {'PASS' if not why else 'FAIL ' + '; '.join(why)}"
    return not why, line


def main():
    args = sys.argv[1:]
    only = args[args.index("--only") + 1] if "--only" in args else None
    global ROWS
    if "--rows" in args:
        ROWS = int(args[args.index("--rows") + 1])
    cases = read_cases(only)
    if args[:1] == ["list"]:
        print("\n".join(c["name"] for c in cases))
        return 0
    build(ROWS)
    print("case | peak_1x | peak_2x | peak_4x | a_mem | b_exact | b_rss | temp_bytes | a_time | verdict")
    bad = 0
    for c in cases:
        ok, line = run_case(c)
        print(line, flush=True)
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
