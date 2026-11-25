"""Runs the eighteen queries of queries_q1_q18.sql on the arms of the archetype and checks that every arm gives the md5 of the oracle.

Arms are named by the schema of their tables (generate.sql): A is the heap twin (the oracle), B the sift_table, C the sift_table after the projections the statement can use were built.
Usage: run.py --dsn "dbname=bench" --arms A=arm_a,B=arm_b,C=arm_c [--repeat 3]. Prints a TSV: query, arm, median milliseconds, md5, same-as-oracle. Exit status 1 when an arm differs.
"""
import argparse
import re
import statistics
import subprocess
import sys
from pathlib import Path


def psql(dsn, sql):
    r = subprocess.run(["psql", "-X", "-qAt", "-v", "ON_ERROR_STOP=1", dsn, "-c", sql], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr.strip())
    return r.stdout.strip()


def queries(path):
    text = Path(path).read_text()
    out = []
    for block in text.split("\n;;\n"):
        m = re.search(r"^-- (Q-A\d+): (.*)$", block, re.M)
        if m:
            body = "\n".join(line for line in block.splitlines() if not line.startswith("--"))
            out.append((m.group(1), m.group(2), body.strip()))
    return out


def run(dsn, sql, schema, repeat):
    q = sql.replace("@E", schema + ".events").replace("@N", schema + ".entities").replace("@J", schema + ".job_logs")
    digest = f"SELECT md5(coalesce(string_agg(r::text, E'\\n'), '')) FROM ({q}) r"
    times = []
    for _ in range(repeat):
        t = psql(dsn, f"SET max_parallel_workers_per_gather = 0; EXPLAIN (ANALYZE, TIMING OFF, SUMMARY ON, COSTS OFF) {q}")
        m = re.search(r"Execution Time: ([0-9.]+) ms", t)
        times.append(float(m.group(1)) if m else float("nan"))
    return statistics.median(times), psql(dsn, digest)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dsn", required=True)
    p.add_argument("--arms", required=True)
    p.add_argument("--repeat", type=int, default=3)
    p.add_argument("--queries", default=str(Path(__file__).with_name("queries_q1_q18.sql")))
    args = p.parse_args()
    arms = [a.split("=") for a in args.arms.split(",")]
    different = 0
    print("query\tarm\tms\tmd5\tsame")
    for qid, _, sql in queries(args.queries):
        oracle = None
        for arm, schema in arms:
            ms, digest = run(args.dsn, sql, schema, args.repeat)
            oracle = digest if oracle is None else oracle
            same = digest == oracle
            different += not same
            print(f"{qid}\t{arm}\t{ms:.1f}\t{digest}\t{'t' if same else 'f'}")
    sys.exit(1 if different else 0)


if __name__ == "__main__":
    main()
