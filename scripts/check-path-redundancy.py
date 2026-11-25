#!/usr/bin/env python3
"""A tracked file does not repeat in its name what the folders it is in already say.

  src/agg/v6/v6_star.c           -> star.c           an ancestor (or its synonym: tbl/table, f2/format2, vk/gpu) as a prefix
  test/unit/f2_scaling_test.c    -> f2_scaling.c     ... or as a suffix
  test/mirror/mirror.sql         -> mirror.sql       the whole name is an ancestor (main, core, api, run, basic or common say more)
  test/sql/col/p98_decoders.sql  -> decoders.sql     tests are named by subsystem and behavior, never by a milestone (p98_, s55_, m4_, prod95_, on_)
  test/sql/col/f2/smoke.sql      a test of test/{sql,expected,t,isolation,unit} is in the folder of its src/ area (core bm25 col agg table compat fault maint mem quarantine lib mk)

A regression test is at most 52 bytes with its folders (test/sql/NAME.sql): pg_regress names its session pg_regress/NAME, and the NOTICE of a name PostgreSQL cuts at 63 is in the output.

Fix the name, or move the file to the folder of its context: there is no allow list. A folder is never called or, in, is, on, to, by or of.
The exact names of the contract (Makefile, README.md, the control files, SiftTest.pm...), test/fixtures/compat*/ and the alternate outputs (NAME_1.out) are not names to judge.

  scripts/check-path-redundancy.py [PATH...]            the findings (exit 1 when there are any) of src/ ports/ sqlite/ mariadb/ test/ scripts/ bench/ tools/, or of the PATHs
  scripts/check-path-redundancy.py --report [PATH...]   the count of findings by root and rule, exit 0 (the debt that is left)
  scripts/check-path-redundancy.py --spec93 [PATH...]   only the gate of the folders of src/ and tools/vk-v6/tests/: no ancestor prefix, no format suffix (_f2 _v6 _v1), no priv.h; x/foo/foo.c stays
  scripts/check-path-redundancy.py --selftest
"""
import os
import re
import subprocess
import sys

ROOTS = ('src/', 'ports/', 'sqlite/', 'mariadb/', 'test/', 'scripts/', 'bench/', 'tools/')
SPEC93_ROOTS = ('src/', 'tools/vk-v6/tests/')
RULES = ('prefix', 'suffix', 'exact-stem', 'priv', 'milestone', 'area', 'folder', 'length')
MAX_TEST_NAME = 52
CONTAINERS = {'src', 'ports', 'sql', 'expected', 'specs', 't', 'c', 'regress'}
CONTRACT = {
	'Makefile', 'CMakeLists.txt', 'README.md', 'CHANGELOG.md', 'DESIGN.md', 'VERSION', '.gitignore', '__init__.py', 'pg_sift.control', 'pg_sift_test.control',
	'pg_sift_helpers.control', 'ubsan.supp', 'cases.mk', 'areas.mk', 'SiftTest.pm', 'SiftCompat.pm',
}
AREAS = {'core', 'bm25', 'col', 'agg', 'table', 'compat', 'fault', 'maint', 'mem', 'quarantine', 'lib', 'mk'}
AREA_DIRS = ('test/sql/', 'test/expected/', 'test/t/', 'test/isolation/specs/', 'test/isolation/expected/', 'test/unit/')
ALTERNATE_DIRS = ('test/expected/', 'test/isolation/expected/')
MILESTONE_DIRS = ('test/', 'scripts/focus/')
MILESTONE = re.compile(r'(p\d+|s\d+|m\d+|prod\d+|on\d*)_')
FUNCTION_WORDS = {'or', 'in', 'is', 'on', 'to', 'by', 'of'}
FORMATS = {'f2': ('f2', 'format2'), 'v6': ('v6',), 'v1': ('v1',)}
SYNONYMS = [
	{'table', 'tbl'}, {'col', 'column'}, {'agg', 'aggregate'}, {'helper', 'helpers'}, {'test', 'tests'}, {'fuzz', 'fz'}, {'bench', 'benchmark'}, {'spec', 'specs'},
	{'stat', 'stats'}, {'par', 'parallel'}, {'vk', 'vulkan', 'gpu'}, {'f2', 'format2'}, {'check', 'checks'}, {'gen', 'generate'}, {'plan', 'plans', 'planner'},
	{'cost', 'costs'}, {'scan', 'scans'}, {'exec', 'executor'}, {'cache', 'caches'}, {'scope', 'scoped', 'scopes'}, {'maint', 'maintenance'},
	{'proj', 'projection', 'projections'}, {'codec', 'colcodec'}, {'seg', 'segment', 'segments', 'segfmt'}, {'tomb', 'tomblog'}, {'group', 'groups', 'grouped'},
	{'join', 'joins'}, {'leaf', 'leaves'}, {'expr', 'exprs'}, {'order', 'ordered'}, {'window', 'windows'}, {'store', 'stor', 'storage'}, {'read', 'reader', 'readers'},
	{'write', 'writer', 'writers'}, {'roles', 'role'}, {'rank', 'ranked'}, {'index', 'idx'}, {'fixture', 'fixtures'}, {'case', 'cases'}, {'fam', 'family', 'families'},
	{'mem', 'memory'}, {'lock', 'locks'}, {'pack', 'packed'}, {'key', 'keys'}, {'key_cut', 'keycut'}, {'null', 'nulls'}, {'part', 'parts', 'partition', 'partitions'},
	{'trap', 'traps'}, {'tid', 'ctid'}, {'stub', 'aggstub'}, {'point', 'points'}, {'analyze', 'analyzer'}, {'worker', 'workers'}, {'child', 'children'},
	{'sum', 'sums'}, {'array', 'arrays'}, {'range', 'ranges'}, {'param', 'params'}, {'kern', 'kernel', 'kernels'}, {'posting', 'postings'}, {'facet', 'facets'},
	{'query', 'queries'}, {'page', 'pages'}, {'vis', 'visibility'}, {'tier', 'tiering'}, {'shader', 'shaders'}, {'step', 'steps'}, {'script', 'scripts'}, {'tool', 'tools'},
]
SYNONYM_OF = {}
for group in SYNONYMS:
	for word in group:
		SYNONYM_OF.setdefault(word, set()).update(group)


def split_name(base):
	return os.path.splitext(base)


def echo_tokens(comps):
	return {c.replace('-', '_') for c in comps} | {a for c in comps for a in FORMATS.get(c, ())}


def format_tokens(comps):
	return {a for c in comps for a in FORMATS.get(c, ())}


def strip_one(comps, base):
	"""(rule, the name without one folder-named prefix or format suffix), or None: the gate of Spec 93."""
	stem, ext = split_name(base)
	for t in sorted(echo_tokens(comps), key=len, reverse=True):
		if stem.startswith(t + '_') and len(stem) > len(t) + 1:
			return 'prefix', stem[len(t) + 1:] + ext
	for t in sorted(format_tokens(comps), key=len, reverse=True):
		if stem.endswith('_' + t) and len(stem) > len(t) + 1:
			return 'suffix', stem[:-len(t) - 1] + ext
	return None


def spec93_findings(path):
	root = next((r for r in SPEC93_ROOTS if path.startswith(r)), None)
	if root is None:
		return
	comps = path[len(root):].split('/')[:-1] if root == 'src/' else ['vk-v6']
	if not comps:
		return
	base = os.path.basename(path)
	fix = strip_one(comps, base)
	if fix:
		yield fix[0], f'repeats its folders; call it {fix[1]} (or put it in the folder of its context)'
	elif base == 'priv.h' or base.endswith('_priv.h'):
		yield 'priv', 'the private header of a context folder is int.h'


def ancestors(comps):
	"""token -> the folder that carries it: the folder, its words, the pair with the next folder, and the synonyms of all of them"""
	norm = [c.lower().replace('-', '_') for c in comps]
	owner = {}
	for i, c in enumerate(norm):
		pair = [f'{c}_{norm[i + 1]}'] if i + 1 < len(norm) else []
		for t in [c, *c.split('_'), *pair]:
			owner.setdefault(t, comps[i])
	for t, c in list(owner.items()):
		for s in SYNONYM_OF.get(t, ()):
			owner.setdefault(s, c)
	return owner


def is_exempt(path):
	base = os.path.basename(path)
	return base in CONTRACT or re.match(r'test/fixtures/compat[^/]*/', path) or re.match(r'sql/pg_sift(_test|_helpers)?--', path)


def name_findings(path):
	dirs = path.split('/')[:-1]
	comps = [d for d in dirs if d.lower() not in CONTAINERS]
	stem, ext = split_name(os.path.basename(path))
	if ext == '.out' and path.startswith(ALTERNATE_DIRS):
		stem = re.sub(r'_[0-9]+$', '', stem)
	toks = [t for t in re.split(r'[_-]', stem.lower()) if t]
	owner = ancestors(comps)
	if not toks:
		return
	if '_'.join(toks) in owner:
		yield 'exact-stem', f"the name is the folder '{owner['_'.join(toks)]}' (call it main, core, api, run, basic or common)"
		return
	for rule, cuts in (('prefix', [(toks[:1], toks[1:]), (toks[:2], toks[2:])]), ('suffix', [(toks[-1:], toks[:-1]), (toks[-2:], toks[:-2])])):
		for carried, rest in cuts:
			if rest and '_'.join(carried) in owner:
				yield rule, f"ancestor '{owner['_'.join(carried)]}' already carries '{'_'.join(carried)}'; call it {'_'.join(rest)}{ext}"
				break


def milestone_findings(path):
	names = path.split('/')[:-1] + [split_name(os.path.basename(path))[0]]
	for n in names[:-1]:
		if MILESTONE.match(n.lower() + '_'):
			yield 'milestone', f"the folder '{n}' is a milestone: name it by subsystem and behavior"
	if MILESTONE.match(names[-1].lower()):
		yield 'milestone', 'a test is named by subsystem and behavior, not by the milestone that added it'


def area_findings(path):
	prefix = next((d for d in AREA_DIRS if path.startswith(d)), None)
	if prefix is None:
		return
	rest = path[len(prefix):]
	if '/' not in rest:
		yield 'area', f'is flat in {prefix}: put it in the folder of its src/ area ({" ".join(sorted(AREAS))})'
	elif rest.split('/')[0] not in AREAS:
		yield 'area', f"'{rest.split('/')[0]}' is no src/ area of {prefix} ({' '.join(sorted(AREAS))})"


def length_findings(path):
	name = path[len('test/sql/'):-len('.sql')]
	if path.startswith('test/sql/') and path.endswith('.sql') and len(name) > MAX_TEST_NAME:
		yield 'length', f'{len(name)} bytes with its folders, at most {MAX_TEST_NAME} (pg_regress names its session pg_regress/NAME, PostgreSQL cuts it at 63 bytes and prints a NOTICE)'


def folder_findings(path):
	for d in path.split('/')[:-1]:
		if d.lower() in FUNCTION_WORDS:
			yield 'folder', f"the folder '{d}' is a function word: name it by what it holds"


def findings(path, spec93=False):
	"""(rule, why) for each way the path repeats its folders"""
	if is_exempt(path):
		return
	if spec93:
		yield from spec93_findings(path)
		return
	legacy = list(spec93_findings(path)) if path.startswith(SPEC93_ROOTS) else []
	yield from legacy
	if path.startswith(MILESTONE_DIRS):
		yield from milestone_findings(path)
	if path.startswith('test/'):
		yield from area_findings(path)
		yield from length_findings(path)
	yield from folder_findings(path)
	yield from (f for f in name_findings(path) if f[0] not in {rule for rule, _ in legacy})


def tracked(paths):
	out = subprocess.run(['git', 'ls-files', '-z', '--', *paths], check=True, capture_output=True, text=True).stdout
	return [p for p in out.split('\0') if p]


def violations(paths, spec93=False):
	for p in paths:
		for rule, why in findings(p, spec93):
			yield p, rule, why


def report(found, files, roots):
	rows = {r: {rule: 0 for rule in RULES} for r in roots}
	for p, rule, _ in found:
		rows[next(r for r in roots if p.startswith(r))][rule] += 1
	print('%-12s %s' % ('root', ' '.join('%10s' % rule for rule in RULES)))
	for r, counts in rows.items():
		print('%-12s %s' % (r, ' '.join('%10d' % counts[rule] for rule in RULES)))
	flagged = len({p for p, _, _ in found})
	print(f'path-redundancy: report: {len(found)} finding(s) in {flagged} of {len(files)} file(s)')


def rules_of(path, spec93=False):
	return {rule for rule, _ in findings(path, spec93)}


def selftest():
	spec93_bad = [
		'src/col/f2/f2_scan.c', 'src/col/f2/track_f2.c', 'src/bm25/bm25_doc.c', 'src/agg/vk/shaders/vk_topn.comp',
		'src/core/maint/maint_cut.c', 'src/bm25/runs/runs_priv.h', 'tools/vk-v6/tests/vk_v6_gen.c', 'src/col/f2/format2_test.c',
	]
	spec93_ok = [
		'src/col/f2/f2.h', 'src/col/f2/scope/scope.c', 'src/col/f2/agg/proj/proj.c', 'src/agg/vk/int.h', 'src/col/f2/col_delta/idx.c',
		'src/table/f2/lock/int.h', 'tools/vk-v6/tests/gen.c', 'src/col/f2/format.c', 'test/sql/col/f2_x.sql', 'src/col/v1/test/alloc_test.c',
	]
	for p in spec93_bad:
		assert rules_of(p, True), f'--spec93 passes {p}'
		assert rules_of(p), f'a file the gate of Spec 93 refuses passes: {p}'
	for p in spec93_ok:
		assert not rules_of(p, True), f'--spec93 refuses {p}: {rules_of(p, True)}'
	bad = {
		'prefix': ['src/agg/v6/v6_star.c', 'test/sql/col/topn/col_topn_arith.sql', 'tools/vk-v6/tests/vk_v6_cost.c', 'scripts/build/build-id.sh', 'bench/gpu/bench-pg.sh',
			'src/agg/gpu/vk_min.h', 'test/sql/table/tbl_bigtx.sql', 'test/sql/col/f2/format2_prune.sql', 'src/col/key_cut/keycut_x.c', 'test/t/col/scope/pairs/scope_pairs_x.sh'],
		'suffix': ['test/unit/f2_scaling_test.c', 'src/agg/v6/topn/rows_topn.c', 'test/helpers/roles_helpers.sql', 'scripts/build/ci-build.sh', 'src/col/f2/track_f2.c',
			'test/sql/table/bigtx_tbl.sql', 'sqlite/tests/hash_test.c'],
		'exact-stem': ['src/agg/plan/plan.c', 'test/mirror/mirror.sql', 'tools/tools.mk', 'src/col/f2/f2.h', 'bench/churn/churn.sh', 'test/stress/stress.sql', 'src/agg/plan/planner.c'],
		'priv': ['src/bm25/runs/priv.h', 'src/bm25/wbuf/page_priv.h'],
		'milestone': ['test/sql/col/p98_decoders.sql', 'test/sql/agg/prod95_query_stream_join.sql', 'test/sql/core/m4_cache_budget.sql', 'test/t/table/s55_granule.sh',
			'test/sql/agg/on_scaling_guards.sql', 'test/expected/col/s55_point_1.out', 'scripts/focus/col/p98_decoders.tsv', 'test/sql/p98/x.sql', 'test/sql/agg/on63_x.sql', 'test/t/table/f2/s3_mock.py'],
		'area': ['test/sql/foo.sql', 'test/expected/foo.out', 'test/t/helpers.bash', 'test/sql/roles/ddl.sql', 'test/expected/zzz/x.out', 'test/isolation/specs/foo.spec',
			'test/isolation/expected/hybrid/x.out', 'test/unit/unit.h', 'test/unit/helper/x.c'],
		'folder': ['test/sql/col/or/x.sql', 'src/col/in/x.c', 'test/t/agg/on/x.sh'],
		'length': ['test/sql/agg/v6/' + 'x' * 46 + '.sql', 'test/sql/' + 'x' * 53 + '.sql'],
	}
	for rule, paths in bad.items():
		for p in paths:
			assert rule in rules_of(p), f'{p} is not refused as {rule}: {rules_of(p)}'
	ok = [
		'test/expected/agg/v6/star_1.out', 'test/expected/agg/v6/star_12.out', 'test/isolation/expected/col/f2/compact_snapshot_2.out', 'test/col/fixtures/topn/disj/search_not_or.sql',
		'test/sql/agg/v6/star_plan.sql', 'test/sql/core/sql_security.sql', 'test/sql/col/f2/smoke.sql', 'test/t/col/f2/wal.pl', 'test/t/lib/common.bash', 'test/t/lib/SiftTest.pm',
		'test/t/fault/matrix.tsv', 'test/unit/mk/f2.mk', 'test/unit/Makefile', 'test/unit/core/par/arena.c', 'test/isolation/emulate.sql', 'test/isolation/specs/col/f2/lock.spec',
		'test/sql/agg/v6/' + 'x' * 45 + '.sql', 'test/t/agg/v6/' + 'x' * 60 + '.sh', 'test/README.md', 'test/fixtures/compat30/pg_sift_test.out', 'test/fixtures/compat2x/foo_compat2x/foo_compat.dat', 'Makefile',
		'sql/pg_sift--5.1--6.0.sql', 'sql/pg_sift_helpers--1.0.sql', 'src/col/f2/col_delta/idx.c', 'src/agg/vk/int.h', 'tools/vk-v6/tests/gen.c', 'src/col/f2/format.c',
		'src/col/v1/test/alloc.c', 'scripts/check-path-redundancy.py', 'scripts/focus/agg/v6/star.tsv', 'scripts/focus/_hot.tsv',
		'scripts/ci/jobs.sh', 'bench/harness/scaling.sh', 'sqlite/tests/hash.c', 'src/col/granule/point.c', 'test/sql/bm25/reference.sql', 'test/fuzz/diff/fam/basic.py',
	]
	for p in ok:
		assert not rules_of(p), f'{p} is refused: {rules_of(p)}'
	found = list(violations(['test/sql/p98_x.sql', 'test/sql/agg/v6/star.sql', 'src/agg/v6/v6_star.c']))
	assert {(p, r) for p, r, _ in found} >= {('test/sql/p98_x.sql', 'milestone'), ('test/sql/p98_x.sql', 'area'), ('src/agg/v6/v6_star.c', 'prefix')}, found
	assert not [f for f in found if f[0] == 'test/sql/agg/v6/star.sql'], found
	print('check-path-redundancy: selftest ok')


def main(args):
	if args == ['--selftest']:
		return selftest()
	spec93 = '--spec93' in args
	as_report = '--report' in args
	paths = [a for a in args if not a.startswith('--')]
	unknown = [a for a in args if a.startswith('--') and a not in ('--spec93', '--report')]
	if unknown:
		sys.exit(f'check-path-redundancy: unknown option {unknown[0]}\n' + __doc__)
	roots = SPEC93_ROOTS if spec93 else ROOTS
	files = [p for p in tracked(paths or list(roots)) if p.startswith(roots)]
	found = list(violations(files, spec93))
	if as_report:
		return report(found, files, roots)
	for p, rule, why in found:
		print(f'{p}: {rule}: {why}')
	if found:
		print(f'check-path-redundancy: {len(found)} finding(s) in {len({p for p, _, _ in found})} file(s) repeat the folders they are in', file=sys.stderr)
		sys.exit(1)


if __name__ == '__main__':
	main(sys.argv[1:])
