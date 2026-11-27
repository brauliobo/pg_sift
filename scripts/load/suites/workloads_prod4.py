"""The fourth wave of production shapes of the load gate (Spec 116): transactions of 80 savepoints (past the 64 subtransactions a backend caches: the readers go to
pg_subtrans) beside row-lock upgrades from FOR KEY SHARE to UPDATE, a partitioned sift_table of sift_table and heap leaves under rows moved across them while partitions are
attached and detached concurrently, a scope index under live facts and pairs beside sift.col_scope_verify and an unprivileged role, 48 pooled sessions of the three engines
beside sessions that connect, build, drop and disconnect while the ledger of the shared memory is watched, prepared transactions of the three engines through a kill -9, and
rewrites (VACUUM FULL, CLUSTER, REINDEX CONCURRENTLY) streamed to the readers of a hot standby (scripts/load/suites: WORKLOADS on test/t/core/load_workloads.sh, REPL on
test/t/core/load_repl.sh).
"""
from __future__ import annotations

import sys
import time
from dataclasses import dataclass

sys.dont_write_bytecode = True

from cases import LoadCase  # noqa: E402
from collect import AGG, BM, N, add, base, checks, col, ctx_kb, engines, kept, ledger_writes, mem_delta, multi_kept, nxt, oracle, oracles, tbl, zipf  # noqa: E402
from suites.common import Limits, prepared_left  # noqa: E402
from suites.repl import DIGEST, catchup, snapshot_oracle, standby  # noqa: E402
from suites.workloads_prod2 import DUTY, snap_oracle  # noqa: E402
from suites.workloads_prod3 import ROW_DIGEST, digest_differs, dups  # noqa: E402


@dataclass(frozen=True)
class Wave4(Limits):
    acl_leaks: int | None = 0
    prepared_lost: int | None = 0
    lock_unenforced: int | None = 0
    prepared_overlock: int | None = 0


def own(w, i: int, n: int) -> int:
    """key I of worker W among N workers that never share a key: the keys of the fixture of residue W.seed mod N"""
    return n * (1 + (i * 37 + w.seed * 101) % (N // n - 1)) + w.seed % n


def snap_digest(w, a: str, b: str, row: str) -> int:
    """digest_differs in worker W: 1 when A and B hold other rows in one REPEATABLE READ snapshot, 0 when they agree or the snapshot failed"""
    out = w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {ROW_DIGEST.format(row=row, t=a)}; {ROW_DIGEST.format(row=row, t=b)}; COMMIT')
    return int(out is not None and out[0] != out[1])


def paced(w):
    """idles DUTY times as long as the last statement of W took: a share of the wall clock on any host"""
    time.sleep(DUTY * max(w.lat[-1:] or [12.5]) / 1000)


X_SHAPE = 'id int8 PRIMARY KEY, grp int, v int8, body text'
X_ROW = "id || ':' || v"
SUBXACTS = 80
X_WRITERS = 4


def x_nested(w, i: int) -> tuple:
    """transaction I of a writer: SUBXACTS savepoints, each updating a key of its own in the sift_table and its heap twin, every tenth rolled back; the increments it keeps"""
    undo = {j for j in range(SUBXACTS) if j % 10 == 9}
    steps = []
    for j in range(SUBXACTS):
        k = own(w, i * SUBXACTS + j, X_WRITERS)
        steps.append(f'SAVEPOINT s{j}; UPDATE x_tbl SET v = v + 1 WHERE id = {k}; UPDATE x_tw SET v = v + 1 WHERE id = {k}' + (f'; ROLLBACK TO s{j}' if j in undo else ''))
    return 'BEGIN; ' + '; '.join(steps) + '; COMMIT', SUBXACTS - len(undo)


def txn_subxact_overflow_lockraise(ctx):
    """a keyed sift_table and its heap twin: 4 writers each commit transactions of 80 savepoints (an update of both tables in each, every tenth rolled back: more
    subtransactions than a backend caches, so every snapshot of the others looks them up in pg_subtrans), 2 sessions lock Zipf-hot keys FOR KEY SHARE and then update them
    in a savepoint (the lock raised), 2 readers compare the tables and the analytic path with the heap in one snapshot: every increment kept once, every answer agrees, no
    deadlock. The checks after the load run in a session of their own, which exits before the hygiene: on Format 1.0 a backend keeps the tombstone log it read in a cache that
    spills to temporary files for as long as it lives"""
    db = ctx.db
    db.run(f'CREATE TABLE x_tbl ({X_SHAPE}) USING sift_table; CREATE TABLE x_tw ({X_SHAPE}); INSERT INTO x_tbl SELECT * FROM load_base_fact; '
           'INSERT INTO x_tw SELECT * FROM load_base_fact; ANALYZE x_tbl, x_tw')
    v0 = int(db.val('SELECT sum(v) FROM x_tbl'))
    ws = ctx.sessions(8)

    def write(w, i):
        sql, n = x_nested(w, i)
        if w.do(sql) is not None:
            w.ledger += n

    def lockraise(w, i):
        k = zipf(w.seed * 7919 + i, N, 1.1)
        share = '; '.join(f'SELECT 1 FROM {t} WHERE id = {k} FOR KEY SHARE' for t in ('x_tbl', 'x_tw'))
        if w.do(f'BEGIN; {share}; SAVEPOINT a; UPDATE x_tbl SET v = v + 1 WHERE id = {k}; UPDATE x_tw SET v = v + 1 WHERE id = {k}; RELEASE a; COMMIT') is not None:
            w.ledger += 1

    def read(w, i):
        w.ledger += snap_oracle(w, AGG.format(t='x_tbl'), 'sift.enable_agg', ()) if i % 2 else snap_digest(w, 'x_tbl', 'x_tw', X_ROW)

    busy = ctx.phase({'w': (ws[:4], write), 'l': (ws[4:6], lockraise), 'r': (ws[6:], read)}, 2.5)
    chk, ups = ctx.sessions(1)[0], sum(w.ledger for w in ws[:6])
    lost = sum(abs(int(chk.val(f'SELECT sum(v) FROM {t}')) - v0 - ups) for t in ('x_tbl', 'x_tw'))
    mismatch = digest_differs(chk, 'x_tbl', 'x_tw', X_ROW) + oracle(chk, AGG.format(t='x_tbl'), 'sift.enable_agg') + sum(w.ledger for w in ws[6:])
    return base(ctx, busy, 'w', lost_rows=lost, dup_rows=dups(chk, 'x_tbl'), oracle_mismatch=mismatch, raised=busy['l'].ops, snapshots=busy['r'].ops, subxacts=SUBXACTS)


P_LEAVES = ((0, 'sift_table'), (1, 'heap'), (2, 'sift_table'), (3, 'heap'))
P_SHAPE = 'id int8, p int, v int8, body text'
P_ROW = "id || ':' || p || ':' || v"
P_LIVE = 'pm WHERE p < 4'
P_AGG = 'SELECT p, count(*), sum(v) FROM pm WHERE p < 4 GROUP BY p ORDER BY p'
P_DETACHED = 200
P_UPPER = 2 * 10 ** 8


def p_attach(n: int) -> str:
    """a new leaf N (a sift_table or a heap, by turns) of P_DETACHED rows of its own list value, attached to pm"""
    lo = P_UPPER + n * 1000
    return (f"CREATE TABLE pm_x{n} (LIKE pm) USING {('sift_table', 'heap')[n % 2]}; INSERT INTO pm_x{n} SELECT g, {100 + n}, g % 1000, 'wire x' FROM "
            f'generate_series({lo}, {lo + P_DETACHED - 1}) g; ALTER TABLE pm ATTACH PARTITION pm_x{n} FOR VALUES IN ({100 + n})')


def ddl_sift_part_move_detach(ctx):
    """a table partitioned by list USING sift_table, two leaves sift_tables and two heaps, and its heap twin: 3 writers move rows of their own keys to the next leaf (from a
    sift_table to a heap and back) and insert rows, each statement on both tables in one transaction, 2 readers compare the live leaves with the twin and the analytic path
    with the heap in one snapshot, while 1 session attaches a new sift_table or heap leaf and detaches it again CONCURRENTLY, idle DUTY times as long as its last statement
    took: the twin agrees in every snapshot and at the end, every committed row kept once, every detached leaf whole"""
    db = ctx.db
    leaves = ''.join(f'CREATE TABLE pm_{p} PARTITION OF pm FOR VALUES IN ({p}) USING {am}; ' for p, am in P_LEAVES)
    db.run(f'CREATE TABLE pm ({P_SHAPE}, PRIMARY KEY (p, id)) PARTITION BY LIST (p) USING sift_table; {leaves}CREATE TABLE pm_tw ({P_SHAPE}, PRIMARY KEY (id)); '
           'INSERT INTO pm SELECT id, id % 4, v, body FROM load_base_fact; INSERT INTO pm_tw SELECT * FROM pm; ANALYZE pm, pm_tw')
    ws = ctx.sessions(6)

    def write(w, i):
        w.ids = getattr(w, 'ids', set())
        if i % 4 == 3:
            k = nxt(w)
            if w.do(f"BEGIN; {'; '.join(f'INSERT INTO {t} VALUES ({k}, {k % 4}, 0, $$wire new$$)' for t in ('pm', 'pm_tw'))}; COMMIT") is not None:
                w.ids.add(k)
            return
        k = own(w, i, 3)
        w.do(f"BEGIN; {'; '.join(f'UPDATE {t} SET p = (p + 1) % 4, v = v + 1 WHERE id = {k}' for t in ('pm', 'pm_tw'))}; COMMIT")

    def read(w, i):
        w.ledger += snap_oracle(w, P_AGG, 'sift.enable_agg', ()) if i % 2 else snap_digest(w, P_LIVE, 'pm_tw', P_ROW)

    def ddl(w, i):
        n = i // 2
        if i % 2 == 0:
            w.do(p_attach(n))
        elif w.do(f'ALTER TABLE pm DETACH PARTITION pm_x{n} CONCURRENTLY') is not None:
            out = w.do(f'SELECT count(*), count(DISTINCT id) FROM pm_x{n}; DROP TABLE pm_x{n}')
            w.ledger += out != [f'{P_DETACHED}|{P_DETACHED}']
        paced(w)

    busy = ctx.phase({'w': (ws[:3], write), 'r': (ws[3:5], read), 'd': (ws[5:], ddl)}, 2.5)
    ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:3]))
    mismatch = digest_differs(db, P_LIVE, 'pm_tw', P_ROW) + oracle(db, P_AGG, 'sift.enable_agg') + sum(w.ledger for w in ws[3:5])
    return base(ctx, busy, 'w', **add(kept(db, 'pm', ids, 10 ** 6, 10 ** 8), {'lost_rows': ws[5].ledger, 'dup_rows': dups(db, 'pm')}), oracle_mismatch=mismatch,
                ddl=busy['d'].ops, snapshots=busy['r'].ops)


SC_MEMBERS = 400
SC_KEYS = 16
SC_SCOPE = "scope = 's: m -> sc_pairs(m, s) max_multiplicity(8)'"
SC_JOIN = 'SELECT count(*), sum(e.v) FROM sc_ev e JOIN sc_pairs p ON p.m = e.m WHERE p.s = {s}'
SC_VERIFY = "SELECT count(*) FILTER (WHERE problem = 'differs'), count(*) FILTER (WHERE problem = 'unverified') FROM sift.col_scope_verify('sc_ev_ix', {n})"
SC_PROBES = (SC_JOIN.format(s=3), "SELECT count(*) FROM sift.col_scope_verify('sc_ev_ix', 4)", 'REINDEX INDEX sc_ev_ix')


def sc_setup(fmt: int) -> str:
    """the pairs (member m in 1 to 4 of SC_KEYS scopes) and the facts with their scope index; the scope summary is Format 2.0, so a run on Format 1.0 builds that one index
    on Format 2.0 (the journal of the pairs and every other object keep the format of the run)"""
    pin = 'SET LOCAL sift.test_create_format1 = off; ' if fmt == 1 else ''
    return (f'CREATE TABLE sc_pairs (m int NOT NULL, s int NOT NULL, PRIMARY KEY (m, s)); INSERT INTO sc_pairs SELECT m, 1 + (m * 5 + j * 3) % {SC_KEYS} '
            f'FROM generate_series(1, {SC_MEMBERS}) m, generate_series(0, 3) j WHERE j <= m % 4 ON CONFLICT DO NOTHING; '
            "CREATE INDEX sc_pairs_j ON sc_pairs USING sift_col (m, s) WITH (track_changes = 'ctid'); "
            f'CREATE TABLE sc_ev (id int8 PRIMARY KEY, m int, v int8); INSERT INTO sc_ev SELECT id, 1 + id % {SC_MEMBERS}, v FROM load_base_fact; '
            f'BEGIN; {pin}CREATE INDEX sc_ev_ix ON sc_ev USING sift_col (id, m, v) WITH ({SC_SCOPE}); COMMIT; ANALYZE sc_ev, sc_pairs; '
            'DROP ROLE IF EXISTS load_sc_probe; CREATE ROLE load_sc_probe; GRANT USAGE ON SCHEMA load_case TO load_sc_probe; GRANT SELECT ON sc_ev TO load_sc_probe')


def sc_counts(out: list) -> tuple:
    """(keys that differ, keys unverified) of the answer of SC_VERIFY in one REPEATABLE READ snapshot"""
    differs, unverified = out[0].split('|')
    return int(differs), int(unverified)


def feat_scope_live_churn_acl(ctx):
    """a heap of facts with a sift_col scope index over pairs (a member in up to 4 of 16 scopes) journaled by track_changes: 2 writers insert, update and delete facts, 1
    session adds, moves and removes pairs, 2 readers compare the scope read of every key with the plain join in one snapshot, 1 runs sift.col_scope_verify in one snapshot,
    1 vacuums and maintains, while a role without SELECT on the pairs reads the scope, verifies it and reindexes it: every one refused with 42501 (acl_leaks), every
    committed fact kept once, every scope answers what the join does"""
    db = ctx.db
    db.run(sc_setup(ctx.fmt))
    try:
        ws = ctx.sessions(8)

        def write(w, i):
            w.ids = getattr(w, 'ids', set())
            k = nxt(w)
            old = sorted(w.ids)[:1] if i % 3 == 2 else []
            drop = f'; DELETE FROM sc_ev WHERE id = {old[0]}' if old else ''
            sql = f'BEGIN; INSERT INTO sc_ev VALUES ({k}, {1 + k % SC_MEMBERS}, {k % 1000}); UPDATE sc_ev SET v = v + 1 WHERE id = {own(w, i, 2)}{drop}; COMMIT'
            if w.do(sql) is not None:
                w.ids = (w.ids | {k}) - set(old)

        def pairs(w, i):
            m, s = 1 + (i * 37) % SC_MEMBERS, 1 + (i // 3 * 5) % SC_KEYS
            w.do((f'INSERT INTO sc_pairs VALUES ({m}, {s}) ON CONFLICT DO NOTHING', f'UPDATE sc_pairs SET s = {1 + s % SC_KEYS} WHERE m = {m} AND s = {s}',
                  f'DELETE FROM sc_pairs WHERE m = {m} AND s IN ({s}, {1 + s % SC_KEYS})')[i % 3], ('23505',))

        def read(w, i):
            w.ledger += snap_oracle(w, SC_JOIN.format(s=1 + (w.seed * 7 + i) % SC_KEYS), 'sift.enable_agg', ())

        def verify(w, i):
            out = w.do(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {SC_VERIFY.format(n=SC_KEYS)}; COMMIT')
            differs, unverified = sc_counts(out) if out else (0, 0)
            w.ledger, w.unverified = w.ledger + differs, getattr(w, 'unverified', 0) + unverified
            time.sleep(0.05)

        def maint(w, i):
            w.do(('VACUUM sc_ev, sc_pairs', "SELECT count(*) FROM sift.maintain('sc_ev_ix')", "SELECT count(*) FROM sift.maintain('sc_pairs_j')")[i % 3])
            time.sleep(0.05)

        def probe(w, i):
            w.do(f'SET ROLE load_sc_probe; {SC_PROBES[i % len(SC_PROBES)]}', ('42501',))
            w.ledger += w.last is None
            w.run('RESET ROLE')
            time.sleep(0.02)

        busy = ctx.phase({'w': (ws[:2], write), 'p': (ws[2:3], pairs), 'r': (ws[3:5], read), 'v': (ws[5:6], verify), 'm': (ws[6:7], maint), 'u': (ws[7:], probe)}, 2.5)
        ctx.close_sessions()
        out, err, _ = db.q(f'BEGIN ISOLATION LEVEL REPEATABLE READ; {SC_VERIFY.format(n=100)}; COMMIT')
        differs, unverified = (0, 0) if err else sc_counts(out)
        mismatch = differs + sum(oracle(db, SC_JOIN.format(s=s), 'sift.enable_agg') for s in range(1, SC_KEYS + 1)) + sum(w.ledger for w in ws[3:6])
        ids = set().union(*(getattr(w, 'ids', set()) for w in ws[:2]))
        m = base(ctx, busy, 'w', **kept(db, 'sc_ev', ids, 10 ** 6), oracle_mismatch=mismatch, acl_leaks=ws[7].ledger, refused=busy['u'].expected, verified=busy['v'].ops,
                 unverified=unverified + getattr(ws[5], 'unverified', 0), pair_edits=busy['p'].ops)
        return add(m, {'unexpected_errors': int(err is not None), 'notes': [f'note: the last verify: {err[0]} {err[1][:100]}'] if err else []})
    finally:
        ctx.close_sessions()
        db.run('DROP OWNED BY load_sc_probe; DROP ROLE load_sc_probe')


POOLS = 'SELECT pool, used_bytes, refusals FROM sift.shared_memory_pools() ORDER BY pool'
RESET = 'DISCARD ALL; SET search_path = load_case, public'


def pools(db) -> dict:
    return {p: (int(u), int(r)) for p, u, r in (row.split('|') for row in db.run(POOLS))}


def pool_conn_churn_shm_ledger(ctx):
    """48 pooled sessions of the three engines, each statement followed by DISCARD ALL: 24 commit rows of a sift_table, a sift_col and a BM25 table, 12 aggregate the
    sift_col table, 12 search the BM25 one, while 4 sessions connect, create, fill, read and drop a sift_table and a sift_col table of their own and disconnect: every
    committed row kept, the index paths answer what the heap does, the memory of the pooled backends after the load, nothing refused by the pools of the shared memory, and
    a second of the 4 alone beside the idle pool leaves the pools holding no more than before it (the tables they drop and the credit of every backend that exits given
    back; the pooled writers grow the manifest of their tables by design)"""
    db = ctx.db
    engines(db)
    ws = ctx.sessions(52)
    write = ledger_writes('e')

    def oltp(w, i):
        write(w, i)
        w.do(RESET)

    def olap(w, i):
        w.do(f"{(AGG.format(t='e_col'), BM.format(t='e_bm'))[w.seed > 36]}; {RESET}")

    def churn(w, i):
        s, t = type(db)(init='SET search_path = load_case, public', app=f'load_wchurn{w.seed}'), f'cc_{w.seed}_{i}'
        _, err, _ = s.q(f'{tbl(t, 256)}; {col(t + "_h", 256)}; SELECT grp, sum(v) FROM {t}_h GROUP BY grp; SELECT count(*) FROM {t}; DROP TABLE {t}, {t}_h')
        s.close()
        if err:
            w.bad.append(f'{err[0]} {err[1][:100]}')
        else:
            w.ledger += 1

    roles = {'w': (ws[:24], oltp), 'r': (ws[24:48], olap), 'c': (ws[48:], churn)}
    probe = ws[:48:4]
    ctx.phase(roles, 0.6)
    before, r0 = ([ctx_kb(w) for w in probe], ctx.rss(probe)), pools(db)
    busy = ctx.phase(roles, 2.0)
    after = [ctx_kb(w) for w in probe], ctx.rss(probe)
    p0 = pools(db)
    ctx.phase({'c': (ws[48:], churn)}, 1.0)
    p1 = pools(db)
    ctx.close_sessions()
    grown = {p: p1[p][0] - p0[p][0] for p in p0}
    left = int(db.val("SELECT count(*) FROM pg_class WHERE relname LIKE 'cc\\_%' AND relnamespace = 'load_case'::regnamespace"))
    return base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[:24]), {'lost_rows': left}), oracle_mismatch=oracles(db), shm_used_after=max(0, sum(grown.values())),
                shm_refusals=sum(p1[p][1] - r0[p][1] for p in p0), cycles=sum(w.ledger for w in ws[48:]), shm_grown=','.join(f'{p}:{g}' for p, g in grown.items()),
                **mem_delta(ctx, before, after))


TPC = 8
TPC_ID = 9 * 10 ** 7
TPC_FREE = 4096
TPC_LOCKS = ('e_tbl', 'e_col', 'e_bm')
TPC_HELD = "SELECT substr(gid, 10)::int FROM pg_prepared_xacts WHERE gid LIKE 'load\\_tpc\\_%'"


def nowait(t: str, k: int) -> str:
    return f'SELECT id FROM {t} WHERE id = {k} FOR UPDATE NOWAIT'


def tpc_prepare(j: int) -> str:
    """prepared transaction J: row 1 + J of each engine locked NOWAIT (the transactions prepared before it hold other rows only) and updated, and a row inserted into each"""
    k = TPC_ID + j
    ups = '; '.join(f'{nowait(t, 1 + j)}; UPDATE {t} SET v = v + 1000 WHERE id = {1 + j}' for t in TPC_LOCKS)
    ins = '; '.join(f"INSERT INTO {t} VALUES ({k}, {k % 64}, 1, 'wire tpc{j}')" for t in TPC_LOCKS)
    return f"BEGIN; {ups}; {ins}; PREPARE TRANSACTION 'load_tpc_{j}'"


def tpc_locks(w, held: set, allow: tuple = ()) -> tuple:
    """(rows of the prepared transactions HELD that worker W locks FOR UPDATE NOWAIT: each must be refused with 55P03, rows of no prepared transaction it is refused: none)"""
    unlocked = overlocked = 0
    for j in range(TPC):
        for t in TPC_LOCKS:
            unlocked += j in held and w.do(nowait(t, 1 + j), ('55P03',) + allow) is not None
            overlocked += w.do(nowait(t, TPC_FREE + j), ('55P03',) + allow) is None and w.last[0] == '55P03'
    return unlocked, overlocked


def tpc_parity(db, held: set) -> int:
    """the rows of the prepared transactions HELD that differ from what COMMIT PREPARED (even J) and ROLLBACK PREPARED (odd J) leave in each engine, by the index paths too"""
    done = {j for j in held if j % 2 == 0}
    base_v = {int(i): int(x) for i, x in (r.split('|') for r in db.run(f'SELECT id, v FROM load_base_fact WHERE id <= {TPC}'))}
    bad = 0
    for t in TPC_LOCKS:
        v = {int(i): int(x) for i, x in (r.split('|') for r in db.run(f'SELECT id, v FROM {t} WHERE id <= {TPC} OR id >= {TPC_ID} AND id < {TPC_ID + TPC}'))}
        bad += sum(v.get(1 + j) != base_v[1 + j] + 1000 * (j in done) for j in range(TPC)) + sum((TPC_ID + j in v) != (j in done) for j in range(TPC))
    bad += sum(db.val(f"SET sift.enable_scan = {g}; SELECT count(*) FROM e_bm WHERE body @@> 'tpc*'; RESET sift.enable_scan") != str(len(done)) for g in ('on', 'off'))
    return bad + oracle(db, f'SELECT count(*), sum(v) FROM e_col WHERE id >= {TPC_ID}', 'sift.enable_agg')


def fault_2pc_crash_recovery(ctx):
    """8 prepared transactions, each holding an updated row and an inserted one of a sift_table, a heap with a sift_col index and one with a BM25 index: 4 writers commit rows
    of the three engines and 1 session locks the prepared rows and rows of no prepared transaction NOWAIT until the server is killed with -9 and started again; then every
    prepared transaction is there, its rows still refused NOWAIT (55P03, lock_unenforced) and no other row refused (prepared_overlock: the lock of a prepared transaction is
    its rows, and every prepare is let in), half of them committed and half rolled back leave exactly their rows in every engine and its index path, every acknowledged
    row kept"""
    db = ctx.db
    engines(db)
    refused = sum(db.q(tpc_prepare(j))[1] is not None for j in range(TPC))
    held = {int(x) for x in db.run(TPC_HELD)}
    ws = ctx.sessions(6)
    for w in ws[:5]:
        w.allow = ('08006', '57P01')
    write = ledger_writes('e', churn=False)

    def probe(w, i):
        unlocked, overlocked = tpc_locks(w, held, w.allow)
        w.ledger, w.over = w.ledger + unlocked, getattr(w, 'over', 0) + overlocked
        time.sleep(0.02)

    def crash(w, i):
        if int(w.val('SELECT count(*) FROM e_tbl WHERE id >= 1000000')) < 200 and i < 400:
            return time.sleep(0.01)
        ctx.hook('LOAD_CRASH')
        return False

    busy = ctx.phase({'w': (ws[:4], write), 'u': (ws[4:5], probe), 'k': (ws[5:], crash)}, 30.0, lead='k')
    ctx.close_sessions()
    ctx.restart('LOAD_RESTART')
    acked = set().union(*(getattr(w, 'ids', set()) for w in ws[:4]))
    have = {int(x) for x in db.run(f'SELECT id FROM e_tbl WHERE id >= 1000000 AND id < {TPC_ID}')}
    kept_held = {int(x) for x in db.run(TPC_HELD)}
    after = ctx.sessions(1)[0]
    unlocked, overlocked = tpc_locks(after, held)
    db.run('; '.join(f"{('COMMIT', 'ROLLBACK')[j % 2]} PREPARED 'load_tpc_{j}'" for j in sorted(kept_held)))
    m = add(kept(db, 'e_col', have, 10 ** 6, TPC_ID), kept(db, 'e_bm', have, 10 ** 6, TPC_ID), {'lost_rows': len(acked - have)})
    return base(ctx, busy, 'w', **m, oracle_mismatch=tpc_parity(db, held) + oracles(db), prepared_lost=len(held - kept_held), lock_unenforced=unlocked + ws[4].ledger,
                prepared_overlock=refused + overlocked + getattr(ws[4], 'over', 0), prepared_left=prepared_left(db), prepared=len(held), acked=len(acked))


R_IDS = 'CREATE INDEX e_col_id ON e_col (id); CREATE INDEX e_bm_id ON e_bm (id); CREATE TABLE e_side (id int8 PRIMARY KEY, v int8) USING sift_table; ' \
        'INSERT INTO e_side SELECT g, g FROM generate_series(1, 2048) g'
REWRITES = ('VACUUM FULL e_tbl', 'CLUSTER e_col USING e_col_id', 'REINDEX TABLE CONCURRENTLY e_bm', 'VACUUM FULL e_bm', 'CLUSTER e_tbl USING e_tbl_pkey',
            'REINDEX TABLE CONCURRENTLY e_col', 'VACUUM FULL e_col', 'REINDEX TABLE CONCURRENTLY e_tbl', 'CLUSTER e_bm USING e_bm_id')
R_CURSOR = ('BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM e_col; DECLARE c CURSOR FOR SELECT id FROM e_col ORDER BY id; FETCH 100 FROM c; '
            'CLUSTER e_side USING e_side_pkey; FETCH ALL FROM c; COMMIT')
R_READS = ((AGG.format(t='e_col'), 'sift.enable_agg'), (BM.format(t='e_bm'), 'sift.enable_scan'), ('SELECT count(*), sum(v) FROM e_tbl WHERE id % 7 = 3', 'sift.enable_scan'))


def repl_rewrite_cluster_standby(ctx):
    """on the primary 2 writers of the three engines, 1 session that rewrites them by turns (VACUUM FULL, CLUSTER, REINDEX TABLE CONCURRENTLY), idle DUTY times as long as
    its last statement took, and 1 that clusters a side sift_table inside a REPEATABLE READ transaction with a cursor open over another table; 4 REPEATABLE READ sessions of
    the hot standby compare the index paths with the heap while the rewrites replay (a query cancelled by a conflict with the recovery is no mismatch): every committed row
    kept, every cursor whole, and after a catch-up the standby answers what the primary does and its sift.check, quarantine and health are clean"""
    db, conn = ctx.db, standby()
    engines(db)
    db.run(R_IDS)
    st = ctx.sessions(1, conn=conn)[0]
    lag = catchup(db, st)
    ws, rs = ctx.sessions(4), ctx.sessions(4, conn=conn)

    def rewrite(w, i):
        w.do(REWRITES[i % len(REWRITES)])
        paced(w)

    def cursor(w, i):
        out = w.do(R_CURSOR)
        w.ledger += out is not None and int(out[0]) != len(out) - 1

    def read(w, i):
        w.ledger += snapshot_oracle(w, *R_READS[(w.seed + i) % len(R_READS)])

    busy = ctx.phase({'w': (ws[:2], ledger_writes('e')), 'd': (ws[2:3], rewrite), 'c': (ws[3:], cursor), 'r': (rs, read)}, 2.5)
    lag += catchup(db, st)
    differ = sum(db.run(q) != st.run(q) for q in DIGEST)
    return base(ctx, busy, 'w', **add(multi_kept(db, 'e', ws[:2]), checks(st), {'oracle_mismatch': oracles(db) + ws[3].ledger}),
                standby_mismatch=differ + lag + sum(w.ledger for w in rs), rewrites=busy['d'].ops, cursors=busy['c'].ops, conflicts=busy['r'].expected,
                standby_reads=busy['r'].ops)


WORKLOADS = (
    LoadCase('txn_subxact_overflow_lockraise', 'txn', txn_subxact_overflow_lockraise, Wave4()),
    LoadCase('ddl_sift_part_move_detach', 'ddl', ddl_sift_part_move_detach, Wave4()),
    LoadCase('feat_scope_live_churn_acl', 'feat', feat_scope_live_churn_acl, Wave4()),
    LoadCase('pool_conn_churn_shm_ledger', 'pool', pool_conn_churn_shm_ledger, Wave4(ctx_kb_delta=1280, rss_mb_delta=16, shm_used_after=0)),
    LoadCase('fault_2pc_crash_recovery', 'fault', fault_2pc_crash_recovery, Wave4(), last=True),
)
REPL = (
    LoadCase('repl_rewrite_cluster_standby', 'repl', repl_rewrite_cluster_standby, Wave4()),
)
