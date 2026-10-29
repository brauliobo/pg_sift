#!/usr/bin/env python3
"""The IO load of a statement, in absolute numbers, to state beside its time and instructions (bench/README.md, "IO is a main metric").

    python3 -I bench/steps/iolib.py [--cold TABLE,..] [--size TABLE,..] [--db NAME] [--json] (-f FILE | SQL)      PGHOST / PGPORT as psql; the user is postgres

What one run reports (`measure`; every figure is a difference over the run, None where the server or the host cannot tell):

    hit read dirtied written   shared blocks of the plan's top node, EXPLAIN (ANALYZE, BUFFERS): the blocks served from shared buffers, read from the OS, dirtied, written.
                               A statement EXPLAIN cannot wrap (DDL, COPY, VACUUM, several statements) takes hit and read from pg_stat_io.
    read_ms                    time in reads, track_io_timing (EXPLAIN's I/O read time)
    stat_*                     pg_stat_io of the client backends, parallel workers (named `background worker` there) and io workers, relations (not WAL): reads, read_bytes, read_ms, writes, write_bytes, extends, extend_bytes.
                               PostgreSQL 17 has op_bytes instead of the byte columns (reads * op_bytes); 18 and 19 have them. Before 16 there is no pg_stat_io.
                               The delta is the cluster's, so it counts every other client backend that runs at the same time: measure on a quiet server
    proc_read_bytes            read_bytes of /proc/PID/io of the backend and of the io workers (PostgreSQL 18 reads in them with io_method = worker): what the storage device
                               was asked for (Linux; None elsewhere or when not readable; parallel workers are other processes and not in it, so run with
                               max_parallel_workers_per_gather = 0 to compare; a data directory on tmpfs reads nothing from a device)
    wal_bytes                  WAL written in the run (pg_current_wal_lsn difference)
    size_bytes                 growth of the total size of the tables passed as `sized` (a build's files: bulk index builds bypass the write counters of pg_stat_io)
    cache, how                 `cold` after `cool` emptied the relations of TABLES, else `warm`; how says what was emptied

The cold run needs no root: `pg_buffercache_evict_relation` (PostgreSQL 18; 17: `pg_buffercache_evict`; the extension is created when missing) empties the shared buffers of each table, its TOAST table and its
indexes, and posix_fadvise(DONTNEED) on their files (after an fsync) empties the OS page cache; on tmpfs the files are the memory and nothing else is emptied, and where the
files cannot be opened the page cache stays. PostgreSQL 17 evicts buffer by buffer (`pg_buffercache_evict`); without pg_buffercache the run is warm and `how` says so (restart the server for a cold run). A cold run is the first
run after the eviction; `measure` of the same statement right after is the warm run.

The harness uses `measure` (steps/cases.py) and `metrics` / `load_metrics` for the rows of bench/results; the same function serves any other script.
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

MB = 2 ** 20
EXPLAINABLE = re.compile(r'\s*(select|with|insert|update|delete|merge|values|table)\b', re.I)
EXPLAIN = 'EXPLAIN (ANALYZE, BUFFERS, COSTS OFF, TIMING OFF, SUMMARY OFF, FORMAT JSON) '
STAT_FIELDS = ('reads', 'read_bytes', 'read_ms', 'writes', 'write_bytes', 'extends', 'extend_bytes', 'hits')
STAT_WHERE = "backend_type IN ('client backend', 'background worker', 'parallel worker', 'io worker') AND object <> 'wal'"


def one(s, sql):
    """the fields of the first row of a statement on a duckdb.Session; a server error raises"""
    _, rows, err, _ = s.run(sql)
    if err:
        raise RuntimeError(err)
    return rows[0].split('|') if rows else []


def setup(s):
    """the facts of the server the figures depend on, once per session: (backend pid, block size, SQL of the pg_stat_io counters or None)"""
    if not hasattr(s, 'io_facts'):
        pid, block, has = one(s, "SELECT pg_backend_pid(), current_setting('block_size'), to_regclass('pg_stat_io') IS NOT NULL")
        sql = None
        if has == 't':
            bytes_ = one(s, "SELECT count(*) FROM pg_attribute WHERE attrelid = 'pg_stat_io'::regclass AND attname = 'read_bytes'")[0] != '0'
            cols = ['reads', 'read_bytes' if bytes_ else 'reads * op_bytes', 'read_time', 'writes', 'write_bytes' if bytes_ else 'writes * op_bytes', 'extends',
                    'extend_bytes' if bytes_ else 'extends * op_bytes', 'hits']
            sql = 'SELECT %s FROM pg_stat_io WHERE %s' % (', '.join('coalesce(sum(%s), 0)' % c for c in cols), STAT_WHERE)
        s.io_facts = (int(pid), int(block), sql)
    return s.io_facts


def proc_read_bytes(pids):
    """read_bytes of /proc/PID/io summed over PIDS; None when any is not readable (not Linux, another user)"""
    total = 0
    for pid in pids:
        try:
            with open('/proc/%s/io' % pid) as f:
                total += next(int(l.split()[1]) for l in f if l.startswith('read_bytes:'))
        except (OSError, StopIteration):
            return None
    return total


def snapshot(s):
    """the counters now (stats flushed first: a backend reports them at its leisure), for `delta`"""
    pid, _, sql = setup(s)
    stat = None
    if sql:
        s.run('SELECT pg_stat_force_next_flush()')
        stat = dict(zip(STAT_FIELDS, (float(x) for x in one(s, sql))))
    try:
        wal = int(one(s, "SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')::bigint")[0])
    except RuntimeError:   # a standby
        wal = None
    workers = s.run("SELECT pid FROM pg_stat_activity WHERE backend_type = 'io worker'")[1]   # PostgreSQL 18 with io_method = worker reads in these, not in the backend
    return dict(stat=stat, wal=wal, proc=proc_read_bytes([pid] + [w for w in workers if w]))


def delta(a, b):
    """what happened between two snapshots: stat_<field> (pg_stat_io), wal_bytes, proc_read_bytes; None where either end has none"""
    out = {'stat_' + k: (b['stat'][k] - a['stat'][k] if a['stat'] and b['stat'] else None) for k in STAT_FIELDS}
    out.update(wal_bytes=None if a['wal'] is None else b['wal'] - a['wal'], proc_read_bytes=None if a['proc'] is None or b['proc'] is None else b['proc'] - a['proc'])
    return out


def fs_type(path):
    """the file system type of the longest mount point that holds PATH ('' when /proc/mounts cannot say)"""
    best = ('', '')
    try:
        with open('/proc/mounts') as f:
            for line in f:
                _, mnt, typ = line.split()[:3]
                mnt = mnt.replace('\\040', ' ')
                if (path == mnt or path.startswith(mnt.rstrip('/') + '/')) and len(mnt) >= len(best[0]):
                    best = (mnt, typ)
    except OSError:
        pass
    return best[1]


def relations(tables):
    """SQL for the oids a table holds data in: itself, its TOAST table and the indexes of both (the recipe of cold_table in steps/lib.sh)"""
    names = ', '.join("'%s'::regclass" % t.replace("'", "''") for t in tables)
    return ('WITH t AS (SELECT oid, reltoastrelid FROM pg_class WHERE oid IN (%s)), h AS (SELECT oid FROM t UNION SELECT reltoastrelid FROM t WHERE reltoastrelid <> 0) '
            'SELECT oid FROM h UNION SELECT indexrelid FROM pg_index WHERE indrelid IN (SELECT oid FROM h)' % names)


def schema_tables(s, schema):
    """the tables with storage of a schema, as names for `cool`"""
    _, rows, err, _ = s.run("SELECT oid::regclass FROM pg_class WHERE relnamespace = '%s'::regnamespace AND relkind IN ('r', 'm')" % schema.replace("'", "''"))
    if err:
        raise RuntimeError(err)
    return rows


def cool(s, tables):
    """empty the relations of TABLES from shared buffers and the OS page cache: how ('evict+fadvise', 'evict (tmpfs)', 'evict (OS cache kept: ...)', plus a note of buffers that stayed
    pinned), or 'warm: REASON' when nothing could be emptied (PostgreSQL 17 evicts buffer by buffer, 18 by relation)"""
    rels = relations(tables)
    try:
        s.run('CHECKPOINT')   # a dirty buffer would stay in memory
        where = lambda: {r.split('|')[1]: r.split('|')[0] for r in s.run(   # noqa: E731  the schema each function is in: the search_path of a lane is its case's schema, which is dropped
            "SELECT n.nspname, p.proname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.proname IN ('pg_buffercache_evict_relation', 'pg_buffercache_evict') AND p.pronargs = 1")[1] if r}
        found = where()
        if not found:
            s.run('CREATE EXTENSION IF NOT EXISTS pg_buffercache SCHEMA public')
            found = where()
        if 'pg_buffercache_evict_relation' in found:
            evict = 'SELECT coalesce(sum(r.buffers_evicted), 0), coalesce(sum(r.buffers_skipped), 0) FROM (%s) x(c), LATERAL %s.pg_buffercache_evict_relation(c) r' % (rels, found['pg_buffercache_evict_relation'])
        elif 'pg_buffercache_evict' in found:   # PostgreSQL 17: one buffer at a time
            ns = found['pg_buffercache_evict']
            evict = ("SELECT count(*) FILTER (WHERE e), count(*) FILTER (WHERE NOT e) FROM (SELECT %s.pg_buffercache_evict(bufferid) e FROM %s.pg_buffercache WHERE reldatabase = "
                     "(SELECT oid FROM pg_database WHERE datname = current_database()) AND relfilenode IN (SELECT pg_relation_filenode(c) FROM (%s) x(c))) y" % (ns, ns, rels))
        else:
            return 'warm: no eviction (pg_buffercache is not installed)'
        _, skipped = one(s, evict)
        files = [l for l in s.run('SELECT pg_relation_filepath(c) FROM (%s) x(c)' % rels)[1] if l]
        data = one(s, 'SHOW data_directory')[0]
    except RuntimeError as e:
        return 'warm: no eviction (%s)' % str(e).split('\n')[0][:80]
    note = ' (%s buffers pinned, not evicted)' % skipped if skipped != '0' else ''
    if fs_type(data) == 'tmpfs':
        return 'evict (tmpfs)' + note
    if not hasattr(os, 'posix_fadvise'):
        return 'evict (OS cache kept: no posix_fadvise)' + note
    try:
        for rel in files:
            for f in glob.glob(os.path.join(data, rel)) + glob.glob(os.path.join(data, rel) + '.*') + glob.glob(os.path.join(data, rel) + '_*'):   # segments and the fsm / vm forks, not a longer oid
                fd = os.open(f, os.O_RDONLY)
                try:
                    os.fsync(fd)   # a dirty page stays in the cache
                    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                finally:
                    os.close(fd)
    except OSError as e:
        return 'evict (OS cache kept: %s)%s' % (e.strerror, note)
    return 'evict+fadvise' + note


def plan_blocks(s, sql):
    """the top node's shared blocks of EXPLAIN (ANALYZE, BUFFERS) of the statement, which this runs once"""
    _, rows, err, _ = s.run(EXPLAIN + sql.rstrip().rstrip(';'))
    try:
        top = json.loads('\n'.join(rows))[0]['Plan']
    except ValueError:   # the server's error text, in its language: the session only recognizes English
        raise RuntimeError(err or ' '.join(rows)[:200])
    return dict(hit=top['Shared Hit Blocks'], read=top['Shared Read Blocks'], dirtied=top['Shared Dirtied Blocks'], written=top['Shared Written Blocks'],
                temp_read=top['Temp Read Blocks'], temp_written=top['Temp Written Blocks'], read_ms=top.get('Shared I/O Read Time', top.get('I/O Read Time')))


def explainable(sql):
    return bool(EXPLAINABLE.match(sql)) and ';' not in sql.rstrip().rstrip(';')


def total_size(s, tables):
    return int(one(s, 'SELECT sum(pg_total_relation_size(c)) FROM (VALUES %s) x(c)' % ', '.join("('%s'::regclass)" % t.replace("'", "''") for t in tables))[0])


def measure(s, sql, tables=None, sized=None):
    """one run of SQL on the duckdb.Session S with its IO load (the module docstring); with TABLES the run follows `cool` of them, and cache says whether that worked;
    SIZED names the tables whose total size (heap, TOAST, indexes) is taken before and after, size_bytes being the growth: the bytes a build or a merge left on disk"""
    how = cool(s, tables) if tables else ''
    s.run('SET track_io_timing = on')   # the read time of EXPLAIN and of pg_stat_io (superuser)
    try:
        size = total_size(s, sized) if sized else None
        before = snapshot(s)
        plan = plan_blocks(s, sql) if explainable(sql) else None
        if plan is None:
            err = s.run(sql)[2]
            if err:
                raise RuntimeError(err)
        d = delta(before, snapshot(s))
        d['size_bytes'] = total_size(s, sized) - size if sized else None
    finally:
        s.run('RESET track_io_timing')
    _, block, _ = setup(s)
    if plan is None:   # no plan to read the blocks from: the counters of pg_stat_io are in bytes
        plan = dict(hit=d['stat_hits'], read=None if d['stat_read_bytes'] is None else d['stat_read_bytes'] / block, dirtied=None, written=None, temp_read=None, temp_written=None, read_ms=d['stat_read_ms'])
    hit, read = plan['hit'], plan['read']
    d.update(plan, cache='cold' if how.startswith('evict') else 'warm', how=how,
             hit_pct=100.0 * hit / (hit + read) if hit is not None and read is not None and hit + read else None,
             read_bytes=read * block if read is not None else None)   # the plan's blocks; pg_stat_io's own bytes stay in stat_read_bytes
    return d


# --- the rows of bench/results (steps/cases.py)

def metrics(io):
    """{metric: value} of one measurement for a results file: io_<cache>_read_blocks, _hit_pct, and after a cold run _read_mb (what pg_stat_io read, else blocks of the block size),
    _device_mb (the backend's /proc read_bytes) and _read_ms; absent where the figure is None"""
    c = 'io_' + io['cache'] + '_'
    mb = lambda b: None if b is None else b / MB  # noqa: E731
    out = {c + 'read_blocks': io['read'], c + 'hit_pct': io['hit_pct']}
    if io['cache'] == 'cold':
        out.update({c + 'read_mb': mb(io['read_bytes']), c + 'device_mb': mb(io['proc_read_bytes']), c + 'read_ms': io['read_ms']})
    return {k: ('%d' % v if k.endswith('blocks') else '%.2f' % v) for k, v in out.items() if v is not None}


def load_metrics(d):
    """{metric: MB} of a load, from `delta` of two snapshots: the file growth and the writes of the backend (the WAL is a row of its own); absent without pg_stat_io"""
    out = {'io_extend_mb': d['stat_extend_bytes'], 'io_write_mb': d['stat_write_bytes']}
    return {k: v / MB for k, v in out.items() if v is not None}


# --- the command

def show(io):
    return '\n'.join('%-18s %s' % (k, '-' if v is None else '%.2f' % v if isinstance(v, float) else v) for k, v in io.items())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('sql', nargs='?', help='the statement (or script) to run once')
    ap.add_argument('-f', '--file', help='read the statement from a file')
    ap.add_argument('--cold', help='TABLE,..: empty these tables, their TOAST tables and indexes first, and measure that first run')
    ap.add_argument('--size', help='TABLE,..: report the growth of their total size (heap, TOAST, indexes), the bytes a build left')
    ap.add_argument('--db', default=os.environ.get('PGDATABASE', 'postgres'))
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args()
    sql = open(a.file).read() if a.file else a.sql
    if not sql:
        ap.error('a statement or -f FILE')
    from duckdb import Session
    s = Session(os.environ.get('PGPORT', '5432'), 600, None, db=a.db, host=os.environ.get('PGHOST', '127.0.0.1'))
    io = measure(s, sql, a.cold.split(',') if a.cold else None, a.size.split(',') if a.size else None)
    print(json.dumps(io, indent=1) if a.json else show(io))


if __name__ == '__main__':
    main()
