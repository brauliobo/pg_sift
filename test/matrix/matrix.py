#!/usr/bin/env python3
"""Coverage percentages of the pushdown, measured and not written down.

Every variant is a SELECT built from what the server and the source say: the key types come from pg_type (a type whose one-column sift_col index
can be built), the aggregates from pg_aggregate, the volatile and stable functions from pg_proc, the sizes and key counts from SIFT_TOPN_* in
src/agg/topn.h, the join kinds from the grammar. A variant runs twice, with the pushdown off (the reference) and on, and counts as covered when
the plan has the node of its family (Top-N: or Custom Scan (SiftAgg)) and the rows equal the reference's. A variant that keeps the normal plan is
named by its refusal slug (SET sift.explain_refusals) and excluded from the percentage when the registry of src/agg/refuse.h marks that slug
as by design; the rest are gaps.

  test/matrix/matrix.py run --label LABEL --out FILE [--tier quick|full] [--rows N] [--by-design FILE]
  test/matrix/matrix.py tsv FILE... --build NAME         the rows of bench/results/coverage-pct.tsv

Needs a started scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT) with the extension installable.
"""
import argparse
import collections
import datetime
import hashlib
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
TMP = os.environ.get('SIFT_TMP', '/tmp/sift-arch')
ENV = dict(os.environ, PGHOST=f'{TMP}/sock', PGPORT=os.environ.get('SIFT_PORT', '56100'), PGUSER='postgres',
           PGOPTIONS='-c client_min_messages=notice -c lc_messages=C', LC_MESSAGES='C', LANGUAGE='C')
DB = 'sift_matrix'
FAMILIES = {'08': 'WHERE ... ORDER BY x LIMIT n of rows', '13': 'RIGHT JOIN, SELECT DISTINCT, volatile functions'}


def psql(db, text, check=True, timeout=120):
    p = subprocess.run(['psql', '-X', '-q', '-At', '-d', db, '-v', 'ON_ERROR_STOP=%d' % check], input=text, capture_output=True, text=True,
                       env=ENV, timeout=timeout)
    if check and p.returncode:
        sys.exit(f'psql {db}: {p.stderr.strip()[:1500]}')
    return p.stdout, p.stderr


def define(name):
    m = re.search(rf'#define\s+{name}\s+(\d+)', open(f'{REPO}/src/agg/topn.h').read())
    return int(m.group(1))


def registry():
    """slug -> by_design, from the X-macro of src/agg/refuse.h: X(ID, "slug", "sentence", limit, by_design)"""
    txt = open(f'{REPO}/src/agg/refuse.h').read()
    return {m.group(1): m.group(2) == '1' for m in re.finditer(r'X\(\w+,\s*"(\w+)",\s*"(?:[^"\\]|\\.)*(?:"\s*")?[^"]*",\s*[01],\s*([01])\)', txt)}


# ---------- what the server says ----------
SAMPLES = {   # typcategory -> expressions of v (an integer of the row) tried in order, the first that casts is the column's value
    'N': ['({v})::{t}'],
    'B': ['({v} % 3 = 0)::{t}'],
    'D': ["(timestamp '2024-01-01' + {v} * interval '97 seconds')::text::{t}"],
    'T': ["({v} || ' minutes')::{t}"],
    'S': ["('s' || lpad({v}::text, 6, '0'))::{t}"],
    'I': ["('10.0.' || {v} % 250 || '.' || {v} / 250)::{t}"],
    'V': ["({v} % 256)::bit(8)::{t}"],
    'U': ['md5({v}::text)::{t}', "decode(md5({v}::text), 'hex')::{t}"],
}


def indexable(typ, expr):
    e = expr.format(v='(i % 3000)', t=typ)
    _, err = psql(DB, f"DROP TABLE IF EXISTS probe; CREATE TEMP TABLE probe AS SELECT {e} AS c FROM generate_series(1, 5) i; CREATE INDEX ON probe USING sift_col (c);", check=False)
    return 'ERROR' not in err


def key_types():
    """the column types a sift_col index takes: [(column suffix, type, category, value expression of v)]"""
    cands, _ = psql('postgres', """SELECT t.typname, t.oid::regtype, t.typcategory FROM pg_type t JOIN pg_opclass oc ON oc.opcdefault AND oc.opcintype = t.oid
		JOIN pg_am am ON am.oid = oc.opcmethod AND am.amname = 'btree' WHERE t.typtype = 'b' AND t.typnamespace = 'pg_catalog'::regnamespace
		AND t.typcategory <> 'A' AND t.oid NOT IN ('oid'::regtype, 'regclass'::regtype) ORDER BY t.oid""")
    out = []
    for line in cands.split('\n'):
        if line:
            name, typ, cat = line.split('|')
            for expr in SAMPLES.get(cat, []):
                if indexable(typ, expr):
                    out.append((name.strip('_'), typ, cat, expr))
                    break
    return out


def aggregates():
    """the aggregates with one argument and a scalar result that accept an integer column, by name"""
    names, _ = psql('postgres', """SELECT DISTINCT p.proname FROM pg_aggregate a JOIN pg_proc p ON p.oid = a.aggfnoid
		WHERE a.aggkind = 'n' AND p.pronargs = 1 AND p.pronamespace = 'pg_catalog'::regnamespace ORDER BY 1""")
    out = []
    for n in names.split():
        o, e = psql(DB, f"SELECT {n}(DISTINCT c_int4) FROM facts_c", check=False)
        if 'ERROR' not in e and not re.search(r'[\[{]', o):
            out.append(n)
    return out


def functions():
    """(name, kind, call, is boolean-able) for the volatile (v), stable (s) and immutable (i) zero-argument functions of pg_catalog and the mxf_ ones"""
    out, _ = psql('postgres', """SELECT proname, provolatile FROM pg_proc WHERE prokind = 'f' AND pronargs = 0 AND pronamespace = 'pg_catalog'::regnamespace
		AND proname IN ('random', 'clock_timestamp', 'timeofday', 'gen_random_uuid', 'now', 'statement_timestamp', 'transaction_timestamp', 'pi')
		UNION ALL SELECT proname, provolatile FROM pg_proc WHERE proname LIKE 'mxf\\_%' ORDER BY 1""")
    return [tuple(r.split('|')) for r in out.split() if r]


# ---------- the variants ----------
class V:
    def __init__(self, fam, sub, sql, expect, ordered=True, prep=None):
        self.fam, self.sub, self.sql, self.expect, self.ordered, self.prep = fam, sub, sql, expect, ordered, prep
        self.id = hashlib.md5((fam + sub + sql + str(prep)).encode()).hexdigest()[:8]


TOPN, AGG = 'Top-N: ', 'Custom Scan (SiftAgg)'


def lit(col):
    v, _ = psql(DB, f"SELECT {col}::text FROM facts_c WHERE id = 1001 AND {col} IS NOT NULL UNION ALL SELECT {col}::text FROM facts_c WHERE {col} IS NOT NULL LIMIT 1")
    return "'" + v.split('\n')[0].replace("'", "''") + "'"


def topn_key_types():
    """the type names sift_topn_key_ok (src/agg/topn.c) lists, in the order of the source"""
    src = open(f'{REPO}/src/agg/topn.c').read()
    body = src[src.index('sift_topn_key_ok'):]
    body = body[:body.index('\n}\n')]
    return list(dict.fromkeys(m.lower() for m in re.findall(r'case ([A-Z0-9]+)OID:', body)))


def topn_variants(types):
    t = {n: (typ, cat) for n, typ, cat, _ in types}
    cols = [n for n, *_ in types]
    out = []

    def top(sub, order, sel=None, where='f.cat < 30', limit='10', off='', tbl='facts_c', total=True, tail='', prep=None, join='', pre='', ordered=True):
        sel = sel or ('f.id, ' + order.split(',')[0].split(' ')[0].replace('NULLS', '') if total else order)
        out.append(V('08', sub, f"{pre}SELECT {sel} FROM {tbl} f {join}{'WHERE ' + where if where else ''} ORDER BY {order}{' LIMIT ' + limit if limit else ''}{off}{tail}", TOPN, ordered, prep))

    base = 'int4' if 'int4' in t else cols[0]
    firstcat = {}
    for n, _, cat, _ in types:
        firstcat.setdefault(cat, n)
    for n in cols:
        for d in ('', ' DESC'):
            top('key type', f'f.c_{n}{d}, f.id', sel=f'f.id, f.c_{n}')
    for n in [x for x in topn_key_types() if x in t][:4]:
        for d in ('', ' DESC'):
            for nl in (' NULLS FIRST', ' NULLS LAST'):
                top('NULLS', f'f.c_{n}{d}{nl}, f.id', sel=f'f.id, f.c_{n}')
    numeric = [n for n in topn_key_types() if n in t][:define('SIFT_TOPN_MAX_KEYS') + 2]
    for k in range(1, len(numeric) + 1):
        for dirs in ('', ' DESC'):
            o = ', '.join(f'f.c_{n}{dirs if j % 2 == 0 else ""}' for j, n in enumerate(numeric[:k])) + ', f.id'
            top('key count', o, sel='f.id, ' + ', '.join(f'f.c_{n}' for n in numeric[:k]))
    cap = define('SIFT_TOPN_MAX_ROWS')
    for lim, off in [('1', ''), ('10', ''), ('1000', ''), (str(cap - 1), ''), (str(cap), ''), (str(cap + 1), ''), (str(10 * cap), ''), ('10', ' OFFSET 1000'),
                     (str(cap - 10), ' OFFSET 10'), (str(cap - 10), ' OFFSET 11')]:
        top('LIMIT and OFFSET', f'f.c_{base} DESC, f.id', sel=f'f.id, f.c_{base}', limit=lim, off=off)
    for args, q in [('10', 'LIMIT $1'), ('10', 'LIMIT $1 OFFSET 5'), ('5, 10', 'LIMIT $2 OFFSET $1')]:
        n = args.count(',') + 1
        top('LIMIT and OFFSET', f'f.c_{base} DESC, f.id', sel=f'f.id, f.c_{base}', limit=None, off='', tail=' ' + q,
            prep=(', '.join(['int'] * n), args))
    for lim in ('FETCH FIRST 10 ROWS ONLY', 'FETCH FIRST 10 ROWS WITH TIES'):
        top('LIMIT and OFFSET', f'f.c_{base} DESC', sel=f'f.c_{base}', limit=None, tail=' ' + lim, total=False)
    # the filters: the operators of the btree opfamily on a sample of each category, and the shapes of the grammar
    for n in firstcat.values():
        v = lit(f'c_{n}')
        c = f'f.c_{n}'
        for sub_w in (f'{c} = {v}', f'{c} < {v}', f'{c} >= {v}', f'{c} IN ({v}, {v})', f'{c} IS NULL', f'{c} IS NOT NULL', f'{c} <> {v}',
                      f'{c} < {v} OR f.cat = 3', f'NOT ({c} < {v})'):
            top('filter', f'f.c_{base} DESC, f.id', sel=f'f.id, f.c_{base}', where=sub_w)
    for w in ('f.cat BETWEEN 5 AND 25', 'f.cat NOT IN (1, 2, 3)', 'f.cat = ANY (ARRAY[1, 2, 3])', 'f.cat % 7 = 3', 'mxf_imm(f.cat) = 3', 'mxf_stab(f.cat) = 3',
              'f.c_text LIKE $$s0001%$$', 'f.c_text LIKE $$%99%$$', 'f.c_text ~ $$^s00[0-4]$$', 'lower(f.c_text) = $$s000001$$', 'f.cat IN (SELECT id FROM dims WHERE kind = $$kind_a$$)',
              'EXISTS (SELECT 1 FROM dims d WHERE d.id = f.cat AND d.kind = $$kind_a$$)', 'f.cat < (SELECT 30)', 'f.c_date < current_date',
              "f.body @@> 'gift'", "f.body @@> 'cable wire'", "f.body @@> 'gift' AND f.cat < 20", "f.body @@> 'gift' OR f.body @@> 'cable'",
              "f.body @@> 'gift' OR f.cat = 3", "NOT f.body @@> 'gift'", 'f.body IS NOT NULL', 'f.cat = 3 OR f.cat = 5', 'f.cat < 30 AND f.c_int4 > 100',
              'random() < 0.5', 'f.cat < mxf_vol(3)', None):
        if w and not all(x in t for x in re.findall(r'c_(\w+)', w)):
            continue
        top('filter', f'f.c_{base} DESC, f.id', sel=f'f.id, f.c_{base}', where=w)
    # the order: ties and tie-breaks
    for o, total in [('f.c_int2', False), ('f.c_int2, f.id', True), ('f.c_int2, f.id DESC', True), ('f.c_int2 DESC, f.id', True), ('f.c_int2, f.c_int4', False),
                     ('f.c_int2, f.c_int4, f.id', True), ('f.c_int2 DESC, f.c_text', False)]:
        if all(x in t for x in re.findall(r'f\.c_(\w+)', o)):
            sel = o.replace(' DESC', '') if not total else 'f.id, ' + re.sub(r', f\.id( DESC)?', '', o).replace(' DESC', '')
            top('tie-break', o, sel=sel, total=total)
    # the table kinds, on the shapes of the other dimensions
    shapes = [(f'f.c_{base} DESC, f.id', 'f.cat < 30', ''), (f'f.c_{base}, f.id', None, ' OFFSET 7'), ('f.id DESC', 'f.cat < 5', ''),
              (f'f.c_{base} DESC, f.id', "f.body @@> 'gift'", ''), (f'f.c_{base} DESC, f.id', "f.body @@> 'gift' OR f.body @@> 'cable'", '')]
    if 'c_text' in cols or 'text' in cols:
        shapes.append(('f.c_text, f.id', 'f.cat < 30', ''))
        shapes.append(('lower(f.c_text), f.id', 'f.cat < 30', ''))
    for tbl in ('facts_c', 'facts_t', 'facts_p'):
        for o, w, off in shapes:
            top('table kind', o, sel='f.id, ' + o.split(',')[0].replace(' DESC', ''), where=w, tbl=tbl, off=off)
    # the sort expressions of the types of the categories
    for n in firstcat.values():
        c = f'f.c_{n}'
        cat = [x for x in types if x[0] == n][0][2]
        forms = {'N': [f'-{c}', f'{c} * 2', f'abs({c})', f'{c} + {c}', f'coalesce({c}, 0)', f'{c}::text'], 'D': [f'{c} + 1', f"date_trunc('month', {c})", f'{c}::text'],
                 'S': [f'lower({c})', f'upper({c})', f'length({c})', f'{c} || {c}'], 'B': [f'NOT {c}']}.get(cat, [])
        for e in forms:
            try:
                top('sort expression', f'{e}, f.id', sel=f'f.id, {e}')
            except Exception:
                pass
    # the rest of the query: what the Top-N sits in
    k = f'f.c_{base}'
    for sub, sql in [
        ('composition', f"SELECT f.id, {k}, d.kind FROM facts_c f JOIN dims d ON d.id = f.cat WHERE d.kind = 'kind_a' ORDER BY {k} DESC, f.id LIMIT 10"),
        ('composition', f"SELECT f.id, {k}, d.kind FROM facts_c f LEFT JOIN dims_h d ON d.id = f.cat ORDER BY {k} DESC, f.id LIMIT 10"),
        ('composition', f"SELECT f.id, d.kind FROM facts_c f JOIN dims d ON d.id = f.cat ORDER BY d.kind, f.id LIMIT 10"),
        ('composition', f"SELECT f.id, {k} FROM facts_c f WHERE f.cat IN (SELECT id FROM dims_h) ORDER BY {k} DESC, f.id LIMIT 10"),
        ('composition', f"SELECT f.id, {k} FROM facts_c f WHERE NOT EXISTS (SELECT 1 FROM dims_h d WHERE d.id = f.cat) ORDER BY {k} DESC, f.id LIMIT 10"),
        ('composition', f"SELECT DISTINCT {k} FROM facts_c f ORDER BY {k} DESC LIMIT 10"),
        ('composition', f"SELECT DISTINCT ON (f.cat) f.cat, f.id, {k} FROM facts_c f ORDER BY f.cat, {k} DESC, f.id LIMIT 10"),
        ('composition', f"SELECT f.id, row_number() OVER (ORDER BY {k}, f.id) FROM facts_c f ORDER BY {k}, f.id LIMIT 10"),
        ('composition', f"SELECT f.id, {k} FROM facts_c f ORDER BY {k} DESC, f.id LIMIT 10 FOR UPDATE"),
        ('composition', f"SELECT f.id, {k} FROM facts_c f ORDER BY {k} DESC, f.id LIMIT 10 FOR SHARE"),
        ('composition', f"SELECT * FROM (SELECT f.id, {k} FROM facts_c f ORDER BY {k} DESC, f.id LIMIT 10) s ORDER BY 2 DESC, 1"),
        ('composition', f"WITH s AS (SELECT f.id, {k} FROM facts_c f ORDER BY {k} DESC, f.id LIMIT 10) SELECT * FROM s ORDER BY 2 DESC, 1"),
        ('composition', f"(SELECT f.id, {k} FROM facts_c f WHERE f.cat < 10 ORDER BY {k}, f.id LIMIT 5) UNION ALL (SELECT f.id, {k} FROM facts_c f WHERE f.cat > 30 ORDER BY {k}, f.id LIMIT 5) ORDER BY 2, 1"),
        ('composition', f"SELECT d.id, x.id FROM dims d CROSS JOIN LATERAL (SELECT f.id FROM facts_c f WHERE f.cat = d.id ORDER BY {k} DESC, f.id LIMIT 3) x ORDER BY 1, 2"),
        ('composition', f"SELECT f.id, {k} FROM facts_c f TABLESAMPLE SYSTEM (100) ORDER BY {k} DESC, f.id LIMIT 10"),
    ]:
        out.append(V('08', sub, sql, TOPN))
    for sel in ('f.id, f.c_int4' if 'int4' in cols else 'f.id', 'f.id, f.cat * 2', '*', 'f.id, f.body', 'f.id, f.pad', 'f.cat, f.id', 'f.id AS x, f.cat AS y', 'f.id, lower(f.body)'):
        top('select list', f'f.cat DESC, f.id', sel=sel)
    return out


def join_variants(types):
    t = {n for n, *_ in types}
    out = []
    A = lambda sub, sql, ordered=True: out.append(V('13', sub, sql, AGG, ordered))
    grp = 'd.kind, count(*) AS n, sum(f.cat) AS s'
    grpf = 'd.kind, count(f.id) AS n, sum(f.cat) AS s'
    for tbl in ('facts_c', 'facts_t', 'facts_p'):
        sub = 'join kind' if tbl == 'facts_c' else 'table kind'
        for jt in ('JOIN', 'LEFT JOIN', 'RIGHT JOIN', 'FULL JOIN'):
            A(sub, f'SELECT {grp} FROM {tbl} f {jt} dims d ON d.id = f.cat GROUP BY 1 ORDER BY 1')
            A(sub, f'SELECT {grpf} FROM dims d {jt} {tbl} f ON d.id = f.cat GROUP BY 1 ORDER BY 1')
            A(sub, f'SELECT {grp} FROM {tbl} f {jt} dims_h d ON d.id = f.cat GROUP BY 1 ORDER BY 1')
        A(sub, f'SELECT f.cat % 5, count(*) FROM {tbl} f WHERE EXISTS (SELECT 1 FROM dims_h d WHERE d.id = f.cat) GROUP BY 1 ORDER BY 1')
        A(sub, f'SELECT f.cat % 5, count(*) FROM {tbl} f WHERE f.cat NOT IN (SELECT id FROM dims_h) GROUP BY 1 ORDER BY 1')
        A(sub, f'SELECT f.cat % 5, count(*) FROM {tbl} f WHERE NOT EXISTS (SELECT 1 FROM dims_h d WHERE d.id = f.cat) GROUP BY 1 ORDER BY 1')
        A(sub, f'SELECT x.kind, count(*) FROM {tbl} f CROSS JOIN LATERAL (SELECT d.kind FROM dims d WHERE d.id = f.cat) x GROUP BY 1 ORDER BY 1')
        A(sub, f'SELECT x.kind, count(*) FROM {tbl} f LEFT JOIN LATERAL (SELECT d.kind FROM dims_h d WHERE d.id = f.cat LIMIT 1) x ON true GROUP BY 1 ORDER BY 1')
    for jt in ('JOIN', 'LEFT JOIN', 'RIGHT JOIN', 'FULL JOIN'):
        for cond in ('d.id = f.cat AND d.w > 3', 'd.id = f.cat AND f.cat < 30', 'd.id = f.cat + 1', 'd.id = f.cat AND d.label = f.c_text', 'd.id < f.cat',
                     'd.id = f.cat OR d.id = f.c_int2', 'd.label = f.c_text', 'd.id IS NOT DISTINCT FROM f.cat', 'true', 'd.id BETWEEN f.cat AND f.cat + 1',
                     'lower(d.label) = f.c_text', 'd.id = f.c_int4 % 40'):
            if not all(x in t for x in re.findall(r'c_(\w+)', cond)):
                continue
            A('join condition', f'SELECT {grp} FROM facts_c f {jt} dims_h d ON {cond} GROUP BY 1 ORDER BY 1')
        for where in ('d.w > 3', 'd.id IS NULL', 'f.cat < 30', 'd.id IS NULL OR f.cat < 30', 'f.id IS NULL'):
            A('join condition', f'SELECT {grp} FROM facts_c f {jt} dims_h d ON d.id = f.cat WHERE {where} GROUP BY 1 ORDER BY 1')
    kids = ['dims', 'dims_p', 'dims_v', '(SELECT * FROM dims WHERE id < 30)', '(SELECT id, kind FROM dims GROUP BY id, kind)', "(VALUES (1, 'kind_a'), (2, 'kind_b')) AS v0(id, kind)",
            '(SELECT g AS id, $$k$$ || (g % 3) AS kind FROM generate_series(0, 39) g)', 'unnest(ARRAY[1, 2, 3]) WITH ORDINALITY AS d(id, kind)', 'orders2']
    for k in kids:
        ref = k if k.startswith(('dims', 'orders2', '(VALUES')) or 'unnest' in k else k + ' d'
        if ref.startswith(('dims', 'orders2')) and ' ' not in ref:
            ref += ' d'
        al = 'v0' if k.startswith('(VALUES') else 'd'
        col = f'{al}.id'
        cols = col if 'orders2' in k else f'{al}.kind'
        for jt in ('JOIN', 'RIGHT JOIN', 'FULL JOIN'):
            A('join child', f'SELECT {cols} AS g, count(*) FROM facts_c f {jt} {ref} ON {col} = f.cat GROUP BY 1 ORDER BY 1')
    A('join child', 'WITH c AS (SELECT * FROM dims WHERE id < 30) SELECT c.kind, count(*) FROM facts_c f JOIN c ON c.id = f.cat GROUP BY 1 ORDER BY 1')
    A('join child', 'SELECT d2.name, count(*) FROM facts_c f JOIN dims d ON d.id = f.cat JOIN dims2 d2 ON d2.id = d.grp GROUP BY 1 ORDER BY 1')
    A('join child', 'SELECT d2.name, count(*) FROM facts_c f RIGHT JOIN dims d ON d.id = f.cat JOIN dims2 d2 ON d2.id = d.grp GROUP BY 1 ORDER BY 1')
    A('join child', 'SELECT d2.name, count(*) FROM facts_c f JOIN dims d ON d.id = f.cat FULL JOIN dims2 d2 ON d2.id = d.grp GROUP BY 1 ORDER BY 1')
    A('join child', 'SELECT g.cat, count(*) FROM facts_c f JOIN facts_t g ON g.id = f.id GROUP BY 1 ORDER BY 1')
    nine = ' '.join(f'JOIN dims d{i} ON d{i}.id = f.cat' for i in range(1, 10))
    A('join child', f'SELECT d1.kind, count(*) FROM facts_c f {nine} GROUP BY 1 ORDER BY 1')
    # DISTINCT
    for n in [x[0] for x in types]:
        A('DISTINCT', f'SELECT DISTINCT f.c_{n} FROM facts_c f WHERE f.cat < 30', False)
    for s in ('f.cat, f.c_int2', 'f.cat % 10', 'f.cat, f.cat % 10', 'f.c_text, f.c_bool', 'lower(f.c_text)', 'f.cat, f.c_text, f.c_bool', 'f.c_int4 % 10', 'f.c_date + 1'):
        if all(x in t for x in re.findall(r'c_(\w+)', s)):
            A('DISTINCT', f'SELECT DISTINCT {s} FROM facts_c f', False)
    for q in ('SELECT DISTINCT f.cat FROM facts_c f ORDER BY 1 LIMIT 5', 'SELECT DISTINCT d.kind FROM facts_c f JOIN dims d ON d.id = f.cat',
              'SELECT DISTINCT d.kind, f.cat % 3 FROM facts_c f RIGHT JOIN dims d ON d.id = f.cat', 'SELECT DISTINCT d.kind FROM facts_c f FULL JOIN dims d ON d.id = f.cat',
              'SELECT DISTINCT count(*) FROM facts_c f GROUP BY f.cat', 'SELECT DISTINCT f.cat, f.c_text FROM facts_c f GROUP BY f.cat, f.c_text',
              'SELECT DISTINCT f.cat, count(*) FROM facts_c f GROUP BY f.cat', 'SELECT DISTINCT sum(f.cat) FROM facts_c f GROUP BY f.c_text',
              'SELECT DISTINCT rank() OVER (ORDER BY f.cat) FROM facts_c f', 'SELECT DISTINCT f.cat, max(f.id) OVER (PARTITION BY f.cat) FROM facts_c f',
              'SELECT count(*) FROM (SELECT DISTINCT f.cat, f.c_text FROM facts_c f) s', 'SELECT DISTINCT s.cat FROM (SELECT DISTINCT f.cat, f.c_text FROM facts_c f) s',
              'SELECT DISTINCT f.cat FROM facts_c f UNION SELECT DISTINCT f.cat + 1 FROM facts_c f',
              'SELECT DISTINCT f.cat, count(*) FROM facts_c f GROUP BY f.cat HAVING count(*) > 100'):
        if not all(x in t for x in re.findall(r'c_(\w+)', q)):
            continue
        A('DISTINCT over grouped, windowed or nested', q, 'ORDER BY' in q)
    for tbl in ('facts_t', 'facts_p'):
        A('DISTINCT', f'SELECT DISTINCT f.cat, f.c_text FROM {tbl} f WHERE f.cat < 30', False)
        A('DISTINCT ON', f'SELECT DISTINCT ON (f.cat) f.cat, f.id, f.c_int4 FROM {tbl} f ORDER BY f.cat, f.c_int4 DESC, f.id')
    for on, o, ok in [('f.cat', 'f.cat, f.c_int4 DESC, f.id', 'f.id, f.c_int4'), ('f.cat', 'f.cat, f.c_int4 DESC', 'f.c_int4'), ('f.cat', 'f.cat, f.c_ts DESC NULLS FIRST, f.id', 'f.id'),
                      ('f.cat, f.c_int2', 'f.cat, f.c_int2, f.c_int4, f.id', 'f.id'), ('f.cat % 7', 'f.cat % 7, f.c_int4 DESC, f.id', 'f.id'), ('lower(f.c_text)', 'lower(f.c_text), f.id', 'f.id'),
                      ('f.cat', 'f.cat, f.c_int4, f.c_date, f.c_numeric, f.c_float8, f.id', 'f.id'), ('f.cat', 'f.cat, f.id DESC', 'f.id'), ('f.c_text', 'f.c_text, f.c_int4 DESC, f.id', 'f.id')]:
        if all(x in t for x in re.findall(r'c_(\w+)', on + o)):
            for w in ('', ' WHERE f.cat < 30'):
                A('DISTINCT ON', f'SELECT DISTINCT ON ({on}) {on}, {ok} FROM facts_c f{w} ORDER BY {o}')
    A('DISTINCT ON', 'SELECT DISTINCT ON (d.kind) d.kind, f.id FROM facts_c f JOIN dims d ON d.id = f.cat ORDER BY d.kind, f.id')
    A('DISTINCT ON', 'SELECT DISTINCT ON (f.cat) f.cat, f.id FROM facts_c f ORDER BY f.cat, f.id LIMIT 5')
    # aggregates with DISTINCT, from pg_aggregate
    for a in aggregates():
        A('aggregate DISTINCT', f'SELECT {a}(DISTINCT f.c_int4) FROM facts_c f', False)
        A('aggregate DISTINCT', f'SELECT f.cat % 5, {a}(DISTINCT f.c_int4) FROM facts_c f GROUP BY 1 ORDER BY 1')
    A('aggregate DISTINCT', 'SELECT f.cat % 5, count(DISTINCT f.cat), count(DISTINCT f.c_text) FROM facts_c f GROUP BY 1 ORDER BY 1')
    A('aggregate DISTINCT', 'SELECT d.kind, count(DISTINCT f.cat) FROM facts_c f JOIN dims d ON d.id = f.cat GROUP BY 1 ORDER BY 1')
    A('aggregate DISTINCT', 'SELECT count(DISTINCT f.cat) FILTER (WHERE f.id % 2 = 0) FROM facts_c f', False)
    A('aggregate DISTINCT', 'SELECT count(DISTINCT lower(f.c_text)) FROM facts_c f', False)
    # volatile, stable and immutable functions, by placement
    for name, vol in functions():
        call = f'mxf_{name[4:]}(f.cat)' if name.startswith('mxf_') else f'{name}()'
        cond = f'({call})::text IS NOT NULL'
        kind = {'v': 'volatile', 's': 'stable', 'i': 'immutable'}[vol]
        sub = 'volatile function' if vol == 'v' else 'stable or immutable function'
        for place, sql in [('WHERE', f'SELECT f.c_text, count(*) FROM facts_c f WHERE {cond} GROUP BY 1 ORDER BY 1'),
                           ('key', f'SELECT ({call})::text IS NULL AS k, count(*) FROM facts_c f GROUP BY 1 ORDER BY 1'),
                           ('argument', f'SELECT f.c_text, count(({call})::text) FROM facts_c f GROUP BY 1 ORDER BY 1'),
                           ('FILTER', f'SELECT f.c_text, count(*) FILTER (WHERE {cond}) FROM facts_c f GROUP BY 1 ORDER BY 1'),
                           ('HAVING', f'SELECT f.c_text, count(*) FROM facts_c f GROUP BY 1 HAVING bool_and({cond}) ORDER BY 1'),
                           ('join', f'SELECT d.kind, count(*) FROM facts_c f JOIN dims d ON d.id = f.cat AND {cond} GROUP BY 1 ORDER BY 1')]:
            if not all(x in t for x in re.findall(r'c_(\w+)', sql)):
                continue
            A(sub, sql)
    return out


# ---------- the data ----------
def build_data(rows, types=None):
    """the database; types: the key types of another run (so that two servers answer the same variants), else the ones this server takes"""
    psql('postgres', f'DROP DATABASE IF EXISTS {DB} WITH (FORCE); CREATE DATABASE {DB}')
    psql(DB, open(f'{HERE}/data.sql').read().replace(':rows', str(rows)))
    types = types or key_types()[:30]
    cols = ', '.join(f"CASE WHEN i % {17 + j} = 0 THEN NULL ELSE {expr.format(v=f'(mx_h(i, {10 + j}) % 3000)', t=typ)} END AS c_{n}" for j, (n, typ, _, expr) in enumerate(types))
    keys = ', '.join(['id', 'cat'] + [f'c_{n}' for n, typ, _, expr in types if indexable(typ, expr)])
    ddl = f"""CREATE TABLE facts_src AS SELECT i AS id, mx_h(i, 1) % 40 AS cat, CASE WHEN i % 41 = 0 THEN NULL ELSE mx_words(i) END AS body, {cols} FROM generate_series(1, {rows}) i;
CREATE TABLE facts_c (LIKE facts_src, pad text DEFAULT repeat('x', 1500), PRIMARY KEY (id));
ALTER TABLE facts_c ALTER COLUMN pad SET STORAGE PLAIN;
INSERT INTO facts_c SELECT * FROM facts_src;
CREATE INDEX facts_c_bm ON facts_c USING sift (body);
CREATE INDEX facts_c_col ON facts_c USING sift_col ({keys}) WITH (write_buffer_limit = 256);
CREATE TABLE facts_t (LIKE facts_src, pad text DEFAULT repeat('x', 1500), PRIMARY KEY (id)) USING sift_table;
INSERT INTO facts_t SELECT * FROM facts_src;
CREATE INDEX facts_t_bm ON facts_t USING sift (body);
CREATE TABLE facts_p (LIKE facts_src, pad text DEFAULT repeat('x', 1500)) PARTITION BY HASH (id);
CREATE TABLE facts_p_0 PARTITION OF facts_p FOR VALUES WITH (MODULUS 3, REMAINDER 0);
CREATE TABLE facts_p_1 PARTITION OF facts_p FOR VALUES WITH (MODULUS 3, REMAINDER 1);
CREATE TABLE facts_p_2 PARTITION OF facts_p FOR VALUES WITH (MODULUS 3, REMAINDER 2);
INSERT INTO facts_p SELECT * FROM facts_src;
CREATE INDEX facts_p_bm ON facts_p USING sift (body);
CREATE INDEX facts_p_col ON facts_p USING sift_col ({keys}) WITH (write_buffer_limit = 256);
DROP TABLE facts_src;
VACUUM (FREEZE, ANALYZE) facts_c; VACUUM (FREEZE, ANALYZE) facts_t; VACUUM (FREEZE, ANALYZE) facts_p;"""
    psql(DB, ddl, timeout=900)
    return types


# ---------- running ----------
ON = "SET sift.enable_agg = on; SET enable_seqscan = off; SET enable_indexscan = off; SET enable_bitmapscan = off;"
REF = "SET sift.enable_agg = off; RESET enable_seqscan; RESET enable_indexscan; RESET enable_bitmapscan;"


def section(text, name):
    m = re.search(rf'^@@{name}\n(.*?)(?=^@@|\Z)', text, re.S | re.M)
    return m.group(1) if m else ''


def run_one(v):
    if v.prep:
        head = f'PREPARE q({v.prep[0]}) AS {v.sql}; SET plan_cache_mode = force_generic_plan;'
        explain, query = f'EXPLAIN (COSTS OFF) EXECUTE q({v.prep[1]})', f'EXECUTE q({v.prep[1]})'
    else:
        head, explain, query = '', f'EXPLAIN (COSTS OFF) {v.sql}', v.sql
    script = f"""\\set ON_ERROR_STOP off
SET statement_timeout = '30s'; SET max_parallel_workers_per_gather = 0; {ON} {head}
SET sift.explain_refusals = on;
\\echo @@PLAN
\\warn @@PLAN
{explain};
SET sift.explain_refusals = off;
\\echo @@ON
\\warn @@ON
{query};
{REF}
\\echo @@REF
\\warn @@REF
{query};
\\echo @@END
\\warn @@END
"""
    p = subprocess.run(['psql', '-X', '-q', '-At', '-F', '|', '-d', DB], input=script, capture_output=True, text=True, env=ENV, timeout=240)
    return p.stdout, p.stderr


def close(a, b):
    if a == b:
        return True
    try:
        x, y = float(a), float(b)
    except ValueError:
        return False
    return abs(x - y) <= 1e-12 + 1e-9 * max(abs(x), abs(y))


def same(a, b, ordered):
    a, b = a.split('\n')[:-1], b.split('\n')[:-1]
    if not ordered:
        a, b = sorted(a), sorted(b)
    return len(a) == len(b) and all(len(x.split('|')) == len(y.split('|')) and all(close(i, j) for i, j in zip(x.split('|'), y.split('|'))) for x, y in zip(a, b))


def classify(v, out, err, slugs):
    """(status, slug): covered, wrong, refused (with the slug of the arm of the family, '' when the server names none) or invalid (the reference fails)"""
    plan, on, ref = section(out, 'PLAN'), section(out, 'ON'), section(out, 'REF')
    if 'ERROR' in section(err, 'REF'):
        return 'invalid', ''
    if 'ERROR' in section(err, 'ON'):
        return 'wrong', ''
    if v.expect in plan:
        return ('covered' if same(on, ref, v.ordered) else 'wrong'), ''
    arms = re.findall(r'SiftAgg not used: (\w+):[^\n]*\n(?:[^\n]*\n)?DETAIL:\s+arm: (\S+)', section(err, 'PLAN'))
    want = 'topn' if v.expect == TOPN else ('distinct' if 'DISTINCT' in v.sql.upper().split('FROM')[0] and 'COUNT(' not in v.sql.upper() else 'single')
    mine = [s for s, a in arms if a.split(':')[-1] == want and s in slugs] or [s for s, _ in arms if s in slugs]
    return 'refused', mine[-1] if mine else ('?' if slugs and arms else '')


def variants_of(types):
    seen = set()
    return [v for v in topn_variants(types) + join_variants(types) if not (v.id in seen or seen.add(v.id))]


def run(args):
    import json
    slugs = registry()
    saved = json.load(open(args.from_)) if args.from_ else None
    types = build_data(args.rows, [tuple(t) for t in saved['types']] if saved else None)
    if saved:
        variants = [V(*x['key']) for x in saved['variants']]
        for v, x in zip(variants, saved['variants']):
            v.id = x['id']
    else:
        variants = variants_of(types)
        if args.tier == 'quick':
            variants = [v for v in variants if int(v.id, 16) % 3 == 0]
    if args.save:
        json.dump({'types': types, 'variants': [{'id': v.id, 'key': [v.fam, v.sub, v.sql, v.expect, v.ordered, v.prep]} for v in variants]}, open(args.save, 'w'))
    designed = {r[0]: r[5] for r in (l.rstrip('\n').split('\t') for l in open(args.by_design))} if args.by_design else {}
    from concurrent.futures import ThreadPoolExecutor
    with ThreadPoolExecutor(args.jobs) as ex:
        outs = list(ex.map(run_one, variants))
    rows = []
    for v, (o, e) in zip(variants, outs):
        st, slug = classify(v, o, e, slugs)
        rows.append([v.id, v.fam, v.sub, st, slug, '1' if slugs.get(slug) else '0', v.sql.replace('\t', ' ').replace('\n', ' ')])
    if args.fixtures:
        rows += [r for r in fixture_rows(args.fixtures, slugs) if r[1] in {v.fam for v in variants}]
    with open(args.out, 'w') as f:
        for r in rows:
            r[5] = designed.get(r[0], r[5])
            f.write('\t'.join(r) + '\n')


# ---------- the fixtures ----------
def fixture_rows(db, slugs):
    """the result rows of the coldiff fixtures of each family: those the proofs of the family's rows in the design notes cite, and the ones of the shapes
    of those (a trap is a shape of its own: only the cited ones), as run by scripts/coldiff.sh on the 'on' variant into coldiff.results of db"""
    shape_of, cited = {}, collections.defaultdict(set)
    for f in os.listdir(f'{REPO}/test/col/fixtures'):
        m = re.search(r'^-- shape: (\S+)', open(f'{REPO}/test/col/fixtures/{f}').read(), re.M)
        shape_of[f[:-4]] = m.group(1) if m else ''
    for line in open(f'{REPO}/docs/coverage.tsv'):
        c = line.rstrip('\n').split('\t')
        if len(c) == 14 and re.match(r'\d\d:', c[13]):
            cited[c[13][:2]] |= {x[8:] for x in c[8].split(',') if x.startswith('fixture:')}
    out, _ = psql(db, "SELECT DISTINCT ON (split_part(name, '@', 1)) split_part(name, '@', 1), ok, coalesce(used, false), coalesce(slug, '') FROM coldiff.results "
                      "WHERE variant = 'on' AND scenario NOT LIKE 'scenario-%' AND coalesce(detail, '') NOT LIKE 'requires not met%' ORDER BY 1, scenario")
    res = {r.split('|')[0]: r.split('|')[1:] for r in out.split('\n') if r}
    rows = []
    for fam, names in sorted(cited.items()):
        shapes = {shape_of.get(n) for n in names} - {'trap', None, ''}
        for n in sorted(names | {n for n, sh in shape_of.items() if sh in shapes}):
            if n not in res:
                continue
            ok, used, slug = res[n][0] == 't', res[n][1] == 't', res[n][2]
            st = 'wrong' if not ok else 'covered' if used else 'refused'
            rows.append([hashlib.md5(('fixture' + n).encode()).hexdigest()[:8], fam, 'fixtures: ' + shape_of[n], st, slug, '1' if slugs.get(slug) else '0', 'fixture ' + n])
    return rows


# ---------- the report ----------
def tests_of(fam):
    kw = {'08': ['topn'], '13': ['join', 'distinct']}[fam]
    names = sorted(os.path.basename(p)[:-4] for p in subprocess.run(['git', '-C', REPO, 'ls-files', 'test/sql'], capture_output=True, text=True).stdout.split()
                   if p.endswith('.sql') and any(k in p for k in kw))
    return ','.join(['script:coverage'] + [f'test:{n}' for n in names])


def tsv(args):
    rows = [l.rstrip('\n').split('\t') for l in open(args.file)]
    tally = collections.defaultdict(collections.Counter)
    for _, fam, sub, st, slug, by, _ in rows:
        if st == 'invalid':
            continue
        for key in ((fam, sub), (fam, '*')) + (((fam, 'refused:' + slug),) if st == 'refused' and slug else ()):
            c = tally[key]
            c['variants'] += 1
            c['pushed'] += st == 'covered'
            c['wrong'] += st == 'wrong'
            c['by_design'] += by == '1' and st == 'refused'
    date = datetime.date.today().isoformat()
    order = lambda kv: (kv[0][0], not kv[0][1] == '*', kv[0][1].startswith('refused:'), kv[0][1])
    for (fam, sub), c in sorted(tally.items(), key=order):
        gap = sub.startswith('refused:')
        den = c['variants'] - c['by_design']
        print('\t'.join([fam, sub, str(c['variants']), str(c['pushed']), str(c['by_design']), str(c['wrong']),
                         '-' if gap or not den else f'{100 * c["pushed"] / den:.1f}', args.build, date,
                         tests_of(fam) if sub == '*' else 'script:coldiff' if sub.startswith('fixtures') else 'script:coverage']))


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest='cmd', required=True)
    r = sp.add_parser('run')
    r.add_argument('--out', required=True)
    r.add_argument('--tier', default='full', choices=['quick', 'full'])
    r.add_argument('--rows', type=int, default=24000)
    r.add_argument('--jobs', type=int, default=2)
    r.add_argument('--by-design', help='the result file of another run: its by-design flag per variant')
    r.add_argument('--fixtures', help='the database of a scripts/coldiff.sh --keep run: its fixture results join the variants')
    r.add_argument('--save', help='write the types and variants of this run (JSON)')
    r.add_argument('--from', dest='from_', help='run the types and variants of a saved run')
    t = sp.add_parser('tsv')
    t.add_argument('file')
    t.add_argument('--build', required=True)
    a = ap.parse_args()
    run(a) if a.cmd == 'run' else tsv(a)
