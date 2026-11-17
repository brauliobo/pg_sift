#!/usr/bin/env python3
"""SQL surface sweep: no call of the sift schema may crash the backend or raise an internal error, whatever its arguments.

  test/fuzz/surface_sweep.py [--tier quick|full] [--only RE] [--budget SEC] [--db NAME] [--list] [--role]

Every function, procedure, aggregate, window function, ordered-set aggregate and trigger function of the extensions pg_sift, pg_sift_test and pg_sift_helpers (the
sift schema and the objects of the extensions) must be exercised in each of three modes: as a superuser, inside a rolled-back savepoint and as an unprivileged role. "Exercised"
means the function body ran with the hostile argument: a normal return (not a strict function skipped for a NULL argument), an error whose call stack names the PL/pgSQL or SQL function, or
an error of a call that analyzes without executing (the same call with WHERE false), so a call that fails to resolve, to cast its arguments or to be allowed does not count; a trigger
function counts when its call counter moved or a firing statement of the created trigger failed; the unprivileged role is expected to be refused an EXECUTE it was not granted. A function that is not exercised in a mode fails the sweep unless scripts/surface.allow has a row (signature, modes,
reason); a row of a function that is exercised, or that is gone, fails too. Every function is called with all-NULL arguments, with
each argument in turn set to a hostile value of its type (NULL, empty, huge, NaN, a regclass that is dropped, a heap, a view, a partitioned table, a temp table,
a table of another schema, a 1 MB text, a nested array, ...) while the others are valid, and with every argument set to the same kind of hostile value. Each call
runs in a plpgsql block with an EXCEPTION clause (a subtransaction) that reports the SQLSTATE; a sample also runs after a ROLLBACK TO SAVEPOINT, and as an unprivileged
role (--role). A part then creates every index and table option with hostile values on tables of every kind of contents (empty, NULLs, all NULL, one row, dropped columns).
A failure is a signal (the session ends), SQLSTATE XX000 or an internal message, and a timeout. Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT).
"""
import argparse
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shapes_matrix  # noqa: E402
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
CREATE INDEX t_brin ON fx.t USING brin (id);
CREATE INDEX t_hash ON fx.t USING hash (g);
CREATE TABLE fx.t2 (id int PRIMARY KEY, g int, v int, kind text, body text);
INSERT INTO fx.t2 SELECT i, i % 4, i % 7, 'k' || i % 2, 'word' || i % 3 FROM generate_series(1, 60) i;
CREATE INDEX t2_col ON fx.t2 USING sift_col (id, g, v, kind);
CREATE INDEX t2_bm ON fx.t2 USING sift (body);
CREATE TABLE fx.trg (id int, g int);
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
CREATE EXTENSION IF NOT EXISTS file_fdw;
DO $$ BEGIN
	IF NOT EXISTS (SELECT 1 FROM pg_foreign_server WHERE srvname = 'fx_srv') THEN CREATE SERVER fx_srv FOREIGN DATA WRAPPER file_fdw; END IF;
END $$;
CREATE FOREIGN TABLE fx.ft (id int, g int) SERVER fx_srv OPTIONS (filename '/dev/null', format 'csv');
CREATE TYPE fx.ct AS (a int);
CREATE TABLE fx.gone (id int);
CREATE TEMP TABLE fx_oids AS SELECT 'fx.gone'::regclass::oid AS gone, 'fx.t_col'::regclass::oid AS col,
	(SELECT reltoastrelid FROM pg_class WHERE oid = 'fx.t'::regclass) AS toast, (SELECT typrelid FROM pg_type WHERE oid = 'fx.ct'::regtype) AS comp;
DROP TABLE fx.gone;
ANALYZE fx.t; ANALYZE fx.tbl;
SET client_min_messages = notice;
"""

OIDS = {'regclass': ["'fx.t'::regclass", "'fx.t_col'::regclass", "'fx.t_bm'::regclass", "'fx.t_bt'::regclass", "'fx.heap'::regclass", "'fx.tbl'::regclass",
                     "'fx.vw'::regclass", "'fx.t_brin'::regclass", "'fx.t_hash'::regclass", "'fx.t2'::regclass", "'fx.t2_col'::regclass", "'fx.t2_bm'::regclass", "'fx.mv'::regclass", "'fx.part'::regclass", "'fx.part_col'::regclass", "'fx.sq'::regclass", "'fx_temp'::regclass",
                     "'fx2.other'::regclass", "'fx.ft'::regclass", "(SELECT toast FROM fx_oids)::regclass",
                     "(SELECT comp FROM fx_oids)::regclass", "(SELECT gone FROM fx_oids)::regclass", '0::oid::regclass', '4294967295::oid::regclass', "'pg_class'::regclass",
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
    'regtype': ["'int4'::regtype", "0::oid::regtype", "'text'::regtype", "4294967295::oid::regtype", "'fx.ct'::regtype"],
    'regproc': ["'now'::regproc", "0::oid::regproc", "4294967295::oid::regproc"],
    'regprocedure': ["'now()'::regprocedure", "0::oid::regprocedure"],
    'regnamespace': ["'fx'::regnamespace", "0::oid::regnamespace", "'pg_toast'::regnamespace", "4294967295::oid::regnamespace"],
    'regrole': ["'postgres'::regrole", "0::oid::regrole", "4294967295::oid::regrole"],
    'regconfig': ["'simple'::regconfig", "0::oid::regconfig"],
    'regcollation': ["'\"C\"'::regcollation", "0::oid::regcollation"],
    'regoperator': ["'=(int4,int4)'::regoperator", "0::oid::regoperator"],
    'regoper': ["'='::regoper", "0::oid::regoper"],
    'regdictionary': ["'simple'::regdictionary", "0::oid::regdictionary"],
    'xid': ["'0'", "'1'", "'4294967295'"],
    'cid': ["'0'", "'4294967295'"],
    'name': ["''", "'id'", "repeat('x', 63)", "'fx'", "'nokey'"],
    'time': ["'00:00'", "'24:00'"],
    'point': ["'(0,0)'", "'(1e308,-1e308)'"],
    'inet': ["'127.0.0.1'"],
    'tsvector': ["''", "'a b'"],
    'tsquery': ["'a'", "'a & b'"],
    'jsonpath': ["'$'", "'$.a'"],
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
VALID_CROSS = dict(VALID_TBL, regclass=["'fx.t'::regclass", "'fx.t2_col'::regclass", "'fx.t2'::regclass"], oid=["'fx.t'::regclass::oid", "'fx.t2_col'::regclass::oid"])
VALID_CROSS2 = dict(VALID_IDX, regclass=["'fx.t_col'::regclass", "'fx.t2'::regclass", "'fx.t_bm'::regclass"], oid=["'fx.t_col'::regclass::oid", "'fx.t2'::regclass::oid"])
BASES = [('tbl', VALID_TBL), ('idx', VALID_IDX), ('bm25', VALID_BM), ('table', VALID_TAB), ('cross', VALID_CROSS), ('cross2', VALID_CROSS2)]


HUGE = 1000


def pool(t, limit=0):
    """the hostile values of a type, as SQL expressions of that type"""
    t = {'int': 'int4', 'integer': 'int4', 'bigint': 'int8', 'smallint': 'int2', 'double precision': 'float8', 'real': 'float4', 'boolean': 'bool',
         'character varying': 'text', 'varchar': 'text', 'text[]': 'text[]', 'integer[]': 'int4[]', 'bigint[]': 'int8[]', 'timestamp with time zone': 'timestamptz',
         'timestamp without time zone': 'timestamp', 'double precision[]': 'float8[]', 'character varying[]': 'text[]', 'anyelement': 'text', 'anyarray': 'text[]',
         'anycompatible': 'text', 'anycompatiblearray': 'text[]', '"char"': 'text', 'oid': 'oid', 'regclass': 'regclass', 'tid': 'tid',
         'time without time zone': 'time', 'regdictionary': 'regdictionary'}.get(t, t)
    if t == 'regclass':
        return [(f'r{i}', v) for i, v in enumerate(OIDS['regclass'])]
    base = POOL.get(t)
    if base is not None and limit:
        base = [v for v in base if not (re.fullmatch(r'-?\d+', v) and abs(int(v)) > limit)]
    if base is None and t.endswith('[]') and POOL.get(t[:-2]):
        base = ["'{}'", 'ARRAY[NULL]::%s[]' % t[:-2]]
    if base is None and t.endswith('range'):
        return [('empty', f"'empty'::{t}")]
    if base is None:
        return []
    return [(f'p{i}', f'({v})::{t}') for i, v in enumerate(base)]


def valid(t, base, nth=0):
    t = {'int': 'int4', 'integer': 'int4', 'bigint': 'int8', 'smallint': 'int2', 'double precision': 'float8', 'real': 'float4', 'boolean': 'bool',
         'character varying': 'text', 'timestamp with time zone': 'timestamptz', 'timestamp without time zone': 'timestamp', 'anyelement': 'text',
         'anyarray': 'text[]', 'integer[]': 'int4[]', 'bigint[]': 'int8[]', 'double precision[]': 'float8[]', 'name': 'text'}.get(t, t)
    v = base.get(t)
    if isinstance(v, list):
        v = v[min(nth, len(v) - 1)]
    return f'({v})::{t}' if v else f'NULL::{t}'


def valid_args(args_t, base):
    seen, out = {}, []
    for t in args_t:
        out.append(valid(t, base, seen.get(t, 0)))
        seen[t] = seen.get(t, 0) + 1
    return out


EXTENSIONS = ('pg_sift', 'pg_sift_test', 'pg_sift_helpers')


def refused_for_role(ob):
    """a function whose EXECUTE is not granted to the unprivileged role: the call is refused before its body, and the expectation of the role mode is that refusal"""
    return ob.get('role_denied', False)


def objects(sess, with_test=False):
    sql = """SELECT p.oid, p.proname, p.prokind, p.proretset, p.provariadic <> 0, p.proisstrict, p.prorettype::regtype::text, coalesce(a.aggkind, ''),
		coalesce(a.aggnumdirectargs, 0), p.oid::regprocedure::text,
		coalesce((SELECT string_agg(format_type(t, NULL), ',' ORDER BY o) FROM unnest(p.proargtypes::oid[]) WITH ORDINALITY u(t, o)), ''),
		(SELECT lanname FROM pg_language WHERE oid = p.prolang),
		(SELECT e.extname FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.refclassid = 'pg_extension'::regclass AND d.deptype = 'e' LIMIT 1)
	FROM pg_proc p LEFT JOIN pg_aggregate a ON a.aggfnoid = p.oid
	WHERE p.oid IN (SELECT objid FROM pg_depend WHERE classid = 'pg_proc'::regclass AND refclassid = 'pg_extension'::regclass AND deptype = 'e'
			AND refobjid IN (SELECT oid FROM pg_extension WHERE extname = ANY(%s)))
	ORDER BY p.proname, p.oid;"""
    sql = sql % ("ARRAY['pg_sift', 'pg_sift_test', 'pg_sift_helpers']" if with_test else "ARRAY['pg_sift', 'pg_sift_helpers']")
    out = []
    for l in sess.run(sql):
        f = l.split('|')
        if len(f) == 13:
            out.append(dict(ext=f[12], lang=f[11], oid=f[0], name=f[1], kind=f[2], retset=f[3] == 't', variadic=f[4] == 't', strict=f[5] == 't', ret=f[6], aggkind=f[7],
                            ndirect=int(f[8]), sig=f[9], args=f[10].split(',') if f[10] else []))
    return out


def qualified(sess, oid):
    return sess.run(f"SELECT p.oid::regprocedure::text, pg_catalog.quote_ident(n.nspname) || '.' || pg_catalog.quote_ident(p.proname) FROM pg_proc p "
                    f"JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.oid = {oid};")[0].split('|')[1]


PLANNER_CALLBACKS = {'matchsel': 'body @@> %1$L', 'match_support': "sift.match(body, %1$L) OR sift.match_idx(body, %1$L, 'fx.t_bm'::regclass::oid)"}


def text_driven(ob):
    """a function SQL cannot call with its own arguments (they are internal or cstring) and reaches by a text: a cast for the input and output functions of a type, the plan of a
    query that uses the operator or the function for the planner callbacks"""
    return ob['args'] == ['cstring'] or ob['ret'] == 'cstring' or ob['name'] in PLANNER_CALLBACKS


def text_calls():
    names = ["'fx.gone'", "'a.b.c.d'", "'\"fx\".\"t\"'", "'.'", "'fx.'", "'fx.t.id'", "'pg_class'", "'0'"]
    return [(f'io{i}', [f'({v})::text']) for i, v in enumerate(POOL['text'] + names)]


def call_sql(ob, name, args):
    if ob['name'] in PLANNER_CALLBACKS:
        return f"EXECUTE format($q$EXPLAIN (COSTS OFF) SELECT id FROM fx.t WHERE {PLANNER_CALLBACKS[ob['name']]}$q$, {args[0]})"
    if ob['args'] == ['cstring']:
        return f"PERFORM ({args[0]})::{ob['ret']}"
    if ob['ret'] == 'cstring':
        return f"PERFORM (({args[0]})::{ob['args'][0]})::text"
    a = ', '.join(args[:-1] + ['VARIADIC ' + args[-1]] if ob['variadic'] and ob['args'][-1] != '"any"' else args)
    if ob['kind'] == 'w':
        return f'PERFORM {name}({a}) OVER () FROM generate_series(1, 3)'
    if ob['kind'] == 'a' and ob['aggkind'] in ('o', 'h'):
        direct, grouped = args[:ob['ndirect']], args[ob['ndirect']:]
        return f'PERFORM {name}({", ".join(direct)}) WITHIN GROUP (ORDER BY {", ".join(grouped) or "1"}) FROM generate_series(1, 3)'
    if ob['kind'] == 'p':
        return f'CALL {name}({a})'
    if ob['kind'] == 'a':
        return f'PERFORM {name}({a}) FROM generate_series(1, 3)'
    if ob['retset']:
        return f'PERFORM count(*) FROM {name}({a})'
    return f'PERFORM {name}({a})'


def wrap(stmt, label):
    body = stmt if stmt.startswith(('CALL', 'PERFORM', 'EXECUTE', 'CREATE', 'DROP', 'ALTER', 'INSERT', 'UPDATE', 'DELETE', 'TRUNCATE')) else 'PERFORM ' + stmt
    return (f"DO $do$ DECLARE c text; BEGIN {body}; RAISE NOTICE '@@OK {label}'; EXCEPTION WHEN OTHERS THEN GET STACKED DIAGNOSTICS c = PG_EXCEPTION_CONTEXT; "
            f"RAISE NOTICE '@@ERR {label} % %', SQLSTATE, SQLERRM; RAISE NOTICE '@@CTX {label} %', replace(c, chr(10), ' | '); END $do$;")


def call_sets(ob, args_t, cap, limit=0):
    """the argument lists of the sweep of one function: (label, [sql expression]) in a fixed order"""
    n = len(args_t)
    out = [('allnull', [f'NULL::{t}' for t in args_t])]
    pools = [pool(t, limit) for t in args_t]
    for bname, base in BASES:
        out.append((f'valid.{bname}', valid_args(args_t, base)))
    for bi, (bname, base) in enumerate(BASES):
        for i, t in enumerate(args_t):
            for lab, v in pools[i]:
                args = valid_args(args_t, base)
                args[i] = v
                out.append((f'{bname}.a{i + 1}.{lab}', args))
            args = valid_args(args_t, base)
            args[i] = f'NULL::{t}'
            out.append((f'{bname}.a{i + 1}.null', args))
    width = max((len(p) for p in pools), default=0)
    for j in range(width):
        out.append((f'all.p{j}', [pools[i][j % len(pools[i])][1] if pools[i] else f'NULL::{args_t[i]}' for i in range(n)]))
    for j in range(n):
        out.append((f'nullone.{j + 1}', [f'NULL::{t}' if i == j else valid(t, VALID_TBL) for i, t in enumerate(args_t)]))
    return subsample(out, cap)


def subsample(calls, cap):
    """at most cap calls, evenly spaced; the all-NULL call and the calls with only valid arguments stay: they are the ones that reach the function"""
    head = [c for c in calls if c[0] == 'allnull' or c[0].startswith('valid.')]
    rest = [c for c in calls if c not in head]
    room = max(cap - len(head), 0)
    if len(rest) > room:
        step = len(rest) / max(room, 1)
        rest = [rest[int(i * step)] for i in range(room)]
    return head + rest


def classify(lines):
    """[(label, status, sqlstate, message, context)]: the context of an error is its call stack, which names a PL/pgSQL or SQL function that raised it"""
    res = []
    for l in lines:
        c = re.match(r'(?:NOTICE|WARNING):\s+(?:[0-9A-Z]{5}: )?@@CTX (\S+) (.*)', l)
        if c and res and res[-1][0] == c.group(1):
            res[-1] = res[-1][:4] + (c.group(2),)
            continue
        m = re.match(r'(?:NOTICE|WARNING):\s+(?:[0-9A-Z]{5}: )?@@(OK|ERR) (\S+)(?: ([0-9A-Z]{5}) (.*))?', l)
        if m:
            res.append((m.group(2), m.group(1), m.group(3), m.group(4), ''))
        else:
            m = re.match(r'ERROR:\s+([0-9A-Z]{5}): (.*)', l)
            if m:
                res.append(('?', 'ERR', m.group(1), m.group(2), ''))
    return res


INIT = "SET client_min_messages = notice; SET statement_timeout = '15s'; SET lock_timeout = '2s'; SET track_functions = 'all';"
MODES = ('super', 'rollback', 'role')
ALLOW = os.path.join(REPO, 'scripts', 'surface.allow')


def call_count(sess, oid):
    sess.run('SELECT pg_stat_force_next_flush();')
    r = sess.run(f'SELECT coalesce(pg_stat_get_function_calls({oid}), 0) FROM (SELECT pg_stat_clear_snapshot()) s;')
    return int(r[0]) if r and r[0].strip().isdigit() else 0


def load_allow():
    rows = {}
    with open(ALLOW) as f:
        for line in f:
            if line.strip() and not line.startswith('#'):
                c = line.rstrip('\n').split('\t')
                if len(c) != 3 or not c[2].strip():
                    sys.exit(f'{ALLOW}: a row is SIGNATURE TAB MODES TAB REASON: {line.strip()}')
                rows[c[0]] = (set(MODES) if c[1] == '*' else set(c[1].split(',')), c[2])
    return rows


def int_limit(allow, ob):
    """the largest integer an integer argument is called with, 0 for no limit: an integer argument of the test extension is a workload (cases, pages, rows), so it is called with
    integers of at most HUGE; _test_relcache_burn takes a relcache callback slot of the backend for each count (the table has 10, a call past them is a FATAL of PostgreSQL)"""
    modes = allow.get(ob['sig'], (set(),))[0]
    return 3 if ob['name'] == '_test_relcache_burn' else HUGE if ob['name'].startswith('_test') or 'no-huge' in modes else 0


def coverage(obs, reach, allow):
    """[(signature, mode)] never exercised and not allowed, and the allow rows that are stale"""
    missing, stale = [], []
    sigs = {o['sig'] for o in obs}
    for o in obs:
        for m in MODES:
            if not reach.get((o['sig'], m)) and m not in allow.get(o['sig'], (set(),))[0]:
                missing.append((o['sig'], m))
    for sig, (modes, why) in allow.items():
        if sig not in sigs:
            stale.append((sig, 'no such function'))
        else:
            for m in sorted(modes - {'no-huge', 'tiny'}):
                if reach.get((sig, m)):
                    stale.append((sig, f'{m} is exercised'))
    return missing, stale


def bad(state, msg):
    return state == 'XX000' or (msg and BAD.search(msg) and not ALLOW_MSG.search(msg)) or state in ('57014',)


def parse():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--tier', default='quick', choices=['quick', 'full'])
    ap.add_argument('--only', default='')
    ap.add_argument('--budget', type=int, default=0)
    ap.add_argument('--db', default='surface')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--no-ddl', action='store_true')
    ap.add_argument('--no-calls', action='store_true')
    ap.add_argument('--with-test-extension', action='store_true', help='also sweep the functions of pg_sift_test (needs a SIFT_TESTING build); many take internal pointers and offsets and raise XX000 on purpose')
    ap.add_argument('--no-coverage', action='store_true', help='do not fail on a function of pg_sift or pg_sift_helpers that is not reached in a mode and has no row in scripts/surface.allow')
    ap.add_argument('--use-cluster', action='store_true', help='use the cluster of SIFT_TMP and SIFT_PORT, not a scratch cluster of its own (the default: it starts one on the install of SIFT_TMP and removes it)')
    return ap.parse_args()


def _sweep(a):
    db = a.db
    subprocess.run(['psql', '-X', '-q', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {db}', '-c', f'CREATE DATABASE {db}'], env=ENV, check=True, capture_output=True)
    subprocess.run(['psql', '-X', '-q', '-d', db, '-c', 'CREATE EXTENSION pg_sift'], env=ENV, check=True, capture_output=True)
    for ext in ('pg_sift_test', 'pg_sift_helpers'):
        subprocess.run(['psql', '-X', '-q', '-d', db, '-c', f'CREATE EXTENSION IF NOT EXISTS {ext}'], env=ENV, capture_output=True)
    sess = Session(db)
    sess.run(INIT)
    other = Session(db)
    other.run('CREATE TEMP TABLE fx_other_temp (id int, g int);')
    OIDS['regclass'].append("%s::oid::regclass" % other.run("SELECT 'fx_other_temp'::regclass::oid;")[0].strip())
    obs = [o for o in objects(sess, a.with_test_extension) if re.search(a.only, o['name'])]
    reach = {}
    allow = load_allow()
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
        sess.run(INIT)

    def reset_fixture():
        nonlocal sess
        try:
            sess.run(FIXTURE)
        except Crash as e:
            reconnect()
            sess.run(FIXTURE)

    def in_stack(ob, ctx):
        return bool(re.search(r'function "?(?:\w+\.)?' + re.escape(ob['name']) + r'"?[( ]', ctx))

    def resolves(ob, name, args, prelude, after):
        """the call is analyzed, not executed: it names the function and is allowed to"""
        text = prelude + wrap(call_sql(ob, name, args) + ' WHERE false', 'probe') + after
        return any(st == 'OK' for _, st, _, _, _ in classify(sess.run(text)))

    def body_ran(ob, name, args, st, ctx, prelude, after):
        """the function ran: it returned a result (not a strict function skipped for a NULL argument), or its call stack names it, or it raised an error from a call that is
        analyzed and allowed; an error of resolving the function, casting its arguments or calling it unprivileged is not the function"""
        if ob['strict'] and any(re.match(r'\(?NULL\b', x) for x in args):
            return False
        return st == 'OK' or in_stack(ob, ctx) or (ob['kind'] != 'p' and resolves(ob, name, args, prelude, after))

    def run_batch(ob, name, calls, label_pfx, prelude='', after='', mode='super'):
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
            for l, st, state, msg, ctx in classify(lines):
                if mode == 'role' and refused_for_role(ob):
                    if state == '42501':
                        reach[(ob['sig'], mode)] = True
                elif body_ran(ob, name, args, st, ctx, prelude, after):
                    reach[(ob['sig'], mode)] = True
                if st == 'OK':
                    stats['ok'] += 1
                    continue
                states[state] = states.get(state, 0) + 1
                if bad(state, msg):
                    fails.append(('ERROR', ob, lab, f'{state}: {msg}'))
                    print(f'FAIL {"TIMEOUT" if state == "57014" else "INTERNAL"} {label_pfx}{name}({", ".join(ob["args"])}) call {lab}: {state}: {(msg or "")[:200]}\n  {stmt[:400]}', flush=True)
                else:
                    stats['errors_clean'] += 1

    def trigger_calls(ob, name):
        """the statements that fire a trigger function or an event trigger function: (label, create, [firing statements], teardown)"""
        if ob['ret'] == 'event_trigger':
            fires = ['CREATE TABLE fx.evt (i int)', 'ALTER TABLE fx.evt ADD COLUMN j int', 'ALTER TABLE fx.evt ALTER COLUMN j TYPE bigint', 'DROP TABLE fx.evt']
            return [(f'evt.{ev}', f'CREATE EVENT TRIGGER sw_ev ON {ev} EXECUTE FUNCTION {name}()', fires, 'DROP EVENT TRIGGER IF EXISTS sw_ev; DROP TABLE IF EXISTS fx.evt')
                    for ev in ('ddl_command_start', 'ddl_command_end', 'sql_drop', 'table_rewrite')]
        out = []
        drop = 'DROP TRIGGER IF EXISTS sw_t ON fx.trg; DROP TRIGGER IF EXISTS sw_s ON fx.trg'
        for lab, args in (('none', ''), ('x', "'x'"), ('cols', "'id', 'g'"), ('rel', "'fx.t', 'id'"), ('empty', "''"), ('many', ', '.join(["'a'"] * 20))):
            for when in ('BEFORE', 'AFTER'):
                out.append((f'trg.{when.lower()}.{lab}', f'CREATE TRIGGER sw_t {when} INSERT OR UPDATE OR DELETE ON fx.trg FOR EACH ROW EXECUTE FUNCTION {name}({args})',
                            ['INSERT INTO fx.trg VALUES (1, 1), (2, NULL)', 'UPDATE fx.trg SET g = 5', 'DELETE FROM fx.trg WHERE id = 1'], drop))
            out.append((f'trg.stmt.{lab}', f'CREATE TRIGGER sw_s AFTER INSERT OR UPDATE OR DELETE OR TRUNCATE ON fx.trg FOR EACH STATEMENT EXECUTE FUNCTION {name}({args})',
                        ['INSERT INTO fx.trg VALUES (3, 3)', 'UPDATE fx.trg SET g = 6', 'TRUNCATE fx.trg'], drop))
        return out

    def trigger_ran(ob, mode, lab, results):
        """the trigger function ran: it returned, or a firing statement of a created trigger failed (the statements are valid, so the trigger raised), or its call stack names it;
        an event trigger is created by a superuser only, and the unprivileged role is expected to be refused"""
        create = results.get(f'{lab}.create')
        if mode == 'role' and ob['ret'] == 'event_trigger':
            return bool(create) and create[1] == '42501'
        failed = [r for l, r in results.items() if l.startswith(f'{lab}.fire') and r[0] == 'ERR']
        return bool(create) and create[0] == 'OK' and bool(failed) or any(in_stack(ob, r[3]) for r in results.values())

    def triggered(ob, name, mode, prelude='', after=''):
        before = call_count(sess, ob['oid'])
        for lab, create, fires, teardown in trigger_calls(ob, name):
            steps = [(f'{lab}.create', create)] + [(f'{lab}.fire{i}', f) for i, f in enumerate(fires)]
            text = prelude + ''.join(wrap(f, l) for l, f in steps) + after + wrap(teardown, f'{lab}.drop')
            stats['calls'] += 1
            try:
                lines = sess.run(text)
            except Crash as e:
                stats['crashes'] += 1
                fails.append(('CRASH', ob, lab, str(e)))
                print(f'FAIL CRASH {mode} {name} {lab}: {e}\n  {create[:400]}', flush=True)
                reconnect()
                reset_fixture()
                continue
            results = {}
            for l, st, state, msg, ctx in classify(lines):
                results.setdefault(l, (st, state, msg, ctx))
                if st == 'OK':
                    stats['ok'] += 1
                    continue
                states[state] = states.get(state, 0) + 1
                if bad(state, msg):
                    fails.append(('ERROR', ob, l, f'{state}: {msg}'))
                    print(f'FAIL INTERNAL {mode} {name} {l}: {state}: {(msg or "")[:200]}\n  {create[:400]}', flush=True)
                else:
                    stats['errors_clean'] += 1
            if trigger_ran(ob, mode, lab, results):
                reach[(ob['sig'], mode)] = True
        if call_count(sess, ob['oid']) > before:
            reach[(ob['sig'], mode)] = True

    def batch(ob, name, calls, label, mode, prelude='', after=''):
        if ob['ret'] in ('trigger', 'event_trigger'):
            triggered(ob, name, mode, prelude, after)
        else:
            run_batch(ob, name, calls, label, prelude, after, mode)

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
            calls = text_calls() if text_driven(ob) else call_sets(ob, ob['args'], cap, int_limit(allow, ob))
            if a.list:
                print(f'{ob["sig"]} kind={ob["kind"]} ret={ob["ret"]} strict={ob["strict"]} calls={len(calls)}')
                continue
            batch(ob, name, calls, '', 'super')
            reset_fixture()
            batch(ob, name, subsample(calls, max(len(calls) // 7, 8)), 'after-rollback ', 'rollback', prelude="BEGIN; SAVEPOINT s0; SELECT 1/0; ROLLBACK TO s0;\n", after='\nROLLBACK;')
        if not a.list:
            reset_fixture()
            sess.run("DROP ROLE IF EXISTS ps_nobody; CREATE ROLE ps_nobody; GRANT USAGE ON SCHEMA fx, fx2, sift TO ps_nobody; GRANT SELECT ON ALL TABLES IN SCHEMA fx TO ps_nobody; "
                     "GRANT INSERT, UPDATE, DELETE, TRUNCATE, TRIGGER ON fx.trg TO ps_nobody;")
            denied = set(sess.run("SELECT oid FROM pg_proc WHERE NOT has_function_privilege('ps_nobody', oid, 'EXECUTE');"))
            for ob in obs:
                ob['role_denied'] = ob['oid'] in denied
            for ob in obs:
                if a.budget and time.time() - t0 > a.budget:
                    break
                name = ob.get('display')
                if not name:
                    continue
                calls = text_calls()[:12] if text_driven(ob) else call_sets(ob, ob['args'], 12 if a.tier == 'quick' else 60, int_limit(allow, ob))
                batch(ob, name, calls, 'as-unprivileged ', 'role', prelude='SET ROLE ps_nobody;\n', after='\nRESET ROLE;')
    if not a.no_ddl and not a.list:
        ddl_sweep(sess, a, stats, fails, reconnect, reset_fixture, states)
    other.close()
    sess.close()
    up = wait_ready(db, 10)
    sig = log_scan(mark)
    nonstrict = [o['name'] for o in obs if not o['strict'] and o['args']]
    print(f'surface: tier={a.tier} functions={stats["functions"]} calls={stats["calls"]} ok={stats["ok"]} clean_errors={stats["errors_clean"]} crashes={stats["crashes"]} '
          f'failures={len(fails)} seconds={time.time() - t0:.0f} server_up={up} log_signals={sig}')
    print('sqlstates: ' + ' '.join(f'{k}={v}' for k, v in sorted(states.items(), key=lambda x: -x[1])[:14]))
    print(f'not strict ({len(nonstrict)}): ' + ', '.join(sorted(set(nonstrict))[:40]))
    subprocess.run(['psql', '-X', '-q', '-d', 'postgres', '-c', f'DROP DATABASE IF EXISTS {db}'], env=ENV, capture_output=True)
    gap = False
    if not a.no_coverage and not (a.only or a.budget or a.no_calls or a.list):
        prod = [o for o in obs if o['ext'] != 'pg_sift_test']
        missing, stale = coverage(prod, reach, allow)
        exercised = sum(1 for o in prod if all(reach.get((o['sig'], m)) for m in MODES))
        print(f'coverage: functions={len(prod)} exercised_in_all_modes={exercised} allow_listed={len(allow)} not_exercised={len(missing)} stale_allow_rows={len(stale)}')
        for sig_, m in missing:
            print(f'FAIL NOT EXERCISED {sig_} in {m}: call it, or add a row to scripts/surface.allow with the reason it cannot be called')
        for sig_, why in stale:
            print(f'FAIL STALE ALLOW ROW {sig_}: {why}')
        gap = bool(missing or stale)
    return 1 if fails or not up or sig or gap else 0


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
        for l, st, state, msg, ctx in classify(lines):
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


PGTEST = os.path.join(REPO, 'scripts', 'pgtest.sh')


def free_port(first):
    for port in range(first, 56300):
        with socket.socket() as sk:
            if sk.connect_ex(('127.0.0.1', port)) != 0:
                return port
    sys.exit('no free port in 56100-56299')


def main():
    """the sweep runs on a cluster of its own, started on the install of SIFT_TMP and removed at the end (stop and destroy are the leak and sanitizer gate of pgtest.sh): a crash
    of the sweep is never the state another test left, and the sweep leaves nothing to a test that runs after it"""
    a = parse()
    if a.use_cluster:
        return _sweep(a)
    root = os.environ.get('SIFT_ROOT') or os.path.join(shapes_matrix.TMP, 'root')
    scratch = tempfile.mkdtemp(prefix='sift-surface-', dir=os.environ.get('TMPDIR') or '/dev/shm')
    port = free_port(int(os.environ.get('SIFT_PORT', '56100')) + 1)
    env = dict(os.environ, SIFT_TMP=scratch, SIFT_ROOT=root, SIFT_PORT=str(port))
    started = subprocess.run([PGTEST, 'start'], env=env, capture_output=True, text=True)
    if started.returncode != 0:
        shutil.rmtree(scratch, ignore_errors=True)
        sys.exit(f'the scratch cluster did not start: {started.stdout[-400:]}{started.stderr[-400:]}')
    shapes_matrix.TMP = scratch
    ENV.update(PGHOST=f'{scratch}/sock', PGPORT=str(port))
    try:
        rc = _sweep(a)
    finally:
        gate = subprocess.run([PGTEST, 'destroy'], env=env, capture_output=True, text=True)
        shutil.rmtree(scratch, ignore_errors=True)
    if gate.returncode != 0:
        print('FAIL SCRATCH CLUSTER GATE (leaks, sanitizer reports or a signal in its log): ' + (gate.stdout + gate.stderr)[-1500:])
        return 1
    return rc


if __name__ == '__main__':
    sys.exit(main())
