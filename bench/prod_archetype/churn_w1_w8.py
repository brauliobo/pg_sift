"""The eight pipelines of the archetype's continuous churn, one worker each, run against an arm's tables for a number of seconds.

W1 append events, W2 flip the latest flag of an entity's events (a bulk UPDATE of one daily flag), W3 update the status of recent events, W4 upsert entities (INSERT .. ON CONFLICT DO UPDATE),
W5 delete the oldest events in a batch, W6 append the log of a job, W7 move a job through its states (many updates of one row), W8 retention: drop the parts older than the window.

Usage: churn_w1_w8.py --dsn "dbname=bench" --schema arm_b --seconds 60 [--replica-dsn "..."]. The replica monitor reports the replay lag in bytes while the workers run.
"""
import argparse
import random
import subprocess
import threading
import time


def psql(dsn, sql):
    r = subprocess.run(["psql", "-X", "-qAt", "-v", "ON_ERROR_STOP=1", dsn, "-c", sql], capture_output=True, text=True)
    return r.returncode == 0, (r.stdout if r.returncode == 0 else r.stderr)


def pipelines(s, rnd):
    e, n, j = s + ".events", s + ".entities", s + ".job_logs"
    ent = rnd.randint(1, 20000)
    job = rnd.randint(1, 5000)
    return {
        "W1": f"INSERT INTO {e} SELECT m + g, {ent}, 'view', now(), 1.50, 1, true, 'new', md5(g::text) FROM generate_series(1, 200) g, (SELECT coalesce(max(id), 0) m FROM {e}) x ON CONFLICT DO NOTHING",
        "W2": f"UPDATE {e} SET is_latest = (id = (SELECT max(id) FROM {e} WHERE entity_id = {ent})) WHERE entity_id = {ent}",
        "W3": f"UPDATE {e} SET status = 'done' WHERE id IN (SELECT id FROM {e} ORDER BY id DESC LIMIT 100) AND status <> 'done'",
        "W4": f"INSERT INTO {n} VALUES ({ent}, 'north', 1, current_date) ON CONFLICT (id) DO UPDATE SET tier = ({n}.tier % 4) + 1",
        "W5": f"DELETE FROM {e} WHERE id IN (SELECT id FROM {e} ORDER BY id LIMIT 100)",
        "W6": f"INSERT INTO {j} SELECT {job}, coalesce(max(seq), -1) + 1, 'queued', now(), 'n' FROM {j} WHERE job_id = {job} ON CONFLICT DO NOTHING",
        "W7": f"UPDATE {j} SET state = (array['queued', 'running', 'retry', 'done'])[1 + (random() * 3)::int], at = now() WHERE job_id = {job} AND seq = 0",
        "W8": f"SELECT * FROM sift.drop_parts_before('{e}', now() - interval '3650 days')",
    }


def worker(name, args, stop, counts):
    rnd = random.Random(name)
    while not stop.is_set():
        ok, out = psql(args.dsn, pipelines(args.schema, rnd)[name])
        counts[name][0 if ok else 1] += 1
        if not ok and args.verbose:
            print(name, out.strip())


def monitor(args, stop, lags):
    while not stop.is_set():
        ok, out = psql(args.replica_dsn, "SELECT pg_wal_lsn_diff(pg_last_wal_receive_lsn(), pg_last_wal_replay_lsn())")
        if ok and out.strip():
            lags.append(float(out))
        stop.wait(1.0)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dsn", required=True)
    p.add_argument("--replica-dsn")
    p.add_argument("--schema", required=True)
    p.add_argument("--seconds", type=int, default=60)
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()
    names = [f"W{i}" for i in range(1, 9)]
    counts = {n: [0, 0] for n in names}
    lags = []
    stop = threading.Event()
    threads = [threading.Thread(target=worker, args=(n, args, stop, counts)) for n in names]
    if args.replica_dsn:
        threads.append(threading.Thread(target=monitor, args=(args, stop, lags)))
    for t in threads:
        t.start()
    time.sleep(args.seconds)
    stop.set()
    for t in threads:
        t.join()
    for n in names:
        print(f"{n}\tok={counts[n][0]}\tfailed={counts[n][1]}")
    if lags:
        print(f"replica_lag_bytes\tmax={max(lags):.0f}\tlast={lags[-1]:.0f}")


if __name__ == "__main__":
    main()
