#!/usr/bin/env python3
"""Moves the files of src/ (and tools/vk-v6/tests/) into one folder per context and drops the prefixes the folders say (PLAN-PREFIX-SUBFOLDER-CLEANUP).

  scripts/refactor-subfolders.py            print the moves (old<TAB>new), change nothing
  scripts/refactor-subfolders.py --apply    git index renames + the rewrite of every reference (#include, build files, lists, scripts, docs); no line of C changes
  scripts/refactor-subfolders.py --pure REV exit 1 unless every C file differs from its REV version in comments and #include lines only
  scripts/refactor-subfolders.py --check    exit 1 when a file still has a move to make or repeats its folder (what scripts/check-path-redundancy.py judges)

The moves follow rules, not a list of files: a file of a folder in CONTEXTS goes to the context whose prefix it has (longest prefix first), loses the prefix, and the rule
runs again in the new folder; the prefix of a format (f2_, v6_, v1_, format2_) and the name of a folder are dropped everywhere. A new file of a known context is moved by its
prefix. Collisions are an error: add the file to OVERRIDES. Run it on a tree with no open series that touches src/.
"""
import importlib.util
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('redundancy', os.path.join(ROOT, 'scripts/check-path-redundancy.py'))
red = importlib.util.module_from_spec(spec)
spec.loader.exec_module(red)

AREAS = ('core', 'bm25', 'col', 'agg', 'table')
GENERATED_SKIP = r'^(releases/|results/|docs/results/|docs/releases/|bench/xbench/|LICENSE$|scripts/(restructure-src|refactor-subfolders|check-path-redundancy)\.|sql/pg_sift--[0-9.-]+\.sql$|sql/[0-9.]+/)'
BUILD_FILES = re.compile(r'(^|/)(Makefile|CMakeLists\.txt|[\w-]+\.mk)$')

# folder (under src/) -> {context folder: None (prefix = its name) | {file prefix: name of the file in the folder ('' = nothing left of the prefix)}}
CONTEXTS = {
	'table/f2': {'col_delta': {'col_delta': ''}, 'lock': {'lock': 'lock', 'rowlock': 'rowlock'}, 'vm': None, 'row': {'row': ''}},
	'table/v1': {'gscan': None, 'vacuum': None},
	'core': {
		'maint': None, 'store': None, 'quarantine': None, 'sql': None, 'dirchain': None, 'visibility': None, 'freelist': None, 'space': None, 'live': None,
		'shm': {'shm': 'shm', 'shmbudget': 'budget'},
	},
	'core/f2': {'fsm': None, 'manifest': None},
	'core/v1_col': {'tomb': None},
	'col/f2': {
		'v6': None, 'scope': None, 'tier': None, 'store': None, 'l0': {'l0': ''}, 'proj': None, 'part': None, 'uniq': {'uniq': '', 'unique': 'unique'},
		'preagg': None, 'build': {'build': 'build', 'build_ext_gate': 'ext_gate'}, 'key': {'key': ''},
		'del': {'delete': 'delete', 'delvec': 'delvec', 'del': '', 'pend_del': 'pend'},
		'col_delta': {'col_delta': '', 'col_diff': 'diff', 'col_host': 'host'}, 'compact': None, 'pred': None, 'tid': {'tid': ''},
		'vacuum': {'vacuum': '', 'vac': ''}, 'drop': {'drop': ''}, 'gpu': {'gpu': ''}, 'kll': None, 'prefetch': None, 'rowbuf': None, 'gbuf': None,
		'sink': None, 'scan': None,
	},
	'col/f2/v6': {'proj': None, 'scope': None, 'unnest': None},
	'col': {
		'codec': None, 'scope': None, 'dictshm': None,
		'shred': {'shred': 'shred', 'shred_kern_avx2': 'avx2', 'shred_kern_avx512': 'avx512', 'shred_kern_neon': 'neon'},
		'like': {'like_simd': 'simd', 'like_simd_neon': 'neon', 'like_simd_x86': 'x86'},
	},
	'col/granule': {'cfilt': {'cfilt': ''}, 'part': {'part': ''}},
	'col/v1': {'kern': None, 'gorder': None, 'order': None, 'conn': None, 'batch': None, 'enc': None, 'track': None, 'scope': {'scope': ''}, 'scan': None,
		'preagg': None, 'seg': None},
	'agg': {'kern6': None, 'hybrid': {'hybrid': 'hybrid', 'hybrid_agg_core': 'core', 'hybrid_agg_exec': 'exec_agg'}, 'gpu': None, 'vexpr': None, 'vk': None},
	'agg/v6': {
		'group': None, 'plan': None, 'par': None, 'distinct': None, 'filter': {'filter': ''}, 'gen': None, 'star': None, 'argext': None, 'first': None,
		'fuse': None, 'osa': None, 'inset': None, 'unnest': None,
		'win': {'window': 'window', 'win': '', 'winx': 'x', 'datebin_win': 'datebin'},
		'join': {'join': 'join', 'asof': 'asof', 'grace_join': 'grace'},
		'cost': {'card': 'card', 'cost': 'cost'},
		'qual': {'qual': 'qual', 'bqual': 'bqual', 'pg_qual': 'pg', 'jsonb_qual': 'jsonb'},
		'topn': {'topn': 'topn', 'rows_topn': 'rows', 'topk_run': 'topk_run'},
		'expr': {'expr': '', 'pg_expr': 'pg'},
	},
	'bm25': {name: None for name in ('planner', 'bitmap', 'kernels', 'segment', 'build', 'query', 'cscan', 'dict', 'eval', 'rank', 'posting', 'merge', 'facet')}
		| {'analyze': {'analyze': 'analyze', 'analyze_tables': 'tables'}},
}

# folder -> the names, besides its own and its ancestors', that its files repeat
EXTRA_TOKENS = {'col/v1_engine': ('v1',), 'col/scope_v6': ('scope_v6',), 'col/scope_v6/test': ('scope_v6',)}
DIR_RENAMES = {'bm25/analyze_tables': 'bm25/analyze/tables'}
# the moves a rule cannot say: old path (from the repository root) -> new path
OVERRIDES = {
	'src/col/f2/format2_test.c': 'src/col/f2/format_test.c',
	'src/col/f2/f2test.h': 'src/col/f2/test.h',
	'src/col/skip/skip_index.h': 'src/col/skip/skip.h',
	'src/agg/v6/join_asof.c': 'src/agg/v6/join/asof_run.c',
	'src/agg/v6/v6_par_group.c': 'src/agg/v6/par/group_agg.c',
}


def split(base):
	return os.path.splitext(base)


def echo_names(d):
	comps = d.split('/')
	return set(comps[1:]) | set(EXTRA_TOKENS.get(d, ())) | {a for c in comps for a in red.FORMATS.get(c, ())}


def strip_echo(d, base):
	"""The name without what the folders repeat (the area is not repeated here: col_delta is a context of col/f2)."""
	comps = d.split('/')
	while True:
		stem, ext = split(base)
		hit = None
		for t in sorted(echo_names(d), key=len, reverse=True):
			if stem.startswith(t + '_') and len(stem) > len(t) + 1:
				hit = stem[len(t) + 1:]
				break
		if hit is None:
			for t in sorted(red.format_tokens(comps), key=len, reverse=True):
				if stem.endswith('_' + t) and len(stem) > len(t) + 1:
					hit = stem[:-len(t) - 1]
					break
		if hit is None:
			return base
		base = ('int' if hit == 'priv' else hit) + ext


def members(ctx, spec):
	return {ctx: ctx} if spec is None else spec


def match_context(d, stem):
	best = None
	for ctx, spec in CONTEXTS.get(d, {}).items():
		for src, dst in members(ctx, spec).items():
			if (stem == src or stem.startswith(src + '_')) and (best is None or len(src) > len(best[1])):
				best = (ctx, src, dst)
	return best


def resolve(d, base):
	"""(folder, name) a file called base in folder d ends in."""
	base = strip_echo(d, base)
	stem, ext = split(base)
	hit = match_context(d, stem)
	if hit:
		ctx, src, dst = hit
		rest = stem[len(src) + 1:]
		new = (dst or ctx) if not rest else (f'{dst}_{rest}' if dst else rest)
		return resolve(f'{d}/{ctx}', new + ext)
	area = d.split('/')[0]
	if stem.startswith(area + '_') and len(stem) > len(area) + 1:
		return resolve(d, stem[len(area) + 1:] + ext)
	return d, base


def new_path(old):
	if old in OVERRIDES:
		return OVERRIDES[old]
	if old.startswith('tools/vk-v6/tests/'):
		return 'tools/vk-v6/tests/' + strip_vk(os.path.basename(old))
	d, base = os.path.dirname(old)[4:], os.path.basename(old)
	for src, dst in DIR_RENAMES.items():
		if d == src or d.startswith(src + '/'):
			d = dst + d[len(src):]
	d, base = resolve(d, base)
	return f'src/{d}/{base}'


def strip_vk(base):
	return base[len('vk_v6_'):] if base.startswith('vk_v6_') else base


def git(*args, stdin=None):
	return subprocess.run(['git', *args], cwd=ROOT, check=True, capture_output=True, text=True, input=stdin).stdout


def tree_files():
	return [f for f in git('ls-files', '-z', '--', 'src', 'tools/vk-v6/tests').split('\0')
		if f and f.split('/')[1:2] and (f.startswith('tools/') or f.split('/')[1] in AREAS)]


def plan(files):
	moves = {f: new_path(f) for f in files}
	moves = {o: n for o, n in moves.items() if o != n}
	kept = set(files) - set(moves)
	seen = {}
	for o, n in sorted(moves.items()):
		if n in kept or n in seen:
			sys.exit(f'collision: {o} and {seen.get(n, n)} would both be {n}: add an entry to OVERRIDES')
		seen[n] = o
	return moves


def rewrite_text(moves, known):
	old_rel = {o[4:]: n[4:] for o, n in moves.items() if o.startswith('src/')}
	old_stem = {split(o)[0]: split(n)[0] for o, n in old_rel.items() if o.endswith('.c')}
	vk_base = {os.path.basename(o): os.path.basename(n) for o, n in moves.items() if o.startswith('tools/')}
	area = '|'.join(AREAS)
	path_re = re.compile(rf'(?<![\w.-])(src/)?((?:{area})/(?:[\w/+-]|\\?\.)*\w)')
	prefix_re = re.compile(r'(addprefix\s+\$\(\w+\)/((?:\w+/)*),)([^)]*)')

	stem_re = re.compile(rf'(?<![\w.-])((?:{area})/[\w/]*\w)')
	old_of = {n: o for o, n in moves.items()}
	changed = 0
	for f in git('ls-files', '-z').split('\0'):
		if not f or re.match(GENERATED_SKIP, f) or f == 'scripts/moved-sources.tsv':
			continue
		p = os.path.join(ROOT, f)
		try:
			text = open(p, encoding='utf-8', newline='').read()
		except (UnicodeDecodeError, IsADirectoryError, FileNotFoundError):
			continue
		head, tail = re.split(r'(?=\n## [0-9])', text, maxsplit=1) if f == 'CHANGELOG.md' else (text, '')
		to_new = lambda s: path_re.sub(lambda m: (m.group(1) or '') + moved(old_rel, m.group(2)), s)
		new = rewrite_c(head, to_new, lambda n: relative_include(old_of.get(f, f), f, n, moves, known)) if f.endswith(('.c', '.h')) else to_new(head)
		if BUILD_FILES.search(f):
			new = prefix_re.sub(lambda m: m.group(1) + ' '.join(moved_in(old_rel, m.group(2), w) for w in m.group(3).split(' ')), new)
		if BUILD_FILES.search(f):
			new = stem_re.sub(lambda m: old_stem.get(m.group(1), m.group(1)), new)
		if f.startswith('tools/vk-v6/'):
			new = re.sub(r'\bvk_v6_\w+\.[ch]\b', lambda m: vk_base.get(m.group(0), m.group(0)), new)
			new = re.sub(r'(?<=tests/)(vk_v6_\w+)\.o\b', lambda m: os.path.splitext(vk_base.get(m.group(1) + '.c', m.group(1) + '.c'))[0] + '.o', new)
		new += tail
		if new != text:
			open(p, 'w', encoding='utf-8', newline='').write(new)
			changed += 1
	return changed


C_TOKEN = re.compile(r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'", re.S)
PATH_STRING = re.compile(r'"(src/)?[\w./+-]+\.[ch]"')
INCLUDE_LINE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+$')


def rewrite_c(text, to_new, include_name):
	"""A C file changes in its #include "paths", its comments and the strings that are a file path, never in code or in other text (the object code stays what it was)."""
	def token(m):
		s = m.group(0)
		if s[0] in '/':
			return to_new(s)
		line_start = text.rfind('\n', 0, m.start()) + 1
		if s[0] == '"' and INCLUDE_LINE.match(text[line_start:m.start()]):
			name = s[1:-1]
			return '"' + (include_name(name) if '/' not in name else to_new(name)) + '"'
		return to_new(s) if s[0] == '"' and PATH_STRING.fullmatch(s) else s
	return C_TOKEN.sub(token, text)


def moved(old_rel, rel):
	new = old_rel.get(rel.replace('\\.', '.'))
	return rel if new is None else new.replace('.', '\\.') if '\\.' in rel else new


def moved_in(old_rel, folder, word):
	new = old_rel.get(folder + word)
	return word if new is None or not new.startswith(folder) else new[len(folder):]


def relative_include(old_includer, new_includer, name, moves, known):
	target = os.path.join(os.path.dirname(old_includer), name)
	if target not in known:
		return name
	nt = moves.get(target, target)
	if os.path.dirname(nt) == os.path.dirname(new_includer) and os.path.basename(nt) == name:
		return name
	return nt[4:] if nt.startswith('src/') else os.path.relpath(nt, os.path.dirname(new_includer))


def refusal_scan(rel, no_dirs):
	"""Whether scripts/gen-refusals.sh read src/agg/rel before the moves."""
	folder, _, name = rel.rpartition('/')
	return name.endswith('.c') and (rel.startswith(('plan', 'spec')) if not folder else '/' not in folder and folder not in no_dirs)


def rekey_refusals(files, moves):
	"""scripts/refusals.allow names a file by its name: a name is no longer unique once the prefixes are gone, so a row names src/agg/ path of the file."""
	path = os.path.join(ROOT, 'scripts/refusals.allow')
	no_dirs = set(re.findall(r"'(\w+)'", re.search(r'NO_REFUSAL_DIRS = \{([^}]*)\}', open(os.path.join(ROOT, 'scripts/gen-refusals.sh')).read()).group(1)))
	owners = {}
	for o in files:
		if o.startswith('src/agg/') and refusal_scan(o[8:], no_dirs):
			owners.setdefault(os.path.basename(o), []).append(moves.get(o, o)[8:])
	lines = []
	for line in open(path).read().split('\n'):
		m = re.match(r'^([\w.]+\.c): ([\w ]+?)( # .*)$', line)
		if not m or m.group(1) not in owners:
			lines.append(line)
			continue
		by_file = {}
		for fn in m.group(2).split():
			home = [r for r in owners[m.group(1)] if re.search(rf'^{fn}\(', open(os.path.join(ROOT, 'src/agg', r)).read(), re.M)]
			for r in home[:1] or owners[m.group(1)][:1]:
				if not r.startswith('v6/par/'):
					by_file.setdefault(r, []).append(fn)
		lines += [f'{r}: {" ".join(fns)}{m.group(3)}' for r, fns in by_file.items()]
	open(path, 'w').write('\n'.join(lines))


def move_files(moves):
	for o, n in moves.items():
		os.makedirs(os.path.dirname(os.path.join(ROOT, n)), exist_ok=True)
		os.rename(os.path.join(ROOT, o), os.path.join(ROOT, n))
	git('rm', '-q', '--cached', '--pathspec-from-file=-', '--pathspec-file-nul', stdin='\0'.join(moves) + '\0')
	git('add', '--pathspec-from-file=-', '--pathspec-file-nul', stdin='\0'.join(moves.values()) + '\0')


def record_moves(moves):
	with open(os.path.join(ROOT, 'scripts/moved-sources.tsv'), 'a') as f:
		f.writelines(f'{n}\t{o}\t\n' for o, n in sorted(moves.items()) if o.startswith('src/') and o.endswith(('.c', '.h')))


def edit(path, old, new, count=1):
	p = os.path.join(ROOT, path)
	text = open(p, newline='').read()
	if new in text:
		return
	if text.count(old) == count or (count is None and old in text):
		open(p, 'w', newline='').write(text.replace(old, new))
	else:
		sys.exit(f'{path}: the text to edit is not there:\n{old}')


# the build and check files that name a place by a glob or a pattern the moves change
def edit_globs():
	edit('Makefile', '$(SRC)/*/*/*/*.c))', '$(SRC)/*/*/*/*.c $(SRC)/*/*/*/*/*.c))')
	edit('Makefile', 'src/*/*/*/*.d .build-flavor', 'src/*/*/*/*.d src/*/*/*/*/*.d .build-flavor')
	edit('scripts/port-isa-check.sh', '*_avx2.o|*_avx512.o)', '*_avx2.o|*_avx512.o|*/avx2.o|*/avx2_*.o|*/avx512.o|*/avx512_*.o)')
	edit('scripts/isa-scan.sh', '*_avx2.o | *_avx512.o | *_popcnt.o)', '*_avx2.o | *_avx512.o | *_popcnt.o | */avx2.o | */avx2_*.o | */avx512.o | */avx512_*.o | */popcnt.o)')
	edit('scripts/lint.sh', 'src/*/*_neon.c src/core/visibility/popcnt.c)', '$(git ls-files "src/*/neon.c" "src/*_neon.c") src/core/visibility/popcnt.c)')
	edit('sqlite/Makefile', '$(B)/*.o $(B)/*/*.o $(B)/*/*/*.o\n', "$$(find $(B) -name '*.o')\n")
	edit('sqlite/Makefile', '-include $(wildcard $(B)/*.d $(B)/*/*.d $(B)/*/*/*.d)', '-include $(wildcard $(B)/*.d $(B)/*/*.d $(B)/*/*/*.d $(B)/*/*/*/*.d)')
	edit('sqlite/Makefile', '%_avx2 %_avx512,', '%_avx2 %_avx512 %/avx2 %/avx2_% %/avx512 %/avx512_%,')
	edit('mariadb/Makefile', '$(OUT)/*.o $(OUT)/*/*.o $(OUT)/*/*/*.o\n', "$$(find $(OUT) -name '*.o')\n")
	edit('mariadb/Makefile', '-include $(wildcard $(OUT)/*.d $(OUT)/*/*.d $(OUT)/*/*/*.d)', '-include $(wildcard $(OUT)/*.d $(OUT)/*/*.d $(OUT)/*/*/*.d $(OUT)/*/*/*/*.d)')


TEST_FILE = r'(^|/)([^/]*_)?test(_[^/]*)?(\.[ch]|/)'


def edit_test_predicates():
	edit('scripts/check-capacity.sh', 'SKIP = re.compile(r"_test|/test/|kern|_avx|_neon|/vk")', 'SKIP = re.compile(r"[_/]test|kern|[_/]avx|[_/]neon|/vk")')
	edit('scripts/check-mem-growth.sh', 'SKIP = ("_test", "/test/", "leak_test")', 'SKIP = ("_test", "/test")')
	edit('scripts/check-null.sh', "path.endswith('_test.c') or '/test/' in path", "re.search(r'[_/]test(_[^/]*)?\\.c$|/test/', path)")
	edit('scripts/check-complexity.sh', "SKIP_FILE = re.compile(r'_test\\.c$|/test/|_spv\\.h$')", "SKIP_FILE = re.compile(r'[_/]test\\.c$|/test/|/spv\\.h$')")
	edit('scripts/check-sites.py', "not f.endswith('_test.c')]", "not f.endswith(('_test.c', '/test.c'))]")
	edit('scripts/check-sites.py', "not r[0].endswith('_test.c'):", "not r[0].endswith(('_test.c', '/test.c')):")
	for f in ('check-polling.sh', 'check-raw-alloc.sh', 'check-threads.sh'):
		edit('scripts/' + f, "':!src/*/*_test.c' ':!src/*/test/*'", "':!src/*/*_test.c' ':!src/*/test.c' ':!src/*/test_*' ':!src/*/test/*'")
	edit('scripts/gen-messages/emitters.py', "SKIP_C = re.compile(r'(^|/)(test|[^/]*_test[^/]*)(\\.[ch]|/)')", "SKIP_C = re.compile(r'%s')" % TEST_FILE)
	edit('scripts/check-relcache-slots.sh', 'mkdir -p "$d/tree/col" "$d/tree/core"', 'mkdir -p "$d/tree/col" "$d/tree/core/store"')
	edit('scripts/check-file-size.sh', "GENERATED='^src/agg/vk_(spv|min)[.]h$'", "GENERATED='^src/agg/vk/(spv|min)[.]h$'")
	edit('scripts/areas.tsv', 'restructure-src|gen-limits', 'restructure-src|check-path-redundancy|refactor-subfolders|gen-limits')
	for old, new in (('planner_path', 'planner/path'), ('cscan_exec', 'cscan/exec'), ('rank_search', 'rank/search')):
		edit('scripts/areas.tsv', old, new, None)
	for old, new in (('src/core/(guc|sql_)', 'src/core/(guc|sql/)'),
	                 ('v6/(v6_(star|sip|join|grace_join|inset)|join)', 'v6/(v6_(star|sip|join|grace_join|inset)|star/|sip\\.|inset/|join)'),
	                 ('|distinct|text|datebin)|group|window|scan)', '|distinct|text|datebin)|distinct/|stream\\.|text\\.|topn/|win/|group|window|scan)'),
	                 ('|^src/col/like_simd', '|^src/col/like/'),
	                 ('v6/(plan|v6_plan|v6_cost|v6_card_cost)', 'v6/(plan|v6_plan|v6_cost|v6_card_cost|cost/)'),
	                 ('agg/v6/(v6_unnest|jsonb_qual)', 'agg/v6/(v6_unnest|unnest/|qual/jsonb)'),
	                 ('table/f2/f2_(dml|lock)', 'table/f2/(f2_(dml|lock)|dml\\.|lock/)'),
	                 ('v6/v6_gpu', 'v6/(v6_)?gpu'),
	                 ('v6/(v6_par|v6_gather)', 'v6/(v6_par|v6_gather|par/|gather\\.)'),
	                 ('col/f2/f2_par\\.', 'col/f2/(f2_)?par\\.')):
		edit('scripts/areas.tsv', old, new)


def edit_context_scans():
	edit('scripts/check-paging.sh', r'FILENAME !~ /core\/live\.c$/', r'FILENAME !~ /core\/live\/live\.c$/')
	edit('scripts/gen-refusals.sh', "[f for d in folders for f in glob.glob(d + '*.c')]", "[f for d in folders for f in glob.glob(d + '**/*.c', recursive=True) if '/v6/par/' not in f]")
	edit('scripts/gen-refusals.sh', 'name = os.path.basename(path)', "name = path[len('src/agg/'):]")
	edit('scripts/gen-refusals.sh', "'spill', 'vk'}", "'spill', 'vk', 'kern6', 'hybrid', 'gpu', 'vexpr'}")


def sort_focus_rows():
	for name in os.listdir(os.path.join(ROOT, 'scripts/focus')):
		p = os.path.join(ROOT, 'scripts/focus', name)
		rows = [row.partition('\t') for row in open(p).read().split('\n') if row]
		open(p, 'w').write(''.join(f'{k}\t{" ".join(sorted(set(v.split())))}\n' for k, _, v in rows))


def edit_layout_check():
	edit('scripts/restructure-src.sh', '(?:/[A-Za-z0-9_.]+)?)}g', '(?:/[A-Za-z0-9_.]+){0,4})}g')
	p = os.path.join(ROOT, 'scripts/restructure-src.sh')
	text = open(p).read()
	start, end = text.index('\t\tif [[ $dir != */* ||'), text.index('\t\tif [[ " $RENAMED " == *" $area "* &&')
	cond = ('\t\tif [[ $dir != */* || $name == */*/*/*/* ]] || [[ " $AREAS " != *" $area "* ]]; then echo "$f: not in a folder of src/ ($AREAS), or deeper than '
		'area/d1/d2/d3/ (four folders)"; rc=1; continue; fi\n')
	open(p, 'w').write(text[:start] + cond + text[end:])
	edit('scripts/lint.sh', "\t'layout\t\tscripts/restructure-src.sh --check'\n",
		"\t'layout\t\tscripts/restructure-src.sh --check'\n\t'path-redundancy\tpython3 -I scripts/check-path-redundancy.py --selftest && python3 -I scripts/check-path-redundancy.py'\n")
	edit('scripts/restructure-src.sh', r'scripts/restructure-src\.sh$|', r'scripts/(restructure-src|refactor-subfolders|check-path-redundancy)\.|')


def apply():
	files = tree_files()
	moves = plan(files)
	if moves:
		move_files(moves)
		print(f'moved {len(moves)} files; rewrote {rewrite_text(moves, set(files))} files')
		record_moves(moves)
		rekey_refusals(files, moves)
	edit_globs()
	edit_test_predicates()
	edit_context_scans()
	sort_focus_rows()
	edit_layout_check()


def bare(text):
	"""The C of a file without its comments and #include lines."""
	text = C_TOKEN.sub(lambda m: '' if m.group(0)[0] == '/' or PATH_STRING.fullmatch(m.group(0)) else m.group(0), text)
	return re.sub(r'^[ \t]*#[ \t]*include[^\n]*\n', '', text, flags=re.M)


def pure(rev):
	"""Exit 1 unless every C file differs from its REV version in comments and #include lines only."""
	pairs = [l.split('\t') for l in git('diff', '-M', '--name-status', rev, '--', '*.c', '*.h').split('\n') if l]
	changed = [(p[-2] if p[0][0] == 'R' else p[-1], p[-1]) for p in pairs if p[0][0] in 'RM']
	bad = [n for o, n in changed if bare(git('show', f'{rev}:{o}')) != bare(open(os.path.join(ROOT, n)).read())]
	print('\n'.join(bad))
	print(f'refactor-subfolders: {len(changed)} C files compared, {len(bad)} differ in more than comments and #include lines')
	sys.exit(1 if bad else 0)


def check():
	files = tree_files()
	left = plan(files)
	found = [f'{o} -> {n}' for o, n in sorted(left.items())] + list(red.violations(files))
	print('\n'.join(found))
	print(f'refactor-subfolders: {len(found)} violation(s)', file=sys.stderr if found else sys.stdout)
	sys.exit(1 if found else 0)


if __name__ == '__main__':
	mode = sys.argv[1] if len(sys.argv) > 1 else ''
	if mode == '--apply':
		apply()
	elif mode == '--check':
		check()
	elif mode == '--pure':
		pure(sys.argv[2])
	else:
		print('\n'.join(f'{o}\t{n}' for o, n in sorted(plan(tree_files()).items())))
