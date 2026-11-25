#!/usr/bin/env python3
"""Spec 100 WS-100-4: the one-shot, deterministic and idempotent move of test/ and scripts/focus/ to the src/-mirrored layout.

  --plan [--out FILE] [--summary FILE]   the old->new table of every tracked file, checked: no collision, no unmapped file, no milestone
                                         prefix, no ancestor prefix/suffix/synonym/exact-stem, every test under one of the AREAS
  --apply-moves                          git mv of the table (and removal of the lib*.bash forwarders that test/t/lib/ replaces)
  --rewrite-refs                         every reference to a moved path or test name: Makefile, scripts/, docs/, .github/, test/
  --verify                               0 collisions, 0 dangling references, 0 redundancy findings (needs the moves and rewrites done)
Delete this script with WS-100-5 once the move has landed.
"""
import argparse, collections, contextlib, importlib.util, os, posixpath, re, subprocess, sys

REPO = subprocess.check_output(['git', 'rev-parse', '--show-toplevel'], text=True).strip()
os.chdir(REPO)
MILESTONE = re.compile(r'^(p\d+|s\d+|m\d+|prod\d+|on\d*)$')
SCOPE = ('test/', 'scripts/focus/')
def load_linter():
    spec = importlib.util.spec_from_file_location('path_redundancy', os.path.join(REPO, 'scripts/check-path-redundancy.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


LINT = load_linter()
def git(*a): return subprocess.check_output(('git',) + a, text=True)
def tracked(): return [f for f in git('ls-files', '-z').split('\0') if f]
def toks(s): return [t for t in re.split(r'[_-]', s.lower()) if t]
def same(a, b): return a == b or b in LINT.SYNONYM_OF.get(a, ())


def owners(dirs): return LINT.ancestors([d for d in dirs if d.lower() not in LINT.CONTAINERS])


# ------------------------------------------------------------------ routes: old name tokens -> src/-mirrored directory
def expand(lines):
    out = {}
    for line in re.sub(r'\n[ \t]+', ' ', lines.strip()).splitlines():
        line = line.split('#')[0].strip()
        if not line: continue
        key, dest = [x.strip() for x in line.split('->')]
        keep = dest.endswith(' keep')
        dest = dest.removesuffix(' keep').strip()
        m = re.match(r'(.*)\{([^}]*)\}(.*)', key)
        for sub in (m.group(2).split() if m else [None]):
            k = (m.group(1) + sub + m.group(3)) if m else key
            out[tuple(k.split())] = (dest.replace('{}', sub or ''), keep)
    return out


ROUTES = expand('''
agg -> agg
agg v6 par -> agg/v6/par
agg v6 -> agg/v6
agg {lookup scope group preagg cte par distinct join jsonb plan topn fuse having key dimmap probe scalar calendar correlated numeric wide
  part child dict sum array combo float range zone cost spill window} -> agg/{}
agg ordered -> agg/order
aggstub -> agg/stub
aggkern -> agg/kern
gpu -> agg/vk
hybrid -> agg/hybrid
kll -> agg/kll
join push -> agg/join
part agg -> agg/part
prod -> agg
prod95 -> agg
on scaling -> agg keep
scope -> agg/scope
par core -> core/par
par -> agg/v6/par
slot -> agg keep
explain -> agg keep
clickbench -> agg keep
{leaf lookup verdict} -> agg keep
s99 winx -> agg/window keep
s99 group -> agg/spill keep
s99 oltp -> table/f2 keep
s99 vacuum -> table/vacuum keep
replica -> table keep
col -> col
col {scope gorder topn key wbuf track build conn point agg index merge dict f2 preagg overlay text tomb dv array scan vacuum zone
  worker reindex dsm partition heapfree} -> col/{}
col ordered -> col/order
col projection -> col/proj
col auto merge -> col/auto_merge
col key cut -> col/key_cut
col scope {pairs twohop text journal patch} -> col/scope/{}
col scope v6 -> col/scope_v6
col gorder group -> col/gorder/group
col wbuf arena -> col/wbuf/arena
col f2 -> col/f2
f2 -> col/f2
format2 -> col/f2
stor f2 -> col/f2
colcodec -> col/codec
shred -> col/shred
proj -> col/proj
topn -> col/topn
automerge -> col/auto_merge
interrupt -> col keep
reloption -> col keep
segfmt -> col keep
s55 granule -> col/granule
s55 point dir -> col/f2 keep
on v1 -> col/v1
p98 f2 -> col/f2
p98 {check ddl decoders inspect journal proj scope tier wide} -> col/f2 keep
p98 {autottl ttl returning temp truncate pg18 drop} -> table/f2 keep
cic -> bm25 keep
dir -> bm25 keep
seal -> bm25 keep
truncate -> bm25 keep
aux -> col/f2 keep
p98 {sql shm} -> core keep
p98 multitenant -> table/f2 keep
p98 encoding -> col/f2 keep
p98 s3 -> table/f2 keep
p98 ext -> core keep
tbl -> table
table -> table
tbl {agg bulk tomb vacuum preagg} -> table/{}
tbl f2 -> table/f2
table f2 -> table/f2
table {agg bulk tomb vacuum preagg} -> table/{}
stor v1 -> table/v1
v6 partitions -> table
v6 virtual -> table keep
virtual -> table keep
publication -> table keep
copy -> table keep
churn -> table/f2
vm -> table keep
zero -> col/f2 keep
bm25 -> bm25
{rank store build read verify filter merge query dead include index vacuum scan wbuf write plan parallel kernels} -> bm25/{}
planner -> bm25/plan
analyze -> bm25/analyze
analyzer -> bm25/analyze
{reference phrase prefix fuzzy facet facets field fields position recheck rls and array count dirty edge insert lang list live ops
  promote sift} -> bm25 keep
core -> core
{shm cache ext extension extlock leak leakcheck limits errors locks lock monitor ddl introspection surface install json library
  relcache settings shared sql scaling fresh replication sb m4} -> core keep
roles -> core/roles
quarantine -> quarantine
maint -> maint
bgw -> maint keep
mem -> mem
fault -> fault
compat -> compat
compat v1 -> compat/v1
v1 -> compat/v1
format v1 -> compat keep
upgrade -> compat/upgrade
pg upgrade -> compat/pg_upgrade
old -> compat keep
v6 guc -> core keep
''')
TAP_DIRS = set('''col col/scope col/build col/track col/worker col/auto_merge col/key_cut col/reindex col/vacuum col/wbuf col/dsm col/merge col/dv
col/heapfree col/order col/dict col/partition col/f2 table table/f2 maint bm25 bm25/store bm25/vacuum bm25/index compat compat/upgrade
compat/pg_upgrade core agg agg/v6 mem quarantine fault'''.split())
ISO_DIRS = set('agg bm25 col col/conn col/f2 col/scope col/track col/wbuf table table/f2 quarantine core'.split())
PERL = {n: d for d, ns in {'bm25': 'crash standby wal_minimal wal_consistency parallel_crash postmaster_kill', 'col': 'space space_cache',
        'agg': 'lookup_shared lookup_shared_concurrent dimmap', 'core': 'leakcheck_identity', 'compat': 'v51_full_compat v6_dump_restore_tablespace',
        'table': 'truncate', 'table/f2': 'dur_unlogged_wal_cdc vm_col_delta_xid vm_frozen_standby'}.items() for n in ns.split()}
FIX_TOKENS = {('s3', 'mock'): ['object', 'store', 'mock'], ('p4',): ['phase4']}
TOKEN_SPLIT = {'textkey': ['text', 'key'], 'textmember': ['text', 'member'], 'texttwohop': ['text', 'twohop']}


def route(name, allowed=None):
    """(dir parts, stem tokens) of an old test name; None when no route knows its prefix."""
    t = [x for w in re.split(r'[_-]', name.lower()) if w for x in TOKEN_SPLIT.get(w, [w])]
    for n in range(min(4, len(t)), 0, -1):
        hit = ROUTES.get(tuple(t[:n]))
        if not hit: continue
        dirs, keep = hit[0].split('/'), hit[1]
        k = max((i for i in range(len(dirs), 0, -1) if allowed is None or '/'.join(dirs[:i]) in allowed), default=0)
        if not k: continue
        used = int(bool(MILESTONE.match(t[0]))) if keep else n if k == len(dirs) else consumed(t[:n], dirs[:k])
        rest = t[max(used, int(bool(MILESTONE.match(t[0]))) if len(t) > 1 else 0):]
        return dirs[:k], FIX_TOKENS.get(tuple(rest), rest)
    return route('_'.join(t[1:]), allowed) if len(t) > 1 and MILESTONE.match(t[0]) else None


def consumed(key, dirs):
    i = 0
    for d in dirs:
        want = toks(d)
        if len(key[i:]) >= len(want) and all(same(a, b) for a, b in zip(key[i:], want)): i += len(want)
        else: break
    return i


def clean(t, dirs):
    t, anc = list(t), owners(dirs)
    while t:
        if MILESTONE.match(t[0]): t = t[1:]
        elif '_'.join(t) in anc: t = []
        elif t[0] in anc: t = t[1:]
        elif len(t) > 1 and f'{t[0]}_{t[1]}' in anc: t = t[2:]
        elif t[-1] in anc: t = t[:-1]
        elif len(t) > 1 and f'{t[-2]}_{t[-1]}' in anc: t = t[:-2]
        else: break
    return t


FALLBACKS = ['basic', 'core', 'main']


TAIL_RESPELL = {'cond': 'conditional', 'on': 'onclause'}


def place(old_to_dir_toks, base=()):
    """{key: (dirs, tokens)} -> {key: (dirs, stem)} with stripped stems, 'basic' for an empty one, and no two keys on one path."""
    stems, taken, cut = {}, collections.defaultdict(set), {}
    for k, (dirs, t) in sorted(old_to_dir_toks.items()):
        c = clean(t, tuple(base) + tuple(dirs))
        if c: stems[k] = (dirs, ('v' if all(x.isdigit() for x in c) else '') + '_'.join(c)); cut[k] = len(c) < len(t) and t[-1] in TAIL_RESPELL
    at = collections.Counter((tuple(d), x) for d, x in stems.values())
    for k, (d, x) in list(stems.items()):
        if at[(tuple(d), x)] > 1 and cut[k]: stems[k] = (d, x + '_' + TAIL_RESPELL[old_to_dir_toks[k][1][-1]])
    for k, (d, x) in stems.items(): taken['/'.join(d)].add(x)
    for k, (dirs, t) in sorted(old_to_dir_toks.items()):
        if k in stems: continue
        stems[k] = (dirs, next(f for f in FALLBACKS if f not in taken['/'.join(dirs)]))
        taken['/'.join(dirs)].add(stems[k][1])
    return stems


# ------------------------------------------------------------------ the fixtures of test/col/fixtures
FIXTURES = expand('''
topn xp -> topn/arith
topn xs -> topn/func
topn xc -> topn/cond
topn or -> topn/disj
topn param -> topn/param
topn key -> topn/key
topn xf -> topn/coalesce
topn part -> topn/part
topn semi -> topn/semi
store -> topn keep
t -> trap
pa -> partition
n -> nulls
w -> wide
k -> keys
j -> join
tr -> tid
vd -> jsonb/value
ps -> dim
h -> hybrid
pt -> subplan
g -> group
do -> distinct_on
wl -> workload
eo -> elem_ops
per order -> per_order
m -> types
f -> flat
pi -> partial
nf -> nested
p -> pushdown
jp -> jsonb/path
d -> distinct
o -> ordered_set
e -> expr
item -> jsonb/items
ee -> jsonb/items
{exists contains path not} -> jsonb/items keep
{metrics metric} -> jsonb/metrics
rc -> jsonb/recordset
notes -> jsonb/notes
x -> bool
c -> series
q -> quals
z -> zone
b -> bm25
{spine quarter month} -> series keep
''')
FIXTURE_RENAME = {'store_topn': 'store_rank'}


def fixture_route(name):
    t = toks(name)
    for n in range(min(3, len(t)), 0, -1):
        hit = FIXTURES.get(tuple(t[:n]))
        if hit:
            return hit[0].split('/'), ['elem'] + t[n:] if t[0] == 'ee' else t[0 if hit[1] else n:]
    return None


# ------------------------------------------------------------------ explicit renames of the harness trees
LIBS = {'lib.bash': 'common.bash', 'crash_scenarios.bash': 'crash_scenarios.bash', 'SiftTest.pm': 'SiftTest.pm', 'SiftCompat.pm': 'SiftCompat.pm',
        **{f'lib_{n}.bash': f'{n}.bash' for n in 'col compat docker names twohop upgrade'.split()}}
LOOSE = {'agg_helpers': 'agg/base', 'agg_having_helpers': 'agg/having', 'agg_preagg_helpers': 'agg/preagg', 'agg_preagg_units': 'agg/preagg_units',
         'agg_probe_helpers': 'agg/probe', 'agg_rows_helpers': 'agg/rows', 'agg_same_helpers': 'agg/same', 'agg_check_pushed': 'agg/check_pushed',
         'agg_check_query': 'agg/check_query', 'agg_cte_inline_fixture': 'agg/cte_inline', 'scope_helpers': 'col/scope/base',
         'scope_multiset_helpers': 'col/scope/multiset', 'col_scope_textmember_states': 'col/scope/textmember_states',
         'col_scope_texttwohop_states': 'col/scope/texttwohop_states', 'proj_helpers': 'col/proj/base', 'roles_helpers': 'core/roles',
         'ext_recreate_fixture': 'core/ext_recreate', 'sift_plan': 'bm25/plan'}
LOOSE_DATA = {'mem-slope.tsv': 'mem/slope.tsv', 'mem-slope-legacy.tsv': 'mem/slope-legacy.tsv', 'mem-slope-v1.tsv': 'mem/slope-v1.tsv',
              'idle-series.tsv': 'mem/idle-series.tsv', 'group-scale.tsv': 'scaling/group_scale.tsv', 'group_scale.py': 'scaling/group_scale.py',
              'par-sorts.tsv': 'scaling/par_sorts.tsv', 'prod95_scale.py': 'scaling/stream_scale.py', 'prod95_scale.tsv': 'scaling/stream_scale.tsv',
              'col/col_index.sql': 'col/index.sql', 'diff/diff.sql': 'diff/harness.sql', 'matrix/matrix.py': 'matrix/run.py'}
FUZZ = {'diff_fuzz.py': 'diff/cli.py', 'diff_fuzz.sql': 'diff/schema.sql', 'diff_fuzz_data.sql': 'diff/data.sql', 'diff_fuzz_search.sh': 'diff/search.sh',
        'txn_fuzz.py': 'txn/cli.py', 'fuzz.sql': 'schema.sql'}


def explicit_renames(files):
    out = {}
    for f in files:
        rel = f[5:]
        if rel in LOOSE_DATA: out[f] = 'test/' + LOOSE_DATA[rel]
        elif f.startswith('test/') and rel.endswith('.sql') and '/' not in rel and rel[:-4] in LOOSE:
            out[f] = 'test/common/sql/' + LOOSE[rel[:-4]] + '.sql'
        elif f.startswith('test/fuzz/'):
            r = f[10:]
            if r in FUZZ: out[f] = 'test/fuzz/' + FUZZ[r]
            elif re.match(r'(diff|txn)_fuzz/', r):
                d = r.split('/')[0][:4].rstrip('_')
                out[f] = f'test/fuzz/{d}/' + re.sub(r'^fam_', 'fam/', r.split('/', 1)[1])
            elif re.fullmatch(r'\w+_fuzz\.py', r): out[f] = f'test/fuzz/{r[:-8]}.py'
            elif re.fullmatch(r'fz_\w+\.[ch]', r): out[f] = f'test/fuzz/c/{r[3:]}'
        elif f.startswith('test/scaling/'):
            r = f[13:]
            if re.fullmatch(r'case_\w+\.c', r): out[f] = f'test/scaling/cases/{r[5:]}'
            elif r == 'check_scaling_c.c': out[f] = 'test/scaling/runner.c'
        elif f.startswith('test/bench_on/bench_on_'): out[f] = 'test/bench_on/' + f[23:]
        elif f.startswith('test/t/tbl_vm_iso/'): out[f] = 'test/t/table/vm_iso/' + f[18:].split('/')[0] + '/basic.' + f.rsplit('.', 1)[1]
    return out


# ------------------------------------------------------------------ the plan
class Plan:
    def __init__(self):
        self.files, self.moves, self.deletes, self.errs = tracked(), {}, set(), []
        self.have = set(self.files)

    def add(self, old, new):
        if old != new: self.moves[old] = new

    def flat(self, root, ext):
        return sorted(f[len(root) + 1:-len(ext)] for f in self.files if f.startswith(root + '/') and f.endswith(ext) and '/' not in f[len(root) + 1:])

    def routed(self, names, allowed, what, base):
        got = {}
        for n in names:
            r = route(n, allowed)
            if r is None: self.errs.append(f'unrouted {what}: {n}')
            else: got[n] = r
        return place(got, base)

    def tests(self, root, ext, allowed, what):
        stems = self.routed(self.flat(root, ext), allowed, what, tuple(root.split('/')))
        for n, (d, s) in stems.items(): self.add(f'{root}/{n}{ext}', f'{root}/{"/".join(d)}/{s}{ext}')
        return stems

    def expected(self, root, stems):
        for n in self.flat(root, '.out'):
            m = re.fullmatch(r'(.*)_(\d+)', n)
            base, alt = (n, '') if n in stems else (m.group(1), '_' + m.group(2)) if m and m.group(1) in stems else (None, '')
            if base is None: self.errs.append(f'orphan expected: {root}/{n}.out'); continue
            d, s = stems[base]
            self.add(f'{root}/{n}.out', f'{root}/{"/".join(d)}/{s}{alt}.out')

    def sql(self):
        stems = self.tests('test/sql', '.sql', None, 'sql test')
        self.expected('test/expected', stems)
        for n in self.flat('scripts/focus', '.tsv'):
            if n == '_hot': continue
            if n not in stems: self.errs.append(f'focus table of no test: {n}')
            else: self.add(f'scripts/focus/{n}.tsv', f'scripts/focus/{"/".join(stems[n][0])}/{stems[n][1]}.tsv')
        self.expected('test/isolation/expected', self.tests('test/isolation/specs', '.spec', ISO_DIRS, 'isolation spec'))

    def tap(self):
        names = {}
        for ext in ('.sh', '.pl', '.py', '.tsv'):
            for n in self.flat('test/t', ext):
                if n in LIBS or f'{n}{ext}' in LIBS: continue
                base = re.sub(r'^\d+_', '', n) if ext == '.pl' else n
                r = (PERL[base].split('/'), toks(base)) if ext == '.pl' and base in PERL else route(base, TAP_DIRS)
                if r is None: self.errs.append(f'unrouted tap test: {n}{ext}')
                else: names[n + ext] = r
        for k, (d, s) in place(names, ('test', 't')).items():
            stem, ext = os.path.splitext(k)
            self.add(f'test/t/{k}', f'test/t/{"/".join(d)}/{s}{ext}')
        for old, new in LIBS.items():
            tgt = f'test/t/lib/{new}'
            if f'test/t/{old}' not in self.have: continue
            (self.deletes.add(f'test/t/{old}') if tgt in self.have else self.add(f'test/t/{old}', tgt))

    def fixtures(self):
        names = {}
        for n in self.flat('test/col/fixtures', '.sql'):
            r = fixture_route(n)
            if n in FIXTURE_RENAME: r = (['topn'], [FIXTURE_RENAME[n]])
            if r is None: self.errs.append(f'unrouted fixture: {n}')
            else: names[n] = r
        for n, (d, s) in place(names, ('test', 'col', 'fixtures')).items(): self.add(f'test/col/fixtures/{n}.sql', f'test/col/fixtures/{"/".join(d)}/{s}.sql')

    def run(self):
        self.sql(), self.tap(), self.fixtures()
        for old, new in explicit_renames(self.files).items(): self.add(old, new)
        self.check()
        return self

    def check(self):
        final = collections.defaultdict(list)
        for f in self.files:
            if f not in self.deletes: final[self.moves.get(f, f)].append(f)
        for new, olds in sorted(final.items()):
            if len(olds) > 1: self.errs.append(f'collision on {new}: {" ".join(olds)}')
            if not new.startswith(SCOPE) or new.startswith('test/unit/'): continue
            for rule, why in LINT.findings(new): self.errs.append(f'{rule}: {new}: {why}')


def cmd_plan(a):
    p = Plan().run()
    rows = sorted(p.moves.items())
    kept = len(p.files) - len(rows) - len(p.deletes)
    summary = [f'tracked files: {len(p.files)}', f'moves: {len(rows)}', f'deletes (forwarders): {len(p.deletes)}', f'kept in place: {kept}', f'errors: {len(p.errs)}']
    by = collections.Counter('/'.join(old.split('/')[:3 if old.startswith(('test/isolation/', 'test/col/')) else 2][:-1] or ['test']) for old, _ in rows if old.startswith('test/'))
    summary += [f'  {k}: {v}' for k, v in sorted(by.items())] + [f'  scripts/focus: {sum(1 for o, _ in rows if o.startswith("scripts/focus/"))}']
    if a.out: open(a.out, 'w').write(''.join(f'{o}\t{n}\n' for o, n in rows) + ''.join(f'{d}\t-\n' for d in sorted(p.deletes)))
    if a.summary: open(a.summary, 'w').write('\n'.join(summary + sorted(p.errs)) + '\n')
    print('\n'.join(summary)), print('\n'.join(sorted(p.errs)[:80]))
    return 1 if p.errs else 0


# ------------------------------------------------------------------ the move table on disk (.git/spec100-plan.tsv) and the names it moves
MAPFILE = os.path.join(REPO, git('rev-parse', '--git-dir').strip(), 'spec100-plan.tsv')
REWRITTEN = MAPFILE.replace('plan.tsv', 'rewritten')


def save_map(p):
    with contextlib.suppress(FileNotFoundError): os.remove(REWRITTEN)
    old = dict(l.rstrip('\n').split('\t') for l in open(MAPFILE)) if os.path.exists(MAPFILE) else {}
    old.update(p.moves), old.update({d: '-' for d in p.deletes})
    open(MAPFILE, 'w').write(''.join(f'{o}\t{n}\n' for o, n in sorted(old.items())))


def load_map():
    rows = dict(l.rstrip('\n').split('\t') for l in open(MAPFILE)) if os.path.exists(MAPFILE) else {}
    return {o: LIBS_NEW[o] if n == '-' else n for o, n in rows.items()}, {o for o, n in rows.items() if n == '-'}


LIBS_NEW = {f'test/t/{o}': f'test/t/lib/{n}' for o, n in LIBS.items()}


class Names:
    KINDS = (('sql', 'test/sql/', '.sql'), ('sh', 'test/t/', '.sh'), ('pl', 'test/t/', '.pl'), ('iso', 'test/isolation/specs/', '.spec'),
             ('fx', 'test/col/fixtures/', '.sql'), ('focus', 'scripts/focus/', '.tsv'))

    def __init__(self, moves):
        self.moves = moves
        self.m = {k: {} for k, _, _ in self.KINDS}
        for o, n in moves.items():
            for k, root, ext in self.KINDS:
                if o.startswith(root) and o.endswith(ext) and '/' not in o[len(root):] and n.endswith(ext):
                    self.m[k][o[len(root):-len(ext)]] = n[len(root):-len(ext)]

    def job(self, i):
        """the new id of an old job id (a shard or a smoke variant keeps its suffix), or None"""
        for pre, kind in (('installcheck-', 'sql'), ('tap-shell-', 'sh'), ('tap-', 'pl')):
            if not i.startswith(pre) or (pre == 'tap-' and i.startswith('tap-shell-')): continue
            body = i[len(pre):]
            for cut in range(len(body), 0, -1):
                if body[:cut] in self.m[kind] and re.fullmatch(r'(-[\w]+|_[b-h])?', body[cut:]):
                    return pre + self.m[kind][body[:cut]] + body[cut:]
        return None

    def jobs(self):
        out = {}
        for pre, kind in (('installcheck-', 'sql'), ('tap-shell-', 'sh'), ('tap-', 'pl')):
            for o, n in self.m[kind].items():
                out[pre + o] = pre + n
                if o in ('fault_matrix', 'fault_matrix_standby'): out[f'{pre}{o}-smoke'] = f'{pre}{n}-smoke'
                if o == 'mem_slope': out.update({f'{pre}{o}-{k}': f'{pre}{n}-{k}' for k in (1, 2, 3)})
        return out


def text_files(extra=()):
    skip = ('research-private/', 'docs/releases/', 'scripts/spec100-migrate.py', 'CHANGELOG.md', 'docs/results/')
    for f in tracked():
        if f.startswith(skip) or not os.path.isfile(f): continue
        try: open(f, encoding='utf-8').read()
        except Exception: continue
        yield f


def edit(path, fn):
    old = open(path, encoding='utf-8', newline='').read()
    new = fn(old)
    if new != old: open(path, 'w', encoding='utf-8', newline='').write(new)
    return new != old


# ------------------------------------------------------------------ pass A: every path of a moved file, wherever it is written
PATH_RX = re.compile(r'(?<![\w.-])((?:test|scripts/focus)/[\w./+@-]*\w)|(?<![\w./$-])(t/[\w]+\.pl)')
DIR_MOVES = {'test/t/tbl_vm_iso': 'test/t/table/vm_iso', 'test/fuzz/diff_fuzz': 'test/fuzz/diff', 'test/fuzz/txn_fuzz': 'test/fuzz/txn'}
EXTS = {'test/sql/': ('.sql',), 'test/expected/': ('.out',), 'test/t/': ('.sh', '.pl'), 'test/isolation/specs/': ('.spec',),
        'test/isolation/expected/': ('.out',), 'test/col/fixtures/': ('.sql',), 'scripts/focus/': ('.tsv',)}


def path_rewriter(moves):
    def one(c):
        if c in moves: return moves[c]
        for old, new in DIR_MOVES.items():
            if c == old or c.startswith(old + '/'): return new + c[len(old):]
        for root, exts in EXTS.items():
            if c.startswith(root):
                for e in exts:
                    if c + e in moves: return moves[c + e][:-len(e)]
        if c.startswith('t/') and 'test/' + c in moves: return moves['test/' + c][5:]
        if c.startswith(('test/fuzz/', 'test/scaling/', 'test/bench_on/')):
            cut = {moves[o][:len(moves[o]) - len(o) + len(c)] if moves[o].endswith(o[len(c):]) else None for o in moves if o.startswith(c) and o != c}
            if len(cut) == 1 and None not in cut: return cut.pop()
        return None

    def run(text):
        return PATH_RX.sub(lambda m: one(m.group(1) or m.group(2)) or m.group(0), text)
    return run


# ------------------------------------------------------------------ pass B: names written bare (lists, job ids, command lines)
def retoken(text, mapping):
    return re.sub(r'(?<![\w/.-])\w+(?![\w/])', lambda m: mapping.get(m.group(0), m.group(0)), text)


def list_rules(N):
    sql, sh, pl, iso = N.m['sql'], N.m['sh'], N.m['pl'], N.m['iso']
    return [(r'(?:SLOW_PERL|SLOW_PERL_TESTS)="([^"]*)"', pl), (r'\bSLOW_SHELL="([^"]*)"', sh), (r'\b(?:SLOW_REGRESS|STOCK_SMOKE)="([^"]*)"', sql),
            (r'\b(?:QUICK_)?TESTS=\(([^)]*)\)', sql), (r'SIFT_ASSERT_SHTAP-([^}]*)\}', sh), (r'\btap_perl ([^#\n;)]*)', pl), (r'\btap_shell ([^#\n;)]*)', sh),
            (r'\binstallcheck ([^#\n;)]*)', sql), (r'\brest t \.sh ([^)#\n]*)', sh), (r'\brest t \.pl ([^)#\n]*)', pl),
            (r'(?:isolation\.sh(?: --emulate)?|--emulate) ((?!-)[\w]+)', iso), (r'pgtest\.sh"? regress ([\w ]+)', sql), (r'pgtest\.sh"? tap ([\w]+)', sh),
            (r'^want (\w+)', sh)]


def bare_rewriter(N):
    jobs, rules = N.jobs(), [(re.compile(r, re.M), m) for r, m in list_rules(N)]

    def run(text):
        text = re.sub(r'\b(installcheck-|tap-shell-|tap-)([\w]+)', lambda m: N.job(m.group(0)) or m.group(0), text)
        for rx, mp in rules:
            text = rx.sub(lambda m, mp=mp: m.group(0)[:m.start(1) - m.start(0)] + retoken(m.group(1), mp) + m.group(0)[m.end(1) - m.start(0):], text)
        return text
    return run


# ------------------------------------------------------------------ regex of job names -> the same jobs, named by folders
def top_alts(s):
    out, depth, cur, i, br = [], 0, '', 0, False
    while i < len(s):
        c = s[i]
        if c == '\\': cur += s[i:i + 2]; i += 2; continue
        if br: br = c != ']'
        elif c == '[': br = True
        elif c == '(': depth += 1
        elif c == ')': depth -= 1
        if c == '|' and depth == 0 and not br: out.append(cur); cur = ''
        else: cur += c
        i += 1
    return out + [cur]


# ------------------------------------------------------------------ the registries
def esc(s): return re.sub(r'([.+*?()|\[\]{}^$\\])', r'\\\1', s)


WIDENED = []


def tree_cover(chosen, universe, ratio=1.0, keep=()):
    """names of one set as an alternation: a folder with at least RATIO of its names in the set folds to 'folder/.*' (RATIO < 1 widens the set)"""
    folders = sorted({n[:i + 1] for n in chosen for i, c in enumerate(n) if c == '/'}, key=lambda p: (p.count('/'), p))
    done, out = [], []
    for f in folders:
        under = [u for u in universe if u.startswith(f)]
        if not any(f.startswith(d) for d in done) and not any(k.startswith(f) for k in keep) and sum(u in chosen for u in under) >= ratio * len(under):
            out.append(esc(f) + '.*'), done.append(f), WIDENED.extend(u for u in under if u not in chosen)
    return out + [esc(n) for n in sorted(chosen) if not any(n.startswith(d) for d in done)]


def regroup_regex(rx, N, tail_default='', ratio=1.0, keep=()):
    """a regex over job ids: the alternatives that match test jobs are rewritten from the jobs they match"""
    m = re.fullmatch(r'\^\((.*)\)(\$?)', rx, re.S)
    alts, tail, wrap = (top_alts(m.group(1)), m.group(2), True) if m else (top_alts(rx), tail_default, False)
    body = '|'.join(r for a in alts for r in ([('^' if a.startswith('^') else '') + x for x in regroup([a.lstrip('^')], tail, N, ratio, keep)] if not wrap else regroup([a], tail, N, ratio, keep)))
    return f'^({body}){tail}' if wrap else body


def regroup(alts, tail, N, ratio=1.0, keep=()):
    jobs, out = N.jobs(), []
    kinds = (('installcheck-', lambda j: j.startswith('installcheck-')), ('tap-shell-', lambda j: j.startswith('tap-shell-')),
             ('tap-', lambda j: j.startswith('tap-') and not j.startswith('tap-shell-')))
    for alt in alts:
        try: rx = re.compile(alt + tail)
        except re.error: out.append(alt); continue
        hit = [o for o in jobs if rx.match(o)]
        if not hit: out.append(alt); continue
        end = '$' if alt.endswith('$') else ''
        for pre, of in kinds:
            ids = {jobs[o][len(pre):] for o in hit if of(jobs[o])}
            uni = {n[len(pre):] for n in jobs.values() if of(n)}
            if ids and ids == uni and not any(k in uni for k in keep): out.append(pre + ('.*$' if end else ''))
            elif ids: out.append(pre + (f'({"|".join(tree_cover(ids, uni, ratio, keep))})' if len(ids) > 1 or '/' in next(iter(ids)) else esc(next(iter(ids)))) + end)
    return out


JOB_REGEX = re.compile(r"'(\^[^'\n]*(?:installcheck-|tap-)[^'\n]*)'")
KEEP_COMPAT = ('compat/pg_upgrade/basic', 'table/replica_mixed')


def rewrite_job_regexes(N):
    def sh(t):
        t = JOB_REGEX.sub(lambda m: "'" + regroup_regex(m.group(1), N, '', 1.0, KEEP_COMPAT if 'tap-shell-(col_dict_shared_shm' in m.group(1) else ()) + "'", t)
        return t.replace('${COMPAT_JOBS/pg_upgrade|/}', '${COMPAT_JOBS/compat\\/pg_upgrade\\/basic|/}').replace(
            '${COMPAT_JOBS/pg_upgrade|replica_mixed|/}', '${COMPAT_JOBS_17/table\\/replica_mixed|/}')
    for f in ('scripts/ci/affected.sh', '.github/workflows/ci.yml'):
        if os.path.exists(f): edit(f, sh)
    if os.path.exists('scripts/ci/format1-skip.txt'):
        edit('scripts/ci/format1-skip.txt', lambda t: '\n'.join(regroup_regex(l, N) if l.startswith('^') and re.search(r'installcheck-|tap-', l) else l for l in t.split('\n')))


def rewrite_makefile(text, N, final):
    sql, lines, out, block = N.m['sql'], text.split('\n'), [], []

    def flush():
        entries = sorted({e for b in block for e in b})
        out.extend(f'TESTING_REGRESS += {e}' for e in entries)
        block.clear()

    def entry(e):
        if '%' not in e: return {sql.get(e, e)}
        olds = [o for o in sql if re.fullmatch(re.escape(e).replace('%', '.*').replace('\\%', '.*'), o)]
        news = {sql[o] for o in olds}
        lcp = os.path.commonprefix(sorted(news))
        return {lcp + '%'} if news and {f for f in final if f.startswith(lcp)} == news else news
    for l in lines:
        m = re.fullmatch(r'TESTING_REGRESS \+= (\S+)', l)
        if m: block.append(entry(m.group(1)))
        else:
            flush(); out.append(l)
    flush()
    return '\n'.join(out)


def rewrite_areas(text, N, old_files, moves):
    out = []
    for l in text.split('\n'):
        f = l.split('\t')
        if l.startswith('=') and len(f) == 2: out.append(f'{f[0]}\t{regroup_regex(f[1], N, "", 0.6)}')
        elif len(f) == 2 and not l.startswith('#'):
            c1, gone = f[0], [o for o in old_files if re.search(f[0], o) and not re.search(f[0], moves[o])]
            if gone:
                kept = [t for t in tracked() if re.search(f[0], t)]
                cover_rx = '^(' + '|'.join(tree_cover({moves[o] for o in gone}, set(tracked()))) + ')$'
                c1 = f'{f[0]}|{cover_rx}' if kept else cover_rx
            c2 = regroup_regex(f[1], N, '', 0.6) if re.search(r'installcheck-|tap-', f[1]) and not re.search(r'\\\d', f[1]) else f[1]
            out.append(f'{c1}\t{c2}')
        else: out.append(l)
    return '\n'.join(out)


def rewrite_coverage(text, N):
    sql, fx, sh = N.m['sql'], N.m['fx'], N.m['sh']

    def tok(t):
        for p, mp in (('fixture:', fx), ('test:', {**sh, **sql}), ('@', fx)):
            if t.startswith(p) and t[len(p):] in mp: return p + mp[t[len(p):]]
        return t
    rows = []
    for l in text.split('\n'):
        f = l.split('\t')
        if len(f) >= 11 and not l.startswith('#') and f[0] != 'id':
            f[8] = ','.join(tok(t) for t in f[8].split(','))
            f[10] = tok(f[10]) if f[10].startswith('@') else f[10]
        rows.append('\t'.join(f))
    return '\n'.join(rows)


def rewrite_focus(path, N):
    def fn(t):
        name, _, rest = t.partition('\t')
        return (N.m['sql'].get(name, name) + '\t' + rest) if name != '=hot' else t
    edit(path, fn)


def column_rules(N):
    sql, sh, pl, rel = N.m['sql'], N.m['sh'], N.m['pl'], lambda root, mp, ext: (lambda n: mp[n[:-len(ext)]] + ext if n.endswith(ext) and n[:-len(ext)] in mp else n)
    tap = lambda n: rel('', sh, '.sh')(n) if n.endswith('.sh') else rel('', pl, '.pl')(n)
    return {'scripts/regress-times.tsv': (0, lambda n: sql.get(n, n)), 'scripts/valgrind-leaks.tests': (0, lambda n: sql.get(n, n)),
            'scripts/tap-sleeps.allow': (0, tap), 'scripts/scratch.tsv': (0, lambda n: N.job(n) or n),
            'scripts/test-budget.allow': (1, rel('', sql, '.sql')), 'scripts/budgets.tsv': (0, lambda n: 'tap:' + sh.get(n[4:], n[4:]) if n.startswith('tap:') else 'fuzz:' + N.moves.get('test/fuzz/' + n[5:], 'test/fuzz/' + n[5:])[10:] if n.startswith('fuzz:') else n)}


def rewrite_columns(N):
    for path, (col, fn) in column_rules(N).items():
        if not os.path.exists(path): continue

        def run(text, col=col, fn=fn):
            rows = []
            for l in text.split('\n'):
                f = l.split('\t')
                if not l.startswith('#') and len(f) > col: f[col] = fn(f[col])
                rows.append('\t'.join(f))
            return '\n'.join(rows)
        edit(path, run)

    def pins(text):
        rows = []
        for l in text.split('\n'):
            f = l.split('\t')
            if not l.startswith('#') and len(f) > 1: f[1] = ' '.join(N.m['sql'].get(n, n) for n in f[1].split())
            rows.append('\t'.join(f))
        return '\n'.join(rows)
    if os.path.exists('scripts/focus-pins.tsv'): edit('scripts/focus-pins.tsv', pins)


# ------------------------------------------------------------------ the sources of the shell and Perl tests
ROOT_OF = re.compile(r'\$\(cd "\$\(dirname "\$0"\)/((?:\.\./)*\.\.)" && pwd\)')
HERE_OF = re.compile(r'\$\(dirname "\$0"\)|\$\{?HERE\}?(?![\w])')


def rewrite_tap_sources(path, moves, paths):
    """a test finds its repo and its siblings by SIFT_REPO and the new path of what it names, not by its own folder"""
    def at(rel):
        p = posixpath.normpath('test/t/' + rel)
        return '${SIFT_REPO:?}' + ('' if p == '.' else '/' + paths(p))

    def sh(t):
        t = ROOT_OF.sub(lambda m: at(m.group(1)), t)
        t = re.sub(r'(?:\$\(dirname "\$0"\)|\$\{?HERE\}?)/([\w./-]*\w)', lambda m: at(m.group(1)), t)
        t = re.sub(r'git -C "\$\(dirname "\$0"\)"', 'git -C "${SIFT_REPO:?}"', t)
        return re.sub(r'^HERE=\$\(dirname "\$0"\)\n', '', t, flags=re.M) if not re.search(r'\$\{?HERE\b', re.sub(r'^HERE=.*$', '', t, flags=re.M)) else t

    def pl(t): return t.replace('use lib $FindBin::RealBin;', 'use lib "$ENV{SIFT_REPO}/test/t/lib";')
    return edit(path, sh if path.endswith('.sh') else pl)


LIB_FIXES = [('/../../scripts/', '/../../../scripts/'), ('/lib_docker.bash', '/docker.bash'), ('POOL_SCRIPT=$(basename "$0" .sh)',
             'POOL_SCRIPT=$(t=${0#*/test/t/}; t=${t%.sh}; t=${t//\\//__}; (( ${#t} <= 32 )) && echo "$t" || echo "${t:0:23}_$("$(dirname "${BASH_SOURCE[0]}")/../../../scripts/sha256.sh" <<<"$0" | cut -c1-8)")'),
             ('"$FindBin::RealBin/../.."', '"$FindBin::RealBin/../../.."')]
LIB_PROSE = [(r'(?<![/\w])lib\.bash\b', 'lib/common.bash'), (r'(?<![/\w])lib_(col|compat|docker|names|twohop|upgrade)\.bash\b', r'lib/\1.bash')]


def replace_in(path, pairs):
    def run(t):
        for a, b in pairs: t = re.sub(a, b, t)
        return t
    if os.path.exists(path): edit(path, run)


FIND_SQL = '$(cd "$REPO/test/sql" && find . -name "*.sql" | sed "s|^\\./||" | LC_ALL=C sort)'
FOCUS_FILES = '$(find "$REPO/scripts/focus" -name "*.tsv")'
SELFTEST_FIXES = [
    (r'for f in "\$REPO"/test/sql/\*\.sql; do(\s+)f=\$\{f##\*/\};?', lambda m: 'for f in ' + FIND_SQL + '; do'),
    (r'"\$REPO"/scripts/focus/\*\.tsv', lambda m: FOCUS_FILES),
    (r'ls "\$REPO/test/sql" \| sed \'s/\\\.sql\$//\' \| sort', lambda m: '(cd "$REPO/test/sql" && find . -name "*.sql" | sed "s|^\\./||; s/\\.sql$//") | sort'),
    (r'tap-shell-\(compat_matrix\|old_lib_refusal\|replica_mixed\|pg_upgrade\)',
     lambda m: 'tap-shell-(compat\\/matrix|compat\\/old_lib_refusal|table\\/replica_mixed|compat\\/pg_upgrade\\/)'),
    (r'tap-shell-core/scaling_slope\)/', lambda m: 'tap-shell-core\\/scaling_slope)/'),
    (r'\[a-z0-9_-\]\+\\t\[0-9\]\+', lambda m: '[a-z0-9_/-]+\\t[0-9]+'),
    (r"SIFT_CI_JOBS='fault_matrix\|", lambda m: "SIFT_CI_JOBS='fault/matrix|"),
    (r'for t in format2_col_am table_f2_parity;', lambda m: 'for t in col/f2/am table/f2/parity;')]
PLAN_FIXES = [(r"f=\$\{f##\*/\}; guarded\[\$\{f%\.sql\}\]=1; done < <\(grep -lE ('[^']*') \"\$REPO\"/test/sql/\*\.sql\)",
               lambda m: 'f=${f#"$REPO/test/sql/"}; guarded[${f%.sql}]=1; done < <(grep -rlE --include=\'*.sql\' ' + m.group(1) + ' "$REPO/test/sql")'),
              (r'\*fault_matrix\*', lambda m: '*fault/matrix*')]
INTERNAL = {'scripts/ci/selftest.sh': SELFTEST_FIXES, 'scripts/ci/plan.sh': PLAN_FIXES, 'scripts/ci/shards.sh': [(r'\)pg_upgrade "', lambda m: ')compat/pg_upgrade/basic "')],
            'scripts/ci.sh': [(r'SIFT_CI_JOBS=fault_matrix', lambda m: 'SIFT_CI_JOBS=fault/matrix')],
            'scripts/ci/affected.sh': [(r'bm25_fuzz\*', lambda m: 'bm25*'),
                                       (r'COMPAT_JOBS_ARM=\$\{[^}]*\}', lambda m: 'COMPAT_JOBS_ARM=${COMPAT_JOBS_17/|table\\/replica_mixed)/)}')],
            'test/scaling/cases.mk': [(r'case_(\w+)\.c', lambda m: f'cases/{m.group(1)}.c')], 'test/scaling/Makefile': [(r'check_scaling_c\.c', lambda m: 'runner.c')]}


def cmd_apply(a):
    p = Plan().run()
    if p.errs: print('\n'.join(p.errs)); return 1
    if p.moves or p.deletes: save_map(p)
    UNIGNORE = ['!/test/**/core/', '!/scripts/focus/core/']
    edit('.gitignore', lambda t: t if UNIGNORE[0] in t else t.replace('!/src/core/\n', '!/src/core/\n' + '\n'.join(UNIGNORE) + '\n'))
    for d in sorted(p.deletes): os.remove(d)
    for old, new in sorted(p.moves.items()):
        os.makedirs(os.path.dirname(new), exist_ok=True)
        os.rename(old, new)
    paths = sorted(p.deletes | set(p.moves) | set(p.moves.values()))
    subprocess.run(['git', 'add', '-A', '--pathspec-from-file=-', '--pathspec-file-nul'], input='\0'.join(paths + ['.gitignore']), text=True, check=True)
    for old in sorted(set(p.deletes) | set(p.moves)):
        with contextlib.suppress(OSError): os.removedirs(os.path.dirname(old))
    print(f'moved {len(p.moves)} files, removed {len(p.deletes)} forwarders')
    return 0


def cmd_rewrite(a):
    moves, deleted = load_map()
    if not moves: print('rewrite-refs: no move table (run --apply-moves first)'); return 1
    if os.path.exists(REWRITTEN): print('rewrite-refs: the references already follow the move table'); return 0
    N, old_files = Names(moves), set(moves)
    final = {f[9:-4] for f in tracked() if f.startswith('test/sql/') and f.endswith('.sql')}
    paths = path_rewriter(moves)
    rewrite_job_regexes(N)
    edit('Makefile', lambda t: rewrite_makefile(t, N, final))
    edit('scripts/areas.tsv', lambda t: rewrite_areas(t, N, old_files, moves))
    edit('docs/coverage.tsv', lambda t: rewrite_coverage(t, N))
    for f in tracked():
        if f.startswith('scripts/focus/') and f != 'scripts/focus/_hot.tsv': rewrite_focus(f, N)
    rewrite_columns(N)
    bare = bare_rewriter(N)
    for f in tracked():
        if f.startswith('test/t/') and f.endswith(('.sh', '.pl')) and not f.startswith('test/t/lib/'): rewrite_tap_sources(f, moves, paths)
        if f in {v for k, v in moves.items() if k in LIBS_NEW and k not in deleted}:
            replace_in(f, [(re.escape(a), b.replace('\\', '\\\\')) for a, b in LIB_FIXES])
    for f in text_files():
        edit(f, paths)
        if f.startswith(('scripts/', '.github/')) and f != 'scripts/areas.tsv': edit(f, bare)
    for f, pairs in INTERNAL.items(): replace_in(f, pairs)
    print(f'areas.tsv: {len(set(WIDENED))} job(s) join a group by their folder')
    for f in text_files(): replace_in(f, LIB_PROSE)
    for f in ('scripts/check-leak-hooks.sh', 'scripts/check-pg-bare.sh'): replace_in(f, [(r'\blib\\\.bash', r'lib/common\\.bash')])
    open(REWRITTEN, 'w').close()
    return 0


def cmd_verify(a):
    p, errs = Plan().run(), []
    errs += [f'plan: {e}' for e in p.errs] + [f'not applied: {o} -> {n}' for o, n in sorted(p.moves.items())[:20]]
    moves, _ = load_map()
    old = {o for o in moves if o.startswith(('test/', 'scripts/focus/'))}
    names = Names(moves)
    jobs = names.jobs()
    stale_jobs = {o for o, n in jobs.items() if o != n}
    for f in text_files():
        for i, l in enumerate(open(f, encoding='utf-8').read().split('\n'), 1):
            for m in PATH_RX.finditer(l):
                if (m.group(1) or m.group(2)) in old: errs.append(f'{f}:{i}: dangling path {m.group(1)}')
            for m in re.finditer(r'\b(?:installcheck-|tap-shell-|tap-)\w+', l):
                if m.group(0) in stale_jobs: errs.append(f'{f}:{i}: old job id {m.group(0)}')
            if (m := re.match(r'\s*\\ir? (test/\S+)', l)) and not os.path.exists(m.group(1)): errs.append(f'{f}:{i}: \\ir target {m.group(1)} is gone')
    sql = {f[9:-4] for f in tracked() if f.startswith('test/sql/') and f.endswith('.sql')}
    for l in open('Makefile').read().split('\n'):
        if (m := re.fullmatch(r'TESTING_REGRESS \+= (\S+)', l)) and not (m.group(1) in sql or '%' in m.group(1) and any(s.startswith(m.group(1)[:-1]) for s in sql)):
            errs.append(f'Makefile: {m.group(1)} is no test')
    for l in open('scripts/areas.tsv'):
        if not l.startswith('#'):
            for rx in l.rstrip('\n').split('\t')[:2]:
                try: re.compile(re.sub(r'\\\d|\{\w+\}', '', rx))
                except re.error as e: errs.append(f'areas.tsv: {rx[:60]}: {e}')
    for f in tracked():
        if f.startswith('scripts/focus/') and f != 'scripts/focus/_hot.tsv':
            if open(f).read().partition('\t')[0] != f[len('scripts/focus/'):-4]: errs.append(f'{f}: its first column is not its name')
    for rule in [e for e in errs]: pass
    print('\n'.join(errs[:100])), print(f'verify: {len(errs)} problem(s)')
    return 1 if errs else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--plan', action='store_true'), ap.add_argument('--apply-moves', action='store_true')
    ap.add_argument('--rewrite-refs', action='store_true'), ap.add_argument('--verify', action='store_true')
    ap.add_argument('--out'), ap.add_argument('--summary')
    a = ap.parse_args()
    return cmd_plan(a) if a.plan else cmd_apply(a) if a.apply_moves else cmd_rewrite(a) if a.rewrite_refs else cmd_verify(a) if a.verify else 2


if __name__ == '__main__': sys.exit(main())
