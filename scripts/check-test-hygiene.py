#!/usr/bin/env python3
"""The hygiene of the test manifests and of the high-level harnesses (Spec 84), in lint.sh.

  scripts/check-test-hygiene.py [--strict-harnesses] [--root DIR]
  scripts/check-test-hygiene.py --selftest

Manifests (no PostgreSQL, no build):
  - scripts/regress-times.tsv names no test that test/sql no longer has, and no test twice;
  - every TESTING_REGRESS line of scripts/testing-regress.mk names a test (or a pattern with %) that test/sql has;
  - scripts/weights-*.tsv, scratch.tsv name no job twice (the loader keeps the last row, so the first one lied);
  - a `# tap: pooled` shell test (test/t) that runs CREATE DATABASE pins no sift. setting in node_init: the pool sets it on the database of the node alone, never on the ones the test creates.
High-level harnesses (test/{corpus,col,plan,fuzz,matrix}/, scripts/{corpus,coldiff,plangate,coverage,fast-gate}.sh) run on Format 1, Format 2.0 and the release library:
  - a plan node is matched by its prefix (`Custom Scan (SiftAgg` or "SiftAgg), never `Custom Scan (SiftAgg)` or "SiftAgg", which SiftAggV6, SiftAggV6Sip, SiftAggV6TopN... are not;
  - the maintenance and the explanation go through the public 6.0 API (sift.promote, sift.merge, sift.maintain, sift.explain_why), never through the retired
    sift.col_promote, sift.col_merge, sift.table_maintain, sift.col_auto_merge, sift.enable_count or sift._test_explain_why.
The harnesses of the workstreams that have not landed (PENDING below, with the count of findings each had when this check was written) may not get another finding;
--strict-harnesses (and an empty PENDING) makes every finding an error. A pending file with no finding left is reported: delete its line.
"""
import argparse
import collections
import fnmatch
import os
import re
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
HARNESS_DIRS = ['test/corpus', 'test/col', 'test/plan', 'test/fuzz', 'test/matrix', 'scripts/load']
HARNESS_FILES = ['scripts/corpus.sh', 'scripts/coldiff.sh', 'scripts/plangate.sh', 'scripts/coverage.sh', 'scripts/fast-gate.sh']
SUFFIXES = ('.sql', '.py', '.sh', '.bash', '.tsv')
RULES = [
    (re.compile(r"Custom Scan \(SiftAgg\)|%\"SiftAgg\"%|\"SiftAgg\""),
     "an exact SiftAgg plan node: match the prefix (`Custom Scan (SiftAgg` or `\"SiftAgg`), the V6 nodes are SiftAggV6, SiftAggV6Sip, SiftAggV6TopN..."),
    (re.compile(r"sift\.(?:col_promote|col_merge|table_maintain|col_auto_merge|enable_count|_test_explain_why)\b"),
     "a retired SQL name: use the public 6.0 API (sift.promote, sift.merge, sift.maintain, sift.explain_why), or drop the setting the harness no longer needs"),
]
# harness -> (workstream that owns it, findings it had): delete a line when its workstream has fixed the file
PENDING = {}


def rows(path):
    with open(path, encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip('\n')
            if line.strip() and not line.startswith('#'):
                yield n, line.split('\t')


def sql_tests(root):
    d = os.path.join(root, 'test/sql')
    return {os.path.relpath(os.path.join(r, f), d)[:-4] for r, _, files in os.walk(d) for f in files if f.endswith('.sql')}


def check_regress_times(root, tests):
    seen, errs = {}, []
    for n, c in rows(os.path.join(root, 'scripts/regress-times.tsv')):
        if c[0] not in tests:
            errs.append(f"scripts/regress-times.tsv:{n}: {c[0]} has no test/sql/{c[0]}.sql: delete the row (a test that was removed or renamed keeps no time)")
        elif c[0] in seen:
            errs.append(f"scripts/regress-times.tsv:{n}: {c[0]} twice (first at line {seen[c[0]]}): keep the one row of the last measurement")
        seen[c[0]] = n
    return errs


def check_testing_regress(root, tests):
    errs = []
    with open(os.path.join(root, 'scripts/testing-regress.mk'), encoding='utf-8') as f:
        for n, line in enumerate(f, 1):
            m = re.match(r'TESTING_REGRESS \+?= (.*)$', line.rstrip('\n'))
            if not m:
                continue
            for pat in m.group(1).split():
                if not any(fnmatch.fnmatchcase(t, pat.replace('%', '*')) for t in tests):
                    errs.append(f"scripts/testing-regress.mk:{n}: TESTING_REGRESS {pat} matches no test/sql/*.sql: delete the line, or name the test it was meant for")
    return errs


def check_weights(root):
    errs = []
    sd = os.path.join(root, 'scripts')
    for name in sorted(f for f in os.listdir(sd) if re.fullmatch(r'(weights-[a-z]+|scratch)\.tsv', f)):
        seen = {}
        for n, c in rows(os.path.join(sd, name)):
            if c[0] in seen:
                errs.append(f"scripts/{name}:{n}: {c[0]} twice (first at line {seen[c[0]]}): the loader keeps the last row, delete the stale one")
            seen[c[0]] = n
    return errs


POOLED_PIN = re.compile(r'node_init \S+ \d+ "([^"]*)"')


def check_pooled_taps(root):
    errs = []
    for r, _, files in os.walk(os.path.join(root, 'test/t')):
        for f in sorted(x for x in files if x.endswith('.sh')):
            rel = os.path.relpath(os.path.join(r, f), root)
            with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as fh:
                text = fh.read()
            if '\n# tap: pooled\n' not in text or 'CREATE DATABASE' not in text:
                continue
            for m in POOLED_PIN.finditer(text):
                if re.search(r'^sift\.[a-z0-9_]+ = ', m.group(1), re.M):
                    errs.append(f"{rel}:{text.count(chr(10), 0, m.start()) + 1}: a pooled node pins a sift. setting and the test runs CREATE DATABASE, where the pool never sets it: "
                                "drop `# tap: pooled`, so the setting is the server's")
    return sorted(errs)


def harness_files(root):
    out = [f for f in HARNESS_FILES if os.path.isfile(os.path.join(root, f))]
    for d in HARNESS_DIRS:
        for r, dirs, files in os.walk(os.path.join(root, d)):
            dirs[:] = [x for x in dirs if x != '__pycache__']
            out += [os.path.relpath(os.path.join(r, f), root) for f in files if f.endswith(SUFFIXES)]
    return sorted(out)


def findings(root, rel):
    out = []
    with open(os.path.join(root, rel), encoding='utf-8', errors='replace') as f:
        for n, line in enumerate(f, 1):
            out += [(n, why) for rx, why in RULES for _ in rx.finditer(line)]
    return out


def check_harnesses(root, strict):
    errs, notes = [], []
    pending = {} if strict else dict(PENDING)
    seen = set()
    for rel in harness_files(root):
        found = findings(root, rel)
        seen.add(rel)
        if rel in pending:
            ws, allowed = pending[rel]
            if not found:
                notes.append(f"{rel}: clean, {ws} has landed: delete it from PENDING in scripts/check-test-hygiene.py")
            elif len(found) > allowed:
                errs += [f"{rel}:{n}: {why} ({ws} has not landed, and this file may not get more than {allowed})" for n, why in found]
            continue
        errs += [f"{rel}:{n}: {why}" for n, why in found]
    for rel in pending:
        if rel not in seen:
            notes.append(f"{rel}: no such harness any more: delete it from PENDING in scripts/check-test-hygiene.py")
    return errs, notes


def run(root, strict):
    tests = sql_tests(root)
    errs = check_regress_times(root, tests) + check_testing_regress(root, tests) + check_weights(root) + check_pooled_taps(root)
    h_errs, notes = check_harnesses(root, strict)
    return errs + h_errs, notes


def report(root, strict):
    errs, notes = run(root, strict)
    for e in errs:
        print(e)
    for n in notes:
        print('note: ' + n)
    left = collections.Counter(ws for rel, (ws, _) in PENDING.items() if os.path.isfile(os.path.join(root, rel)))
    pend = ', '.join(f'{ws} {n} file(s)' for ws, n in sorted(left.items())) or 'none'
    if errs:
        print(f"check-test-hygiene: {len(errs)} finding(s); fix the manifest or the harness, do not widen PENDING")
        return 1
    print(f"check-test-hygiene: ok (manifests clean; harnesses clean{'' if strict else ', pending: ' + pend})")
    return 0


# ---------- the selftest: a tree of its own for every rule ----------
def put(root, rel, text):
    p = os.path.join(root, rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, 'w', encoding='utf-8') as f:
        f.write(text)


def tree(root, **over):
    files = {'test/sql/a.sql': '', 'test/sql/b.sql': '', 'scripts/testing-regress.mk': 'TESTING_REGRESS =\nTESTING_REGRESS += a\nTESTING_REGRESS += b%\n',
             'scripts/regress-times.tsv': '# h\na\t1.0\nb\t2.0\n', 'scripts/weights-quick.tsv': '# h\nj1\t1\t2\nj2\t3\t4\n', 'scripts/scratch.tsv': 'j1\t5\n',
             'test/corpus/run.py': 'ok\n', 'scripts/plangate.sh': "ok 'Custom Scan (SiftAgg%'\n"}
    files.update(over)
    for rel, text in files.items():
        put(root, rel, text)


def selftest():
    bad = 0
    global PENDING
    saved = PENDING
    cases = [
        ('a clean tree passes', {}, False, {}, []),
        ('a stale regress-times row', {'scripts/regress-times.tsv': 'a\t1\ngone\t3\n'}, False, {}, ['regress-times.tsv:2: gone has no test/sql/gone.sql']),
        ('a regress-times row twice', {'scripts/regress-times.tsv': 'a\t1\na\t2\n'}, False, {}, ['regress-times.tsv:2: a twice']),
        ('a stale TESTING_REGRESS name', {'scripts/testing-regress.mk': 'TESTING_REGRESS += a\nTESTING_REGRESS += gone\n'}, False, {}, ['scripts/testing-regress.mk:2: TESTING_REGRESS gone matches no test']),
        ('a TESTING_REGRESS pattern with no test', {'scripts/testing-regress.mk': 'TESTING_REGRESS += zz%\n'}, False, {}, ['scripts/testing-regress.mk:1: TESTING_REGRESS zz% matches no test']),
        ('a duplicate weights key', {'scripts/weights-quick.tsv': 'j1\t1\nj1\t2\n'}, False, {}, ['weights-quick.tsv:2: j1 twice']),
        ('a duplicate scratch key', {'scripts/scratch.tsv': 'j1\t1\nj1\t2\n'}, False, {}, ['scratch.tsv:2: j1 twice']),
        ('a pooled test that pins a setting and creates a database',
         {'test/t/x.sh': '#!/bin/bash\n# tap: pooled\nnode_init r 3 "autovacuum = off\nsift.test_create_format1 = on"\nq "CREATE DATABASE d"\n'}, False, {},
         ['test/t/x.sh:3: a pooled node pins a sift. setting']),
        ('a pooled test that pins a setting in its own database is fine', {'test/t/x.sh': '#!/bin/bash\n# tap: pooled\nnode_init r 3 "sift.test_create_format1 = on"\n'}, False, {}, []),
        ('a server of its own may pin and create databases', {'test/t/x.sh': '#!/bin/bash\nnode_init r 3 "sift.test_create_format1 = on"\nq "CREATE DATABASE d"\n'}, False, {}, []),
        ('a pooled test that creates databases without a pin is fine',
         {'test/t/x.sh': '#!/bin/bash\n# tap: pooled\nnode_init r 3 "autovacuum = off"\nq "CREATE DATABASE d"\n'}, False, {}, []),
        ('an exact SiftAgg node in a harness', {'test/col/run.sql': "SELECT 1 WHERE p LIKE '%Custom Scan (SiftAgg)%';\n"}, False, {}, ['test/col/run.sql:1: an exact SiftAgg plan node']),
        ('an exact JSON SiftAgg node', {'test/plan/run.sql': "j::text LIKE '%\"SiftAgg\"%'\n"}, False, {}, ['test/plan/run.sql:1: an exact SiftAgg plan node']),
        ('a retired call in a fuzzer', {'test/fuzz/x.py': "q('SELECT sift.col_merge(i)')\n"}, False, {}, ['test/fuzz/x.py:1: a retired SQL name']),
        ('the prefix is fine', {'test/col/run.sql': "p LIKE '%Custom Scan (SiftAgg%' AND j ~ '\"SiftAgg'\n"}, False, {}, []),
        ('the public API is fine', {'test/fuzz/x.py': "q('SELECT sift.merge(i), sift.maintain(t), sift.explain_why(q)')\n"}, False, {}, []),
        ('a pending file keeps its findings', {'test/col/run.sql': "'Custom Scan (SiftAgg)'\n"}, False, {'test/col/run.sql': ('WS-84-2', 1)}, []),
        ('a pending file may not get another finding', {'test/col/run.sql': "'Custom Scan (SiftAgg)'\n'Custom Scan (SiftAgg)'\n"}, False, {'test/col/run.sql': ('WS-84-2', 1)},
         ['test/col/run.sql:1: an exact SiftAgg plan node', 'test/col/run.sql:2: an exact SiftAgg plan node']),
        ('strict reports a pending file', {'test/col/run.sql': "'Custom Scan (SiftAgg)'\n"}, True, {'test/col/run.sql': ('WS-84-2', 1)}, ['test/col/run.sql:1: an exact SiftAgg plan node']),
        ('a pending file that is clean is noted', {'test/col/run.sql': 'ok\n'}, False, {'test/col/run.sql': ('WS-84-2', 1)}, ['NOTE test/col/run.sql: clean']),
        ('a pending file that is gone is noted', {}, False, {'test/col/gone.sql': ('WS-84-2', 1)}, ['NOTE test/col/gone.sql: no such harness']),
    ]
    for name, over, strict, pend, want in cases:
        with tempfile.TemporaryDirectory(dir='/dev/shm' if os.path.isdir('/dev/shm') else None) as d:
            tree(d, **over)
            PENDING = pend
            errs, notes = run(d, strict)
            got = errs + ['NOTE ' + n for n in notes]
            ok = len(got) == len(want) and all(w in g for w, g in zip(want, got))
            if not ok:
                bad = 1
                print(f"FAIL: {name}: want {want}, got {got}")
    PENDING = saved
    # the tree this runs in: every PENDING row names a harness that exists and still has a finding of the count it names or fewer
    root = os.path.dirname(HERE)
    if os.path.isdir(os.path.join(root, 'test/sql')):
        for rel, (ws, n) in PENDING.items():
            if os.path.isfile(os.path.join(root, rel)) and len(findings(root, rel)) > n:
                bad = 1
                print(f"FAIL: PENDING {rel} allows {n} finding(s) of {ws} and the file has {len(findings(root, rel))}")
    if not bad:
        print(f"check-test-hygiene: selftest ok ({len(cases)} cases)")
    return bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--strict-harnesses', action='store_true', help='every finding of a harness is an error, PENDING or not')
    ap.add_argument('--root', default=os.path.dirname(HERE))
    ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    return report(a.root, a.strict_harnesses or not PENDING)


if __name__ == '__main__':
    sys.exit(main())
