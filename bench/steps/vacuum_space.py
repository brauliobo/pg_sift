#!/usr/bin/env python3
"""Repeat the store load/churn on a private native PostgreSQL cluster.

Transport adapter only: DDL, generator, load and UPDATE/DELETE phases come from
store.py. The cluster must expose the extension and have no unrelated tables in
store, store_col or store_tbl; these databases are replaced on every repetition.
Run the same command against the baseline and candidate installations.
"""
import argparse
import json
import os
import platform
import subprocess
import time
from types import SimpleNamespace

import store


class NativeClient:
    def __init__(self, eid):
        self.psql = store.Psql(eid)

    def run(self, statement, capture=False):
        start = time.perf_counter()
        out = self.psql.run(statement, capture)
        return (time.perf_counter() - start) * 1000, '\n'.join(out) + '\n' if capture else None

    def close(self):
        self.psql.close()


def accounting(eid):
    def query(sql):
        return json.loads(store.psql_value(eid, sql))

    result = query("SELECT json_object_agg(c.relname, pg_relation_size(c.oid)) FROM pg_class c "
                   "WHERE c.oid = 'orders'::regclass OR c.oid IN (SELECT indexrelid FROM pg_index WHERE indrelid = 'orders'::regclass)")
    if eid == 'sift':
        result['space'] = query("SELECT row_to_json(i) FROM sift.col_index_info('orders_col') i")
        result['segments'] = query("SELECT coalesce(json_agg(i), '[]') FROM sift.col_segments('orders_col') i")
    elif eid == 'sift_table':
        result['space'] = query("SELECT row_to_json(i) FROM sift.table_space('orders') i")
        result['segments'] = query("SELECT coalesce(json_agg(i), '[]') FROM sift.table_segments('orders') i")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--data', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--label', required=True)
    parser.add_argument('--repetitions', type=int, default=3)
    parser.add_argument('--rows', type=int, default=1000000)
    parser.add_argument('--load-seconds', type=int, default=15)
    args = parser.parse_args()
    store.port = lambda eid: args.port
    store.Client = NativeClient
    cfg = SimpleNamespace(rows=args.rows, data=args.data, tier=f'{args.rows}-c2-native', runs=15, warmups=3,
                          probes=20, rates=[0, 1000, 10000], load_seconds=args.load_seconds, untimed=[])
    store.cmd_gen(cfg)
    result = dict(label=args.label, date=time.strftime('%Y-%m-%d'), rows=args.rows,
                  method=f'store.py load_pg/update_engine; native transport; 15 latency samples; 3 warmups; 20 probes; {args.load_seconds}s rate windows',
                  host=dict(cpu=subprocess.check_output(['lscpu'], text=True).split('Model name:')[1].splitlines()[0].strip(),
                            kernel=platform.release(), threads=os.cpu_count(), ram_bytes=os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES'), cpus=sorted(os.sched_getaffinity(0))), repetitions=[])
    real_size = store.Writer.size_mib
    for repetition in range(args.repetitions):
        rows, reference, states = store.Rows(cfg.tier), {}, {}
        record = dict(run=repetition + 1, load_start=os.getloadavg(), states=states, cells=rows.rows)
        result['repetitions'].append(record)
        for eid in ('pg', 'sift', 'sift_table'):
            store.load_pg(eid, cfg, rows)
            result['postgres'] = store.psql_value(eid, 'SELECT version()')
            if eid == 'sift':
                result['extension'] = store.psql_value(eid, "SELECT extversion FROM pg_extension WHERE extname = 'pg_sift'")
        def size(writer):
            value = real_size(writer)
            states.setdefault(writer.eid, []).append(accounting(writer.eid))
            return value
        store.Writer.size_mib = size
        for eid in ('pg', 'sift', 'sift_table'):
            store.update_engine(eid, cfg, rows, reference)
            client = NativeClient(eid)
            q = store.load_queries(['02'])[0].statement(eid, cfg.rows)
            for _ in range(3):
                client.run(q)
            times = [client.run(q)[0] for _ in range(15)]
            rows.add(eid, '02', 'after maintenance', 'latency_ms', 'ms', st=store.summary(times))
            client.close()
        store.Writer.size_mib = real_size
        record['load_end'] = os.getloadavg()
        with open(args.out, 'w') as f:
            json.dump(result, f, indent=2)
            f.write('\n')
        store.log(f'{args.label} repetition {repetition + 1} stored')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
