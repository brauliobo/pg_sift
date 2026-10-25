#!/usr/bin/env python3
"""The measured case of the options that docs/options.tsv records: index size, build and write cost with and without an option, and the query it is for.

  PG* in the environment (scripts/pgtest.sh env, a cluster with sift installed)
  bench/options_evidence.py [--rows N] [--only KEY[,KEY]] [--out bench/results/options-evidence.tsv]

Each experiment builds two identical tables that differ in one option (or one setting), alternates the measurements between them so that a drift of the load hits
both, and keeps the median. Small data on purpose (50,000 to 300,000 rows, one scratch cluster under 1.5 GB): the figures are ratios of one option against its
absence, not absolute speeds. The result file has one row per figure: key, experiment, rows, metric, without, with, unit, note. The cells of docs/options.tsv quote it.
"""
import argparse
import os
import re
import statistics
import subprocess
import sys

ENV = dict(os.environ, LC_ALL='C', PGOPTIONS='-c client_min_messages=warning -c max_parallel_workers_per_gather=0 -c sift.async_maintenance=off')
ROWS = []


def psql(sql, db):
    r = subprocess.run(['psql', '-X', '-q', '-At', '-v', 'ON_ERROR_STOP=1', '-d', db], input=sql, capture_output=True, text=True, env=ENV)
    if r.returncode:
        sys.exit('psql: %s\n%s' % (r.stderr.strip(), sql[:400]))
    return r.stdout


def timed(db, stmts):
    """-> [ms] of every statement, run in one session"""
    r = subprocess.run(['psql', '-X', '-q', '-At', '-v', 'ON_ERROR_STOP=1', '-d', db], input='\\timing on\n\\o /dev/null\n' + ';\n'.join(stmts) + ';\n',
                       capture_output=True, text=True, env=ENV)
    return [float(x) for x in re.findall(r'^Time: ([\d.]+) ms', r.stdout + r.stderr, re.M)]


def one(db, sql):
    return psql(sql, db).strip()


def size(db, rel):
    return int(one(db, "SELECT pg_relation_size('%s')" % rel))


def tsize(db, rel):
    return int(one(db, "SELECT pg_total_relation_size('%s')" % rel))


def row(key, exp, n, metric, a, b, unit, note=''):
    ROWS.append((key, exp, n, metric, a, b, unit, note))
    print('%-34s %-18s %-26s %12s %12s %s %s' % (key, exp, metric, a, b, unit, note), file=sys.stderr)


def pct(a, b):
    return '%+.1f%%' % (100.0 * (b - a) / a)


def ab_query(db, qa, qb, reps=9):
    """alternate the two statements, each run twice in its session (the second run is the one kept: the first pays the backend's cold caches): -> medians (a, b)"""
    ta, tb = [], []
    for i in range(reps):
        t = timed(db, [qa, qb, qa, qb] if i % 2 == 0 else [qb, qa, qb, qa])
        x, y = (t[2], t[3]) if i % 2 == 0 else (t[3], t[2])
        ta.append(x)
        tb.append(y)
    return statistics.median(ta), statistics.median(tb)


def ab_set(db, off, on, q, reps=9):
    """the same statement with a setting off and on, alternating, each twice in its session: -> medians (off, on)"""
    ta, tb = [], []
    for i in range(reps):
        t = timed(db, [off, q, q, on, q, q] if i % 2 == 0 else [on, q, q, off, q, q])
        x, y = (t[2], t[5]) if i % 2 == 0 else (t[5], t[2])
        ta.append(x)
        tb.append(y)
    return statistics.median(ta), statistics.median(tb)


def ab_write(db, sa, sb, rounds=5):
    ta, tb = [], []
    for i in range(rounds):
        if i % 2 == 0:
            t = timed(db, [sa, sb])
            ta.append(t[0])
            tb.append(t[1])
        else:
            t = timed(db, [sb, sa])
            ta.append(t[1])
            tb.append(t[0])
    return statistics.median(ta), statistics.median(tb)


def fresh(db, *tables):
    for t in tables:
        psql('DROP TABLE IF EXISTS %s CASCADE' % t, db)


# the text corpus: 30 words of a vocabulary of 4000 stems (skewed), a quarter in initial capitals, a fifth with an accent, a third with a suffix
WORDS = """(SELECT string_agg(CASE WHEN j %% 4 = 0 THEN initcap(w) WHEN j %% 5 = 0 THEN replace(w, 'w', 'wé') ELSE w END || (ARRAY['', 's', 'ing', 'ed'])[1 + j %% 4], ' ')
  FROM (SELECT j, 'w' || floor(4000 * power((hashint4(%(salt)s * 131 + j) & 2147483647)::float8 / 2147483647, 3))::int AS w FROM generate_series(1, 30) j) x)"""


def corpus(db, name, n, first=1, salt='i'):
    psql("""CREATE TABLE %s AS SELECT i AS id, %s AS body, 'k' || (i %% 50) AS kind FROM generate_series(%d, %d) i""" % (name, WORDS % {'salt': salt}, first, first + n - 1), db)


def two(db, a, b, src):
    psql('CREATE TABLE %s AS TABLE %s; CREATE TABLE %s AS TABLE %s' % (a, src, b, src), db)


def bm25_option(db, key, opt_off, opt_on, n, query, label=None):
    """index size, build time and a batch of inserts for the BM25 index with and without WITH (opt)"""
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')
    corpus(db, 'oe_src', n)
    corpus(db, 'oe_more', 20000, n + 1)
    two(db, 'oe_a', 'oe_b', 'oe_src')
    ba = timed(db, ['CREATE INDEX oe_a_i ON oe_a USING sift (body) %s' % opt_off])[0]
    bb = timed(db, ['CREATE INDEX oe_b_i ON oe_b USING sift (body) %s' % opt_on])[0]
    lab = label or key
    row(key, lab, n, 'index size', size(db, 'oe_a_i'), size(db, 'oe_b_i'), 'bytes', pct(size(db, 'oe_a_i'), size(db, 'oe_b_i')))
    row(key, lab, n, 'build', round(ba), round(bb), 'ms', pct(ba, bb))
    wa, wb = ab_write(db, 'INSERT INTO oe_a SELECT * FROM oe_more', 'INSERT INTO oe_b SELECT * FROM oe_more')
    row(key, lab, n, 'insert 20000 rows', round(wa), round(wb), 'ms', pct(wa, wb))
    if query:
        qa, qb = ab_query(db, query.format(t='oe_a'), query.format(t='oe_b'))
        row(key, lab, n, 'query', round(qa, 2), round(qb, 2), 'ms', query.format(t='T'))
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')


def exp_text(db, n):
    q = "SELECT count(*) FROM {t} WHERE body @@> 'w12 w40'"
    bm25_option(db, 'sift(stemmer)', '', "WITH (stemmer = 'en')", n, "SELECT count(*) FROM {t} WHERE body @@> 'w12ing'", 'stemmer none/en')
    bm25_option(db, 'sift(case_folding)', "WITH (case_folding = false)", '', n, q, 'case_folding off/on')
    bm25_option(db, 'sift(accent_folding)', "WITH (accent_folding = false)", '', n, q, 'accent_folding off/on')
    bm25_option(db, 'sift(prefix_index)', '', 'WITH (prefix_index = 3)', n, "SELECT count(*) FROM {t} WHERE body @@> 'w12*'", 'prefix_index 0/3')
    bm25_option(db, 'sift(prefix_index)', '', 'WITH (prefix_index = 15)', n, "SELECT count(*) FROM {t} WHERE body @@> 'w12*'", 'prefix_index 0/15')


def exp_keyword(db, n):
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')
    psql("CREATE TABLE oe_src AS SELECT i AS id, 'k' || (i %% 2000) AS kind FROM generate_series(1, %d) i; CREATE TABLE oe_more AS SELECT i AS id, 'k' || (i %% 2000) AS kind FROM generate_series(%d, %d) i" % (n, n + 1, n + 20000), db)
    two(db, 'oe_a', 'oe_b', 'oe_src')
    ba = timed(db, ['CREATE INDEX oe_a_i ON oe_a USING sift (kind)'])[0]
    bb = timed(db, ["CREATE INDEX oe_b_i ON oe_b USING sift (kind) WITH (analyzer = 'keyword')"])[0]
    row('sift(analyzer)', 'analyzer text/keyword', n, 'index size', size(db, 'oe_a_i'), size(db, 'oe_b_i'), 'bytes', pct(size(db, 'oe_a_i'), size(db, 'oe_b_i')))
    row('sift(analyzer)', 'analyzer text/keyword', n, 'build', round(ba), round(bb), 'ms', pct(ba, bb))
    wa, wb = ab_write(db, 'INSERT INTO oe_a SELECT * FROM oe_more', 'INSERT INTO oe_b SELECT * FROM oe_more')
    row('sift(analyzer)', 'analyzer text/keyword', n, 'insert 20000 rows', round(wa), round(wb), 'ms', pct(wa, wb))
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')


def exp_fields(db, n):
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')
    for t, first, cnt in (('oe_src', 1, n), ('oe_more', n + 1, 20000)):
        psql("CREATE TABLE %s AS SELECT i AS id, 'w%d w%d' AS title, %s AS body, 'k' || (i %% 50) AS tags FROM generate_series(%d, %d) i"
             % (t, 7, 11, WORDS % {'salt': 'i'}, first, first + cnt - 1), db)
    two(db, 'oe_a', 'oe_b', 'oe_src')
    ba = timed(db, ['CREATE INDEX oe_a_i ON oe_a USING sift (sift.sections(title, body, tags))'])[0]
    bb = timed(db, ["CREATE INDEX oe_b_i ON oe_b USING sift (sift.sections(title, body, tags)) WITH (fields = 'title,body,tags')"])[0]
    row('sift(fields)', 'fields unset/set', n, 'index size', size(db, 'oe_a_i'), size(db, 'oe_b_i'), 'bytes', pct(size(db, 'oe_a_i'), size(db, 'oe_b_i')))
    row('sift(fields)', 'fields unset/set', n, 'build', round(ba), round(bb), 'ms', pct(ba, bb))
    wa, wb = ab_write(db, 'INSERT INTO oe_a SELECT * FROM oe_more', 'INSERT INTO oe_b SELECT * FROM oe_more')
    row('sift(fields)', 'fields unset/set', n, 'insert 20000 rows', round(wa), round(wb), 'ms', pct(wa, wb))
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')


def exp_merge_fanin(db, n):
    """a text index that took 20 batches of inserts: segments to query against merge work"""
    fresh(db, 'oe_src', 'oe_a', 'oe_b')
    corpus(db, 'oe_src', n)
    two(db, 'oe_a', 'oe_b', 'oe_src')
    psql('CREATE INDEX oe_a_i ON oe_a USING sift (body) WITH (merge_fanin = 2, write_buffer_limit = 64); CREATE INDEX oe_b_i ON oe_b USING sift (body) WITH (merge_fanin = 8, write_buffer_limit = 64)', db)
    ta = tb = 0.0
    for r in range(20):
        corpus(db, 'oe_more', 2000, 1000000 + r * 2000)
        t = timed(db, ['INSERT INTO oe_a SELECT * FROM oe_more', 'INSERT INTO oe_b SELECT * FROM oe_more'])
        ta += t[0]
        tb += t[1]
        psql('DROP TABLE oe_more', db)
    row('sift(merge_fanin)', 'fanin 2/8', n, '20 inserts of 2000 rows', round(ta), round(tb), 'ms', pct(ta, tb))
    row('sift(merge_fanin)', 'fanin 2/8', n, 'index size', size(db, 'oe_a_i'), size(db, 'oe_b_i'), 'bytes', pct(size(db, 'oe_a_i'), size(db, 'oe_b_i')))
    qa, qb = ab_query(db, "SELECT count(*) FROM oe_a WHERE body @@> 'w12 w40'", "SELECT count(*) FROM oe_b WHERE body @@> 'w12 w40'")
    row('sift(merge_fanin)', 'fanin 2/8', n, 'query after the batches', round(qa, 2), round(qb, 2), 'ms', pct(qa, qb))
    fresh(db, 'oe_src', 'oe_a', 'oe_b')


def exp_switches(db, n):
    """the BM25 switches: the index scan and the count pushdown against the plan PostgreSQL has without them (sift.score() needs the scan, so the scan is timed without it)"""
    fresh(db, 'oe_src', 'oe_a')
    corpus(db, 'oe_src', n)
    psql('CREATE TABLE oe_a AS TABLE oe_src; CREATE INDEX oe_a_i ON oe_a USING sift (body); VACUUM (FREEZE, ANALYZE) oe_a', db)
    for label, terms in (('rare terms', 'w12 w40'), ('frequent terms', 'w2 w5')):
        x, y = ab_set(db, 'SET sift.enable_scan = off', 'SET sift.enable_scan = on', "SELECT id FROM oe_a WHERE body @@> '%s' ORDER BY id LIMIT 10" % terms)
        row('sift.enable_scan', 'enable_scan off/on', n, 'first 10 ids of a match, %s (no score: it needs the scan)' % label, round(x, 2), round(y, 2), 'ms', pct(x, y))
        x, y = ab_set(db, 'SET sift.enable_count = off', 'SET sift.enable_count = on', "SELECT count(*) FROM oe_a WHERE body @@> '%s'" % terms)
        row('sift.enable_count', 'enable_count off/on', n, 'count(*) of a match, %s' % label, round(x, 2), round(y, 2), 'ms', pct(x, y))
    fresh(db, 'oe_src', 'oe_a')


def exp_async(db, n):
    """20 inserts of 2,000 rows into a text index that seals its write buffer at 64 kB: the writer merges itself (off) or leaves it to the worker (on)"""
    fresh(db, 'oe_src', 'oe_a', 'oe_b')
    corpus(db, 'oe_src', n)
    two(db, 'oe_a', 'oe_b', 'oe_src')
    psql('CREATE INDEX oe_a_i ON oe_a USING sift (body) WITH (write_buffer_limit = 64); CREATE INDEX oe_b_i ON oe_b USING sift (body) WITH (write_buffer_limit = 64)', db)
    ta = tb = 0.0
    for r in range(20):
        corpus(db, 'oe_more', 2000, 1000000 + r * 2000)
        t = timed(db, ['SET sift.async_maintenance = off', 'INSERT INTO oe_a SELECT * FROM oe_more', 'SET sift.async_maintenance = on', 'INSERT INTO oe_b SELECT * FROM oe_more'])
        ta += t[1]
        tb += t[3]
        psql('DROP TABLE oe_more', db)
    row('sift.async_maintenance', 'async_maintenance off/on', n, '20 inserts of 2000 rows', round(ta), round(tb), 'ms', pct(ta, tb))
    fresh(db, 'oe_src', 'oe_a', 'oe_b')


# ---- sift_col ----
COLSRC = """CREATE TABLE %s AS SELECT i AS id, (i * 7919) %% 5000 AS k, i %% 7 AS g, i %% 100 AS v, md5(i::text) AS s, date '2024-01-01' + i %% 365 AS d, repeat('x', 60) AS pad FROM generate_series(%d, %d) i"""


def colsrc(db, name, n, first=1):
    psql(COLSRC % (name, first, first + n - 1), db)


def col_pair(db, key, exp, n, idx_a, idx_b, query, extra_write=True):
    """two copies of one table, a sift_col index each: size, build, a batch of inserts and of updates, the query"""
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')
    colsrc(db, 'oe_src', n)
    colsrc(db, 'oe_more', 20000, n + 1)
    two(db, 'oe_a', 'oe_b', 'oe_src')
    ba = timed(db, [idx_a.format(t='oe_a', i='oe_a_i')])[0]
    bb = timed(db, [idx_b.format(t='oe_b', i='oe_b_i')])[0]
    psql('VACUUM (FREEZE, ANALYZE) oe_a, oe_b', db)
    row(key, exp, n, 'index size', size(db, 'oe_a_i'), size(db, 'oe_b_i'), 'bytes', pct(size(db, 'oe_a_i'), size(db, 'oe_b_i')))
    row(key, exp, n, 'build', round(ba), round(bb), 'ms', pct(ba, bb))
    if query:
        qa, qb = ab_query(db, query.format(t='oe_a'), query.format(t='oe_b'))
        row(key, exp, n, 'query', round(qa, 2), round(qb, 2), 'ms', query.format(t='T'))
    if extra_write:
        wa, wb = ab_write(db, 'INSERT INTO oe_a SELECT * FROM oe_more', 'INSERT INTO oe_b SELECT * FROM oe_more')
        row(key, exp, n, 'insert 20000 rows', round(wa), round(wb), 'ms', pct(wa, wb))
        ua, ub = ab_write(db, 'UPDATE oe_a SET v = v + 1 WHERE id % 10 = 0', 'UPDATE oe_b SET v = v + 1 WHERE id % 10 = 0')
        row(key, exp, n, 'update 10% of the rows', round(ua), round(ub), 'ms', pct(ua, ub))
    fresh(db, 'oe_src', 'oe_a', 'oe_b', 'oe_more')


BASE = 'CREATE INDEX {i} ON {t} USING sift_col (id, k, g, v, s, d) %s'


def exp_key_cut_rows(db, n):
    q = 'SELECT g, count(*), sum(v) FROM {t} WHERE k BETWEEN 100 AND 140 GROUP BY g'
    for rows in (256, 4096):
        col_pair(db, 'sift_col(key_cut_rows)', 'key_cut_rows 0/%d' % rows, n, BASE % "WITH (key_cut = 'k')", BASE % ("WITH (key_cut = 'k', key_cut_rows = %d)" % rows), q)


def exp_point_columns(db, n):
    col_pair(db, 'sift_col(point_columns)', 'point_columns none/k', n, BASE % '', BASE % "WITH (point_columns = 'k')", 'SELECT count(*) FROM {t} WHERE k IN (17, 4242, 3)')


def exp_dead_rewrite(db, n):
    """update 30% of the rows, VACUUM: the rows of the units that carry a dead row against a lone segment rewritten"""
    fresh(db, 'oe_src', 'oe_a', 'oe_b')
    colsrc(db, 'oe_src', n)
    two(db, 'oe_a', 'oe_b', 'oe_src')
    psql(BASE.format(t='oe_a', i='oe_a_i') % 'WITH (dead_rewrite_pct = 100)', db)
    psql(BASE.format(t='oe_b', i='oe_b_i') % 'WITH (dead_rewrite_pct = 10)', db)
    psql('VACUUM (FREEZE, ANALYZE) oe_a, oe_b', db)
    for r in range(3):
        psql('UPDATE oe_a SET v = v + 1 WHERE id % 10 < 3; UPDATE oe_b SET v = v + 1 WHERE id % 10 < 3', db)
        va, vb = timed(db, ['VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) oe_a', 'VACUUM (FREEZE, DISABLE_PAGE_SKIPPING) oe_b'])
    row('sift_col(dead_rewrite_pct)', 'dead_rewrite_pct 100/10', n, 'VACUUM after the third round', round(va), round(vb), 'ms', pct(va, vb))
    row('sift_col(dead_rewrite_pct)', 'dead_rewrite_pct 100/10', n, 'index size', size(db, 'oe_a_i'), size(db, 'oe_b_i'), 'bytes', pct(size(db, 'oe_a_i'), size(db, 'oe_b_i')))
    qa, qb = ab_query(db, 'SELECT g, sum(v) FROM oe_a GROUP BY g', 'SELECT g, sum(v) FROM oe_b GROUP BY g')
    row('sift_col(dead_rewrite_pct)', 'dead_rewrite_pct 100/10', n, 'GROUP BY after the churn', round(qa, 2), round(qb, 2), 'ms', pct(qa, qb))
    fresh(db, 'oe_src', 'oe_a', 'oe_b')


def exp_vis_page_cache(db, n):
    fresh(db, 'oe_src', 'oe_a')
    colsrc(db, 'oe_src', n)
    psql("CREATE TABLE oe_a AS TABLE oe_src; " + BASE.format(t='oe_a', i='oe_a_i') % "WITH (key_cut = 'k')" + '; VACUUM (FREEZE, ANALYZE) oe_a', db)
    q = 'SELECT g, count(*), sum(v) FROM oe_a WHERE k BETWEEN 100 AND 400 GROUP BY g'
    x, y = ab_set(db, 'SET sift.vis_page_cache = off', 'SET sift.vis_page_cache = on', q)
    row('sift.vis_page_cache', 'vis_page_cache off/on', n, 'key-cut range query', round(x, 2), round(y, 2), 'ms', pct(x, y))
    fresh(db, 'oe_src', 'oe_a')


def exp_compression(db, n):
    fresh(db, 'oe_src')
    colsrc(db, 'oe_src', n)
    for opt in ('auto', 'none', 'lz4', 'zstd:3'):
        fresh(db, 'oe_a')
        psql('CREATE TABLE oe_a AS TABLE oe_src', db)
        b = timed(db, [BASE.format(t='oe_a', i='oe_a_i') % ("WITH (compression = '%s')" % opt)])[0]
        psql('VACUUM (FREEZE, ANALYZE) oe_a', db)
        t = timed(db, ['SELECT g, sum(v), count(DISTINCT s) FROM oe_a GROUP BY g'] * 6)
        row('sift_col(compression)', 'compression %s' % opt, n, 'size / build ms / scan ms', size(db, 'oe_a_i'), round(b), 'bytes, ms', 'scan %.1f ms' % statistics.median(t[1:]))
    fresh(db, 'oe_src', 'oe_a')


def exp_key_columns(db, n):
    col_pair(db, 'sift_col(key_columns)', 'key_columns off/auto', n, BASE % "WITH (key_columns = 'off')", BASE % '', 'SELECT count(DISTINCT v) FROM {t}', False)


def exp_key_columns_named(db, n):
    col_pair(db, 'sift_col(key_columns)', 'key_columns off/named k', n, BASE % "WITH (key_columns = 'off')", BASE % "WITH (key_columns = 'k')", 'SELECT count(*) FROM {t} WHERE k = 17')


# ---- sift_table ----
def table_pair(db, key, exp, n, ddl, sa, sb, query, cols="i, (i * 7919) % 5000, i % 7, i % 100, md5(i::text), repeat('x', 60)"):
    """two sift_tables loaded in 200 batches with the setting off and on: load time, size, query"""
    fresh(db, 'oe_a', 'oe_b')
    psql(ddl.format(t='oe_a') + ';' + ddl.format(t='oe_b'), db)
    ta = tb = 0.0
    step = n // 20
    for r in range(20):
        lo = r * step + 1
        ins = 'INSERT INTO {t} SELECT %s FROM generate_series(%d, %d) i' % (cols.replace('%', '%%'), lo, lo + step - 1)
        ins = ins.replace('%%', '%')
        t = timed(db, [sa, ins.format(t='oe_a'), sb, ins.format(t='oe_b')])
        ta += t[1]
        tb += t[3]
    row(key, exp, n, 'load in 20 statements', round(ta), round(tb), 'ms', pct(ta, tb))
    psql('VACUUM (FREEZE, ANALYZE) oe_a, oe_b', db)
    row(key, exp, n, 'table size', tsize(db, 'oe_a'), tsize(db, 'oe_b'), 'bytes', pct(tsize(db, 'oe_a'), tsize(db, 'oe_b')))
    qa, qb = ab_query(db, query.format(t='oe_a'), query.format(t='oe_b'))
    row(key, exp, n, 'query', round(qa, 2), round(qb, 2), 'ms', query.format(t='T'))
    fresh(db, 'oe_a', 'oe_b')


TBL = 'CREATE TABLE {t} (id int, k int, g int, v int, s text, pad text) USING sift_table'


def exp_table(db, n):
    q = 'SELECT g, sum(v), count(*) FROM {t} WHERE k < 2500 GROUP BY g'
    table_pair(db, 'sift.table_inline_promote', 'inline_promote off/on', n, TBL, 'SET sift.table_inline_promote = off', 'SET sift.table_inline_promote = on', q)
    table_pair(db, 'sift.table_write_buffer_limit', 'write_buffer_limit 2MB/8MB', n, TBL, "SET sift.table_write_buffer_limit = '2MB'", "SET sift.table_write_buffer_limit = '8MB'", q)
    table_pair(db, 'sift.table_dict_lookup', 'dict_lookup off/on', n, TBL, 'SET sift.table_dict_lookup = off', 'SET sift.table_dict_lookup = on',
               "SELECT count(*) FROM {t} WHERE s = md5('4242')")
    table_pair(db, 'sift.table_dict_lookup', 'dict_lookup off/on, 2 columns', n, 'CREATE TABLE {t} (id int, s text) USING sift_table', 'SET sift.table_dict_lookup = off', 'SET sift.table_dict_lookup = on',
               "SELECT count(*) FROM {t} WHERE s = md5('4242')", 'i, md5(i::text)')
    table_pair(db, 'sift.table_compression', 'compression auto/zstd:3', n, TBL, "SET sift.table_compression = 'auto'", "SET sift.table_compression = 'zstd:3'", q)


def exp_prefetch(db, n):
    fresh(db, 'oe_src', 'oe_a')
    corpus(db, 'oe_src', n)
    psql('CREATE TABLE oe_a AS TABLE oe_src; CREATE INDEX oe_a_i ON oe_a USING sift (body); VACUUM (FREEZE, ANALYZE) oe_a', db)
    q = "SELECT count(*), sum(length(body)) FROM oe_a WHERE body @@> 'w12' AND length(body) > 10"
    x, y = ab_set(db, 'SET sift.prefetch = off', 'SET sift.prefetch = on', q)
    row('sift.prefetch', 'prefetch off/on', n, 'unordered scan, warm cache', round(x, 2), round(y, 2), 'ms', pct(x, y) + ' (the page cache cannot be dropped here)')
    fresh(db, 'oe_src', 'oe_a')


EXPS = {'text': exp_text, 'keyword': exp_keyword, 'fields': exp_fields, 'merge_fanin': exp_merge_fanin, 'key_cut_rows': exp_key_cut_rows, 'point_columns': exp_point_columns,
        'dead_rewrite': exp_dead_rewrite, 'vis_page_cache': exp_vis_page_cache, 'compression': exp_compression, 'key_columns': exp_key_columns, 'key_columns_named': exp_key_columns_named, 'table': exp_table,
        'prefetch': exp_prefetch, 'async': exp_async, 'switches': exp_switches}

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--only', default='')
    ap.add_argument('--out', default='')
    a = ap.parse_args()
    db = 'oe_%d' % os.getpid()
    psql('CREATE DATABASE ' + db, 'postgres')
    try:
        psql('CREATE EXTENSION pg_sift', db)
        for name, fn in EXPS.items():
            if not a.only or name in a.only.split(','):
                fn(db, a.rows)
    finally:
        psql('DROP DATABASE IF EXISTS %s WITH (FORCE)' % db, 'postgres')
    if a.out:
        with open(a.out, 'a') as f:
            for r in ROWS:
                f.write('\t'.join(str(x) for x in r) + '\n')
