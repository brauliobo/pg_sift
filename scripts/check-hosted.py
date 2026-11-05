#!/usr/bin/env python3
"""A local dry run of the hosted workflow (.github/workflows/ci.yml), for the box that has no runner: python3 -I scripts/check-hosted.py [BASE]

It runs the step that builds the job matrix, with the environment each event would give it and the scope scripts/ci.sh affected --github says for BASE (default: all),
and fails when a job the line promises is missing: PostgreSQL 17, 18 and 19 (git), AArch64, assertions and the full tier; and when the runners or the compiler cache are
not as the line pins them. actionlint runs too when it is on PATH or in $ACTIONLINT. Needs PyYAML; without it nothing is checked and the exit status is 2.
"""
import json, os, shutil, subprocess, sys

try:
    import yaml
except ImportError:
    sys.exit('check-hosted: PyYAML is needed')

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WF = os.path.join(ROOT, '.github/workflows/ci.yml')
doc = yaml.safe_load(open(WF))
bad = []


def need(ok, what):
    if not ok:
        bad.append(what)


def step(job, key, val):
    return next((s for s in doc['jobs'][job]['steps'] if s.get(key) == val), None)


def scope(base):
    out = subprocess.run([os.path.join(ROOT, 'scripts/ci.sh'), 'affected', '--github', base], cwd=ROOT, capture_output=True, text=True, check=True).stdout
    return dict(l.split('=', 1) for l in out.splitlines() if '=' in l)


def matrix(event, sc, cipr=False, versions='', heavy=False, deep=False):
    env = dict(os.environ, EVENT=event, CIPR=str(cipr).lower(), VERSIONS=versions, HEAVY=str(heavy).lower(), DEEP=str(deep).lower(), LABEL_ASSERTIONS='false',
               NSHARDS=sc['nshards'], PG=sc['pg'], JOBS=sc['jobs'], FAST=sc['fast'], FASTJOBS=sc['fastjobs'], FASTPG=sc['fastpg'], CASSERT=sc['cassert'], CASSERTJOBS=sc['cassertjobs'])
    # the step appends its outputs to $GITHUB_OUTPUT and echoes them (tee): the standard output is both
    out = subprocess.run(['bash', '-c', step('changes', 'id', 'matrix')['run']], cwd=ROOT, env=dict(env, GITHUB_OUTPUT='/dev/stdout'), check=True, capture_output=True, text=True).stdout
    return json.loads(next(l for l in out.splitlines() if l.startswith('matrix='))[len('matrix='):])


def kinds(m):
    return {(j['pg'], j.get('tier'), 'arm' if j.get('arm') else 'git' if j.get('git') else 'assert' if j.get('cassert') else 'x86') for j in m}


base = sys.argv[1] if len(sys.argv) > 1 else 'all'
sc = scope(base)
runs = {
    'push': (matrix('push', sc, versions='17 18 19'), {(17, 'quick', 'x86'), (18, 'quick', 'x86'), (19, 'quick', 'git'), (18, 'quick', 'arm'), (18, 'quick', 'assert')}),
    'dispatch, heavy': (matrix('workflow_dispatch', sc, versions='17 18 19', heavy=True), {(17, 'full', 'x86'), (18, 'full', 'x86'), (19, 'quick', 'git'), (18, 'quick', 'arm')}),
    'pull request, integration branch': (matrix('pull_request', sc, cipr=True), {(18, 'fast', 'x86'), (18, 'fast', 'arm'), (17, 'fast', 'x86'), (19, 'fast', 'git')}),
}
for name, (m, want) in runs.items():
    have = kinds(m)
    print('%-34s %3d jobs: %s' % (name, len(m), ', '.join('pg%d %s %s' % k for k in sorted(have, key=str))))
    need(want <= have, '%s lacks %s' % (name, sorted(want - have, key=str)))

# the runners: pinned images, no floating label
for name, job in doc['jobs'].items():
    ro = str(job.get('runs-on', ''))
    need('latest' not in ro, 'job %s runs on %s: a floating runner image' % (name, ro))
need(doc['jobs']['macos']['runs-on'] == 'macos-15', 'the macOS job is not on macos-15 (arm64)')
need('postgresql@17 postgresql@18' in step('macos', 'name', 'Install bash 5 and PostgreSQL 17 and 18')['run'], 'the macOS job does not install PostgreSQL 17 and 18')

# the compiler cache: installed, on PATH, kept by pg, OS, architecture, compiler and sources (the key is made by a step), restored from the newest cache of the branch or of main, and saved by the first job to finish
pg = doc['jobs']['postgresql']['steps']
inst = next(s for s in pg if str(s.get('name', '')).startswith('Install PostgreSQL'))
keyer = step('postgresql', 'name', 'Key of the compiler cache')
cache = step('postgresql', 'name', 'Cache the compiler')
keep = step('postgresql', 'name', 'Keep the compiler cache')
need(' ccache ' in inst['run'], 'the compiler cache is not installed with PostgreSQL')
need('/usr/lib/ccache >> "$GITHUB_PATH"' in inst['run'], 'the ccache wrappers are not put on PATH')
need(keyer and all(part in keyer['run'] for part in ('matrix.pg', 'RUNNER_OS', 'RUNNER_ARCH', 'gcc', "hashFiles('src/**'")), 'the key of the compiler cache is not made of the PostgreSQL major, the OS, the architecture, the compiler and the sources')
need(cache and cache['with']['path'].endswith('/ccache'), 'the compiler cache does not keep the directory of ccache')
if cache:
    need(cache['uses'].startswith('actions/cache/restore@'), 'the compiler cache is not restored by actions/cache/restore (the save is a step of its own)')
    need('steps.ccache.outputs.key' in cache['with']['key'], 'the compiler cache is not restored by the key of its step')
    need('steps.ccache.outputs.prefix' in cache['with']['restore-keys'] and 'matrix.pg' in cache['with']['restore-keys'], 'restore-keys lacks the prefix of the compiler and the major')
need(keep and keep['uses'].startswith('actions/cache/save@') and keep['with']['key'] == cache['with']['key'] and '!cancelled()' in keep['if'], 'the compiler cache is not saved, under the key it was looked up by, when the run is not cancelled')
need(pg.index(cache) > pg.index(inst), 'the compiler cache is restored before the compiler wrappers are installed')

al = os.environ.get('ACTIONLINT') or shutil.which('actionlint')
if al:
    r = subprocess.run([al], cwd=ROOT, capture_output=True, text=True)
    print('actionlint: %s' % ('ok' if r.returncode == 0 else r.stdout + r.stderr))
    need(r.returncode == 0, 'actionlint')
else:
    print('actionlint: not on PATH (set ACTIONLINT); the hosted lint job runs it')

for b in bad:
    print('check-hosted: ' + b)
sys.exit(1 if bad else 0)
