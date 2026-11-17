#!/usr/bin/env python3
"""SQL surface sweep: no call of the sift schema may crash the backend or raise an internal error, whatever its arguments.

  test/fuzz/surface_sweep.py [--tier quick|full] [--only RE] [--budget SEC] [--db NAME] [--list] [--role]

Every function, procedure, aggregate and view that pg_sift installs (the sift schema and the objects of the extension) is called with all-NULL arguments, with
each argument in turn set to a hostile value of its type (NULL, empty, huge, NaN, a regclass that is dropped, a heap, a view, a partitioned table, a temp table,
a table of another schema, a 1 MB text, a nested array, ...) while the others are valid, and with every argument set to the same kind of hostile value. Each call
runs in a plpgsql block with an EXCEPTION clause (a subtransaction) that reports the SQLSTATE; a sample also runs after a ROLLBACK TO SAVEPOINT, and as an unprivileged
role (--role). A part then creates every index and table option with hostile values on tables of every kind of contents (empty, NULLs, all NULL, one row, dropped columns).
A failure is a signal (the session ends), SQLSTATE XX000 or an internal message, and a timeout. Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT).
"""
import argparse
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from shapes_matrix import ENV, Crash, Session, wait_ready, log_mark, log_scan  # noqa: E402

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
BAD = re.compile(r'cache lookup failed|could not find|unrecognized (node|object|type|reloption)|invalid memory|variable not found|no relation entry|'
                 r'negative bitmapset|was not found|bogus|unexpected (null|type|node)|could not open relation|could not read block|invalid page|corrupt')
ALLOW_MSG = re.compile(r'unrecognized parameter|unrecognized configuration parameter')

FIXTURE = r"""
SET client_min_messages = warning;
DROP SCHEMA IF EXISTS fx CASCADE;
DROP SCHEMA IF EXISTS fx2 CASCADE;
CREATE SCHEMA fx; CREATE SCHEMA fx2;
CREATE TABLE fx.t (id int PRIMARY KEY, g int, v int, amount numeric(10,2), kind text, body text, d date, ts timestamptz);
INSERT INTO fx.t SELECT i, i % 5, CASE WHEN i % 4 = 0 THEN NULL ELSE i % 9 END, i * 1.5, 'k' || i % 3, 'word' || i % 4 || ' text', date '2026-01-01' + i, timestamptz '2026-01-01' + i * interval '1 hour'
	FROM generate_series(1, 120) i;
CREATE INDEX t_col ON fx.t USING sift_col (id, g, v, amount, kind, d, ts);
CREATE INDEX t_bm ON fx.t USING sift (body);
CREATE INDEX t_bt ON fx.t (g);
CREATE TABLE fx.heap (id int, g int);
INSERT INTO fx.heap SELECT i, i % 5 FROM generate_series(1, 20) i;
CREATE TABLE fx.tbl (id int, g int, v int) USING sift_table;
INSERT INTO fx.tbl SELECT i, i % 5, i FROM generate_series(1, 50) i;
CREATE VIEW fx.vw AS SELECT * FROM fx.t;
CREATE MATERIALIZED VIEW fx.mv AS SELECT id, g FROM fx.t;
CREATE TABLE fx.part (id int, g int) PARTITION BY RANGE (id);
CREATE TABLE fx.part_1 PARTITION OF fx.part FOR VALUES FROM (0) TO (100);
INSERT INTO fx.part SELECT i, i % 3 FROM generate_series(1, 50) i;
CREATE INDEX part_col ON fx.part USING sift_col (id, g);
CREATE TABLE fx2.other (id int, g int);
INSERT INTO fx2.other SELECT i, i FROM generate_series(1, 10) i;
CREATE SEQUENCE fx.sq;
CREATE TEMP TABLE fx_temp (id int, g int);
INSERT INTO fx_temp SELECT i, i FROM generate_series(1, 10) i;
CREATE INDEX fx_temp_col ON fx_temp USING sift_col (id, g);
CREATE TABLE fx.gone (id int);
CREATE TEMP TABLE fx_oids AS SELECT 'fx.gone'::regclass::oid AS gone, 'fx.t_col'::regclass::oid AS col;
DROP TABLE fx.gone;
ANALYZE fx.t; ANALYZE fx.tbl;
SET client_min_messages = notice;
"""

OIDS = {'regclass': ["'fx.t'::regclass", "'fx.t_col'::regclass", "'fx.t_bm'::regclass", "'fx.t_bt'::regclass", "'fx.heap'::regclass", "'fx.tbl'::regclass",
                     "'fx.vw'::regclass", "'fx.mv'::regclass", "'fx.part'::regclass", "'fx.part_col'::regclass", "'fx.sq'::regclass", "'fx_temp'::regclass",
                     "'fx2.other'::regclass", "(SELECT gone FROM fx_oids)::regclass", '0::oid::regclass', '4294967295::oid::regclass', "'pg_class'::regclass",
                     "'fx.t_pkey'::regclass"]}
BIG = "repeat('x', 1000000)"
POOL = {
    'text': ["''", "'id'", "'  '", "'ñandú 日本語 😀'", "E'a\\\\0b'", "'a''b\"c\\\\'", "'id, g'", "'nokey'", "'fx.t'", "'g'", "'word1'", "'word1 & word2'", BIG,
             "'auto'", "'off'", "'-1'", "'{}'", "'*'", "'id desc'", "'1; DROP TABLE fx.t'", "'(('"],
    'int4': ['0', '1', '-1', '2147483647', '-2147483648', '100', '4096'],
    'int8': ['0', '1', '-1', '9223372036854775807', '-9223372036854775808', '4294967296'],
    'int2': ['0', '1', '-1', '32767', '-32768'],
    'float8': ["0", "-1", "'NaN'", "'Infinity'", "'-Infinity'", "1e308", "1e-320", "0.5"],
    'float4': ["0", "-1", "'NaN'", "'Infinity'", "'-Infinity'", "3.4e38", "0.5"],
    'numeric': ["0", "-1", "'NaN'", "1e100", "0.5", "'Infinity'"],
    'bool': ['true', 'false'],
    'jsonb': ["'{}'", "'[]'", "'null'", "'\"\"'", "'[[1,[2,[3]]]]'", "'{\"a\":{\"b\":{\"c\":[]}}}'", "to_jsonb(repeat('x', 1000000))", "'{\"id\": null}'", "'1'"],
    'json': ["'{}'", "'[]'", "'null'", "'[[1,[2,[3]]]]'"],
    'bytea': ["''", "'\\x00'", "decode(repeat('ab', 1000000), 'hex')"],
    'uuid': ["'00000000-0000-0000-0000-000000000000'"],
    'tid': ["'(0,0)'", "'(0,1)'", "'(4294967295,65535)'"],
    'timestamptz': ["'infinity'", "'-infinity'", "'0001-01-01'", "'epoch'", "now()"],
    'timestamp': ["'infinity'", "'-infinity'", "'0001-01-01'", "'epoch'"],
    'date': ["'infinity'", "'-infinity'", "'0001-01-01'", "'2026-01-01'"],
    'interval': ["'0'", "'-1 day'", "'1 year'", "'178000000 years'"],
    'oid': ['0', '4294967295', "'fx.t'::regclass::oid", "'fx.t_col'::regclass::oid", "(SELECT gone FROM fx_oids)", '1259'],
    'text[]': ["'{}'", "ARRAY[NULL]::text[]", "'{a,b}'", "ARRAY[['a','b'],['c','d']]", f"ARRAY[{BIG}]", "ARRAY['id','g']", "ARRAY['nokey']", "ARRAY(SELECT 'x' FROM generate_series(1, 100000))"],
    'int4[]': ["'{}'", "ARRAY[NULL]::int[]", "'{1,2}'", "ARRAY[[1,2],[3,4]]", "ARRAY(SELECT i FROM generate_series(1, 100000) i)", "'{-1,0,2147483647}'"],
    'int8[]': ["'{}'", "ARRAY[NULL]::bigint[]", "'{1,2}'", "ARRAY[[1,2],[3,4]]"],
    'float8[]': ["'{}'", "ARRAY['NaN']::float8[]", "'{1,2}'"],
    'oid[]': ["'{}'", "ARRAY[0]::oid[]", "ARRAY['fx.t'::regclass::oid]"],
    'regclass[]': ["'{}'", "ARRAY['fx.t']::regclass[]"],
}
VALID_TBL = {'regclass': "'fx.t'::regclass", 'text': "'id'", 'oid': "'fx.t'::regclass::oid", 'int4': '3', 'int8': '3', 'int2': '3', 'float8': '0.5', 'float4': '0.5',
             'numeric': '1', 'bool': 'true', 'jsonb': "'{}'", 'json': "'{}'", 'text[]': "ARRAY['id','g']", 'int4[]': "'{1,2}'", 'int8[]': "'{1,2}'",
             'bytea': "'\\x00'", 'uuid': "'00000000-0000-0000-0000-000000000000'", 'tid': "'(0,1)'", 'timestamptz': 'now()', 'timestamp': "'2026-01-01'", 'date': "'2026-01-01'",
             'interval': "'1 day'", 'float8[]': "'{1,2}'", 'oid[]': "ARRAY[0]::oid[]", 'regclass[]': "ARRAY['fx.t']::regclass[]"}
VALID_IDX = dict(VALID_TBL, regclass="'fx.t_col'::regclass", oid="'fx.t_col'::regclass::oid")
VALID_BM = dict(VALID_TBL, regclass="'fx.t_bm'::regclass", oid="'fx.t_bm'::regclass::oid", text="'word1'")
VALID_TAB = dict(VALID_TBL, regclass="'fx.tbl'::regclass", oid="'fx.tbl'::regclass::oid")
BASES = [('tbl', VALID_TBL), ('idx', VALID_IDX), ('bm25', VALID_BM), ('table', VALID_TAB)]
SKIP_NAME = re.compile(r'^_test_|^test_|_test$')


def pool(t):
    """the hostile values of a type, as SQL expressions of that type"""
    t = {'int': 'int4', 'integer': 'int4', 'bigint': 'int8', 'smallint': 'int2', 'double precision': 'float8', 'real': 'float4', 'boolean': 'bool',
         'character varying': 'text', 'varchar': 'text', 'text[]': 'text[]', 'integer[]': 'int4[]', 'bigint[]': 'int8[]', 'timestamp with time zone': 'timestamptz',
         'timestamp without time zone': 'timestamp', 'double precision[]': 'float8[]', 'character varying[]': 'text[]', 'anyelement': 'text', 'anyarray': 'text[]',
         'anycompatible': 'text', 'anycompatiblearray': 'text[]', '"char"': 'text', 'name': 'text', 'oid': 'oid', 'regclass': 'regclass', 'tid': 'tid'}.get(t, t)
    if t == 'regclass':
        return [(f'r{i}', v) for i, v in enumerate(OIDS['regclass'])]
    base = POOL.get(t)
    if base is None and t.endswith('range'):
        return [('empty', f"'empty'::{t}")]
    if base is None:
        return []
    return [(f'p{i}', f'({v})::{t}') for i, v in enumerate(base)]


def valid(t, base):
    t = {'int': 'int4', 'integer': 'int4', 'bigint': 'int8', 'smallint': 'int2', 'double precision': 'float8', 'real': 'float4', 'boolean': 'bool',
         'character varying': 'text', 'timestamp with time zone': 'timestamptz', 'timestamp without time zone': 'timestamp', 'anyelement': 'text',
         'anyarray': 'text[]', 'integer[]': 'int4[]', 'bigint[]': 'int8[]', 'double precision[]': 'float8[]', 'name': 'text'}.get(t, t)
    v = base.get(t)
    return f'({v})::{t}' if v else f'NULL::{t}'


def objects(sess):
    sql = """SELECT p.oid, p.proname, p.prokind, p.proretset, p.provariadic <> 0, p.proisstrict,
		coalesce((SELECT string_agg(format_type(t, NULL), ',' ORDER BY o) FROM unnest(p.proargtypes::oid[]) WITH ORDINALITY u(t, o)), '')
	FROM pg_proc p WHERE p.pronamespace = 'sift'::regnamespace
		OR p.oid IN (SELECT objid FROM pg_depend WHERE classid = 'pg_proc'::regclass AND refclassid = 'pg_extension'::regclass AND deptype = 'e'
			AND refobjid IN (SELECT oid FROM pg_extension WHERE extname = 'pg_sift'))
	ORDER BY p.proname, p.oid;"""
    out = []
    for l in sess.run(sql):
        f = l.split('|')
        if len(f) == 7:
            out.append(dict(oid=f[0], name=f[1], kind=f[2], retset=f[3] == 't', variadic=f[4] == 't', strict=f[5] == 't', args=f[6].split(',') if f[6] else []))
    return out


def qualified(sess, oid):
    return sess.run(f"SELECT p.oid::regprocedure::text, pg_catalog.quote_ident(n.nspname) || '.' || pg_catalog.quote_ident(p.proname) FROM pg_proc p "
                    f"JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.oid = {oid};")[0].split('|')[1]


def call_sql(ob, name, args):
    a = ', '.join(args)
    if ob['kind'] == 'p':
        return f'CALL {name}({a})'
    if ob['kind'] == 'a':
        return f'PERFORM {name}({a}) FROM generate_series(1, 3)'
    if ob['retset']:
        return f'PERFORM count(*) FROM {name}({a})'
    return f'PERFORM {name}({a})'


def wrap(stmt, label):
    body = stmt if stmt.startswith(('CALL', 'PERFORM', 'EXECUTE')) else 'PERFORM ' + stmt
    return (f"DO $do$ BEGIN {body}; RAISE NOTICE '@@OK {label}'; EXCEPTION WHEN OTHERS THEN RAISE NOTICE '@@ERR {label} % %', SQLSTATE, SQLERRM; END $do$;")


def call_sets(ob, args_t, cap):
    """the argument lists of the sweep of one function: (label, [sql expression]) in a fixed order"""
    n = len(args_t)
    out = [('allnull', [f'NULL::{t}' for t in args_t])]
    pools = [pool(t) for t in args_t]
    for bi, (bname, base) in enumerate(BASES):
        for i, t in enumerate(args_t):
            for lab, v in pools[i]:
                args = [valid(x, base) for x in args_t]
                args[i] = v
                out.append((f'{bname}.a{i + 1}.{lab}', args))
            args = [valid(x, base) for x in args_t]
            args[i] = f'NULL::{t}'
            out.append((f'{bname}.a{i + 1}.null', args))
    width = max((len(p) for p in pools), default=0)
    for j in range(width):
        out.append((f'all.p{j}', [pools[i][j % len(pools[i])][1] if pools[i] else f'NULL::{args_t[i]}' for i in range(n)]))
    for j in range(n):
        out.append((f'nullone.{j + 1}', [f'NULL::{t}' if i == j else valid(t, VALID_TBL) for i, t in enumerate(args_t)]))
    if len(out) > cap:
        step = len(out) / cap
        out = [out[int(i * step)] for i in range(cap)]
    return out


def classify(lines):
    """[(label, status, sqlstate, message)]"""
    res = []
    for l in lines:
        m = re.match(r'(?:NOTICE|WARNING):\s+(?:[0-9A-Z]{5}: )?@@(OK|ERR) (\S+)(?: ([0-9A-Z]{5}) (.*))?', l)
        if m:
            res.append((m.group(2), m.group(1), m.group(3), m.group(4)))
        else:
            m = re.match(r'ERROR:\s+([0-9A-Z]{5}): (.*)', l)
            if m:
                res.append(('?', 'ERR', m.group(1), m.group(2)))
    return res


def bad(state, msg):
    return state == 'XX000' or (msg and BAD.search(msg) and not ALLOW_MSG.search(msg)) or state in ('57014',)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tier', default='quick', choices=['quick', 'full'])
    ap.add_argument('--only', default='')
    ap.add_argument('--budget', type=int, default=0)
    ap.add_argument('--db', default='surface')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--no-ddl', action='store_true')
    ap.add_argument('--no-calls', action='store_true')
    a = ap.parse_args()
    db = a.db
    subprocess.run(['psql', '-X', '-q', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {db}', '-c', f'CREATE DATABASE {db}'], env=ENV, check=True, capture_output=True)
    subprocess.run(['psql', '-X', '-q', '-d', db, '-c', 'CREATE EXTENSION pg_sift'], env=ENV, check=True, capture_output=True)
    sess = Session(db)
    sess.run("SET client_min_messages = notice; SET statement_timeout = '15s'; SET lock_timeout = '2s';")
    obs = [o for o in objects(sess) if not SKIP_NAME.search(o['name']) and re.search(a.only, o['name'])]
    cap = 60 if a.tier == 'quick' else 400
    t0 = time.time()
    stats = dict(functions=len(obs), calls=0, errors_clean=0, ok=0, crashes=0, skipped=0)
    fails = []
    mark = log_mark()
    states = {}

    def reconnect():
        nonlocal sess
        sess.close()
        if not wait_ready(db):
            sys.exit('the server did not come back after the crash')
        sess = Session(db)
        sess.run("SET client_min_messages = notice; SET statement_timeout = '15s'; SET lock_timeout = '2s';")

    def reset_fixture():
        nonlocal sess
        try:
            sess.run(FIXTURE)
        except Crash as e:
            reconnect()
            sess.run(FIXTURE)

    def run_batch(ob, name, calls, label_pfx, prelude='', after=''):
        nonlocal sess
        for lab, args in calls:
            stmt = call_sql(ob, name, args)
            text = prelude + wrap(stmt, lab) + after
            stats['calls'] += 1
            try:
                lines = sess.run(text)
            except Crash as e:
                stats['crashes'] += 1
                fails.append(('CRASH', ob, lab, str(e)))
                print(f'FAIL CRASH {label_pfx}{name}({", ".join(ob["args"])}) call {lab}: {e}\n  {stmt[:400]}', flush=True)
                reconnect()
                reset_fixture()
                continue
            for l, st, state, msg in classify(lines):
                if st == 'OK':
                    stats['ok'] += 1
                    continue
                states[state] = states.get(state, 0) + 1
                if bad(state, msg):
                    fails.append(('ERROR', ob, lab, f'{state}: {msg}'))
                    print(f'FAIL {"TIMEOUT" if state == "57014" else "INTERNAL"} {label_pfx}{name}({", ".join(ob["args"])}) call {lab}: {state}: {(msg or "")[:200]}\n  {stmt[:400]}', flush=True)
                else:
                    stats['errors_clean'] += 1

    if not a.no_calls:
        reset_fixture()
        for k, ob in enumerate(obs):
            if a.budget and time.time() - t0 > a.budget:
                stats['skipped'] += 1
                continue
            try:
                name = qualified(sess, ob['oid'])
            except Crash:
                reconnect()
                continue
            ob['display'] = name
            calls = call_sets(ob, ob['args'], cap)
            if a.list:
                print(f'{name}({", ".join(ob["args"])}) kind={ob["kind"]} strict={ob["strict"]} calls={len(calls)}')
                continue
            run_batch(ob, name, calls, '')
            reset_fixture()
            sample = calls[::7]
            run_batch(ob, name, sample, 'after-rollback ', prelude="BEGIN; SAVEPOINT s0; SELECT 1/0; ROLLBACK TO s0;\n", after='\nROLLBACK;')
        if not a.list:
            for ob in obs:
                if ob.get('display'):
                    reset_fixture()
                    sess.run("DROP ROLE IF EXISTS ps_nobody; CREATE ROLE ps_nobody; GRANT USAGE ON SCHEMA fx, fx2, sift TO ps_nobody; GRANT SELECT ON ALL TABLES IN SCHEMA fx TO ps_nobody;")
                    break
            for ob in obs:
                if a.budget and time.time() - t0 > a.budget:
                    break
                name = ob.get('display')
                if not name:
                    continue
                calls = call_sets(ob, ob['args'], 12 if a.tier == 'quick' else 60)
                run_batch(ob, name, calls, 'as-unprivileged ', prelude='SET ROLE ps_nobody;\n', after='\nRESET ROLE;')
    if not a.no_ddl and not a.list:
        ddl_sweep(sess, a, stats, fails, reconnect, reset_fixture, states)
    sess.close()
    up = wait_ready(db, 10)
    sig = log_scan(mark)
    nonstrict = [o['name'] for o in obs if not o['strict'] and o['args']]
    print(f'surface: tier={a.tier} functions={stats["functions"]} calls={stats["calls"]} ok={stats["ok"]} clean_errors={stats["errors_clean"]} crashes={stats["crashes"]} '
          f'failures={len(fails)} seconds={time.time() - t0:.0f} server_up={up} log_signals={sig}')
    print('sqlstates: ' + ' '.join(f'{k}={v}' for k, v in sorted(states.items(), key=lambda x: -x[1])[:14]))
    print(f'not strict ({len(nonstrict)}): ' + ', '.join(sorted(set(nonstrict))[:40]))
    subprocess.run(['psql', '-X', '-q', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {db}'], env=ENV, capture_output=True)
    return 1 if fails or not up or sig else 0


def options():
    """(kind, name, applies) from docs/options.tsv: kind is sift, sift_col or sift_table"""
    out = []
    with open(os.path.join(REPO, 'docs', 'options.tsv')) as f:
        for line in f:
            if line.startswith('#'):
                continue
            c = line.rstrip('\n').split('\t')
            m = re.match(r'^(sift|sift_col)\((\w+)\)$', c[0])
            if m and len(c) > 1 and c[1] != '-':
                out.append((m.group(1), m.group(2), c[1]))
    return out


DDL_VALUES = ["''", "'NULL'", "'null'", "'none'", "'off'", "'on'", "'auto'", "'x'", "'-1'", "'0'", "'1'", "'2147483648'", "'1e999'", "'id, id'", "'nokey'", "'a b c'",
              "'id -> nowhere(id)'", "'id: id -> fx.t(id, g)'", "repeat('x', 100000)", "'ñ日本'", "'(('", "'1,2,3'", "'ctid'", "'id'", "'g, v'", "'4096'", "'1000000000'"]
SCEN = {
    'empty': "CREATE TABLE ddl_t (id int PRIMARY KEY, g int, v int, amount numeric(10,2), kind text, body text, d date, ts timestamptz, tags text[], a jsonb)",
    'nulls': "CREATE TABLE ddl_t (id int PRIMARY KEY, g int, v int, amount numeric(10,2), kind text, body text, d date, ts timestamptz, tags text[], a jsonb); "
             "INSERT INTO ddl_t SELECT i, NULLIF(i % 3, 0), NULLIF(i % 4, 0), NULLIF(i % 5, 0), NULLIF('k' || i % 3, 'k0'), NULLIF('w' || i, 'w5'), NULL, "
             "NULLIF(timestamptz '2026-01-01' + i * interval '1 h', timestamptz '2026-01-01 03:00'), NULLIF(ARRAY['a'], ARRAY['a']), NULLIF('{\"a\": 1}'::jsonb, '{\"a\": 1}') "
             "FROM generate_series(1, 40) i",
    'allnull': "CREATE TABLE ddl_t (id int PRIMARY KEY, g int, v int, amount numeric(10,2), kind text, body text, d date, ts timestamptz, tags text[], a jsonb); "
               "INSERT INTO ddl_t (id) SELECT i FROM generate_series(1, 20) i",
    'single': "CREATE TABLE ddl_t (id int PRIMARY KEY, g int, v int, amount numeric(10,2), kind text, body text, d date, ts timestamptz, tags text[], a jsonb); "
              "INSERT INTO ddl_t VALUES (1, 1, 1, 1, 'k', 'w', '2026-01-01', now(), ARRAY['a'], '{}')",
    'dropped': "CREATE TABLE ddl_t (junk int, id int PRIMARY KEY, g int, junk2 text, v int, amount numeric(10,2), kind text, body text, d date, ts timestamptz, tags text[], a jsonb); "
               "INSERT INTO ddl_t SELECT i, i, i % 3, 'j', i % 4, i, 'k' || i % 3, 'w' || i, '2026-01-01', now(), ARRAY['a'], '{}' FROM generate_series(1, 30) i; "
               "ALTER TABLE ddl_t DROP COLUMN junk, DROP COLUMN junk2",
}
COLS = 'id, g, v, amount, kind, d, ts'


def ddl_sweep(sess, a, stats, fails, reconnect, reset_fixture, states):
    opts = options()
    scen = list(SCEN) if a.tier == 'full' else ['empty', 'nulls', 'allnull', 'dropped']
    vals = DDL_VALUES if a.tier == 'full' else DDL_VALUES[::2]
    t0 = time.time()
    n0 = stats['calls']

    def go(label, text):
        stats['calls'] += 1
        try:
            lines = sess.run(text)
        except Crash as e:
            stats['crashes'] += 1
            fails.append(('CRASH', None, label, str(e)))
            print(f'FAIL CRASH ddl {label}: {e}\n  {text[:400]}', flush=True)
            reconnect()
            return False
        for l, st, state, msg in classify(lines):
            if st == 'OK':
                stats['ok'] += 1
            elif not bad(state, msg):
                stats['errors_clean'] += 1
                states[state] = states.get(state, 0) + 1
            if st == 'ERR' and bad(state, msg):
                fails.append(('ERROR', None, label, f'{state}: {msg}'))
                print(f'FAIL INTERNAL ddl {label}: {state}: {(msg or "")[:200]}\n  {text[:300]}', flush=True)
        return True

    for sname in scen:
        sess.run('DROP TABLE IF EXISTS ddl_t CASCADE; ' + SCEN[sname] + ';')
        for kind, name, applies in opts:
            if kind == 'sift_col':
                create = f'CREATE INDEX ddl_i ON ddl_t USING sift_col ({COLS})'
            else:
                create = 'CREATE INDEX ddl_i ON ddl_t USING sift (body)'
            for vi, v in enumerate(vals):
                lab = f'{sname}.{kind}.{name}.v{vi}'
                go(lab + '.create', wrap(f'EXECUTE format($f$%s WITH ({name} = %s)$f$, {create!r}, quote_literal({v}))', lab) + "\nDROP INDEX IF EXISTS ddl_i;")
            lab = f'{sname}.{kind}.{name}.dup'
            go(lab, wrap(f"EXECUTE '{create} WITH ({name} = ''a'', {name} = ''b'')'", lab) + '\nDROP INDEX IF EXISTS ddl_i;')
            lab = f'{sname}.{kind}.{name}.unset'
            go(lab, wrap(f"EXECUTE '{create} WITH ({name})'", lab) + '\nDROP INDEX IF EXISTS ddl_i;')
            go(f'{sname}.{kind}.{name}.setup', f'DROP INDEX IF EXISTS ddl_i; {create};')
            for vi, v in enumerate(vals[::3]):
                lab = f'{sname}.{kind}.{name}.alter{vi}'
                go(lab, wrap(f"EXECUTE format('ALTER INDEX ddl_i SET ({name} = %s)', quote_literal({v}))", lab))
            lab = f'{sname}.{kind}.{name}.reset'
            go(lab, wrap(f"EXECUTE 'ALTER INDEX ddl_i RESET ({name})'", lab))
            go(f'{sname}.{kind}.{name}.reindex', wrap("EXECUTE 'REINDEX INDEX ddl_i'", lab) + '\nDROP INDEX IF EXISTS ddl_i;')
        # contradictory combinations
        for lab, w in [('keycut_keyorder', "key_cut = 'off', key_order = 'id'"), ('rollup_off_cols', "rollup_columns = 'off', rollup_columns = 'g'"),
                       ('connect_scope', "connect = 'g -> ddl_t(id)', scope = 'x: g -> ddl_t(g, id)'"), ('track_cols', "track_changes = 'off', track_changes = 'id'"),
                       ('everything', "key_cut = 'g', key_order = 'g', order_array_columns = 'off', preagg = 'off', rollup_columns = 'g: v', superunit = 1")]:
            go(f'{sname}.combo.{lab}', wrap(f"EXECUTE 'CREATE INDEX ddl_i ON ddl_t USING sift_col ({COLS}) WITH ({w})'", f'{sname}.{lab}') + '\nDROP INDEX IF EXISTS ddl_i;')
    for kind, name, applies in opts:
        if 'sift_table' not in applies:
            continue
        for vi, v in enumerate(vals[::2]):
            lab = f'tableam.{name}.v{vi}'
            go(lab, wrap(f"EXECUTE format('CREATE TABLE ddl_tt (id int, g int) USING sift_table WITH ({name} = %s)', quote_literal({v}))", lab) + '\nDROP TABLE IF EXISTS ddl_tt;')
    stats['ddl_calls'] = stats['calls'] - n0
    print(f'ddl sweep: {stats["calls"] - n0} statements, {len(opts)} options, {time.time() - t0:.0f} s', flush=True)


if __name__ == '__main__':
    sys.exit(main())
