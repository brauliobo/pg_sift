# The coverage check of the cases (`bench/bench cases coverage`): every claim of a release has a case, rows of the current version, rows of its peers and a block in a page.
# A claim is a CHANGELOG line under `## Unreleased` or a commit since the last tag that changes src/ or sql/ and gives a measured speed. Nothing is measured here.

import os
import re
import subprocess
import sys

import extras

from .registry import read_registry
from .runcfg import HERE, REPO

ALLOW = os.path.join('scripts', 'bench-cases.allow')
RESULTS = os.path.join('bench', 'results')
OUT = os.path.join('bench', 'out')   # the generated pages are untracked: they exist after a run of `bench/bench cases --docs`
PAGES = (os.path.join(OUT, 'README-blocks.md'), os.path.join(OUT, 'BENCHMARKS.md'))
VERSION = re.compile(r'\b\d+\.\d+\.\d+\b')   # a release number (`3.4.0 to 3.7.1`) is no figure of a speed
CLAIM = re.compile(r'\d[\d,.]* ?(?:ms|us|µs|microseconds?|s)? (?:against|->|to|before|was|vs\.?) \d|\d[\d.]*x (?:faster|less|more|smaller|slower)|instructions|mispredict')
FOOTER = re.compile(r'`n/a`: (?:a measure )?not measured under')   # steps/triple.py NOT_MEASURED, whatever the version it names
LANES = ('sift', 'native')
FIGURE = 'figure '   # a waiver of ALLOW: `figure PHRASE<TAB>reason`, a CHANGELOG line that states a figure with no row in bench/claims.tsv yet
ROWS = 1000000   # the size of the published cases (bench/results/cases.tsv)


def read(root, path):
    with open(os.path.join(root, path)) as f:
        return f.read()


def git(root, *args):
    return subprocess.run(['git', '-C', root, *args], capture_output=True, text=True).stdout


def last_tag(root):
    return git(root, 'describe', '--tags', '--abbrev=0').strip()


def landed(root, since):
    """[(short sha, subject, touches the CHANGELOG)] of the commits since `since` that change src/ or sql/ and say a measured speed in their message"""
    out = []
    for block in git(root, 'log', '--first-parent', '--format=%x00%h%x01%B%x01', '--name-only', since + '..HEAD').split('\0')[1:]:
        sha, message, files = block.split('\x01')
        files = files.split()
        if any(f.startswith(('src/', 'sql/')) for f in files) and CLAIM.search(VERSION.sub('', message)):
            out.append((sha, message.split('\n')[0], 'CHANGELOG.md' in files))
    return out


def unreleased(root):
    """the lines of `## Unreleased` that add or change something, or give a before and after"""
    out, on = [], False
    for line in read(root, 'CHANGELOG.md').split('\n'):
        if line.startswith('## '):
            on = line.startswith('## Unreleased')
        elif on and line.startswith('- '):
            kind = line[2:].split(':', 1)[0]
            if kind in ('Added', 'Changed') or kind in ('Improved', 'Fixed') and CLAIM.search(VERSION.sub('', line)):
                out.append(line)
    return out


def allow_list(root):
    lines = [l.partition('\t') for l in read(root, ALLOW).split('\n') if l.strip() and not l.startswith('#')] if os.path.exists(os.path.join(root, ALLOW)) else []
    return {key: reason.strip() for key, _, reason in lines}


def results(root, name):
    """(meta, rows) of bench/results/NAME.tsv, or None"""
    path = os.path.join(root, RESULTS, name + '.tsv')
    return extras.read(path) if os.path.exists(path) else None


def version(root):
    return re.search(r"default_version\s*=\s*'([^']*)'", read(root, 'pg_sift.control')).group(1)


def check(root, strict=False, since=None):
    """(errors, notes): errors are (message, [case ids to measure]); the footers of the pages and the missing results are errors only when `strict`"""
    errors, notes = [], []
    allow, used = allow_list(root), set()
    since = since or last_tag(root)
    cases = read_registry(root)
    pages = {p: read(root, p) for p in PAGES if os.path.exists(os.path.join(root, p))}
    for c in (c for c in cases if c.gap):   # `-- gap:` cases run and assert their plan; the release has none
        notes.append(f'{c.id}: a known gap on the {c.gap[0]} lane, closes with {c.gap[1]}')
        if strict:
            errors.append((f'{c.id}: a known gap on the {c.gap[0]} lane ({c.gap[1]}); the release gate wants the flag gone', []))

    claims = []   # (text, the case that measures it or None)
    for sha, subject, in_changelog in landed(root, since):
        if f'commit {sha}' in allow:
            used.add(f'commit {sha}')
        elif not in_changelog:
            errors.append((f'{sha} {subject}: a landed commit that gives a speed has no CHANGELOG line and no `commit {sha}<TAB>reason` in {ALLOW}', []))
    for line in unreleased(root):
        waiver = next((k for k in allow if k in line), None)
        case = next((c for c in cases if c.changelog != '-' and c.changelog in line), None)
        if waiver:
            used.add(waiver)
        elif case:
            claims.append((line, case))
        else:
            errors.append((f'CHANGELOG.md: no case measures "{line[2:90]}" (add bench/cases/ID.sql and a row of bench/cases.tsv, or a reasoned line in {ALLOW})', []))
    errors += [(f'{ALLOW}: "{k}" matches no claim', []) for k in allow if k not in used and not k.startswith(FIGURE)]

    cur = results(root, 'cases')
    if cur is None:
        message = f'no {RESULTS}/cases.tsv: the rows of the cases cannot be checked in this tree'
        errors.append((message, [case.id for _, case in claims])) if strict else notes.append(message)
    for _, case in claims if cur else []:
        errors += rows_gaps(root, case, cur, strict)
    if claims and not any('<!-- bench:' in text for text in pages.values()):
        message = f'no generated block in {" or ".join(PAGES)}: the blocks of the cases cannot be checked in this tree'
        errors.append((message, [])) if strict else notes.append(message)
    else:
        for _, case in claims:
            if not any(f'<!-- bench:{case.block}:begin' in text for text in pages.values()):
                errors.append((f'{case.id}: its block "{case.block}" is in no page of {" or ".join(PAGES)}', []))

    footers = [(p, n) for p, text in pages.items() for n, l in enumerate(text.split('\n'), 1) if FOOTER.search(l)]
    if strict:
        errors += [(f'{p}:{n}: a table footer says "not measured" (the generator found no CPU or IO figure for a cell)', []) for p, n in footers]
    elif footers:
        notes.append(f'{len(footers)} table footers say "not measured" in {" and ".join(sorted({p for p, _ in footers}))}; --strict fails on them')
    return errors, notes


def rows_gaps(root, case, cur, strict):
    """the gaps of one case: a lane or peer without a measured row, rows of another version, rows older than the last change of the code (strict)"""
    errors, want, metric = [], version(root), case.metrics[0]

    def gaps(spec, meta, rows):
        found = [r for r in rows if r['case'] == case.id and r['arm'] == spec[1] and r['metric'] == metric and r['value'] not in ('NA', 'error', '')]
        if not found:
            errors.append((f'{case.id}: no measured {metric} row of {":".join(spec) if spec[0] != "cases" else spec[1]}', [case.id]))
        elif any(r['version'] != want for r in found):
            errors.append((f'{case.id}: the {spec[1]} row is stale (pg_sift {found[0]["version"]}, this tree is {want})', [case.id]))
        elif strict and not measured_after_code(root, meta):
            errors.append((f'{case.id}: the {spec[1]} row was measured at build {meta["build"]}, before the last change of src/ or sql/', [case.id]))

    for arm in LANES:
        gaps(('cases', arm), *cur)
    for peer in (p for p in case.peers.split(',') if p != '-'):
        name, _, arm = peer.rpartition(':')
        other = results(root, name or 'cases')
        if other is None:
            errors.append((f'{case.id}: its peer {peer} has no file {RESULTS}/{name or "cases"}.tsv', [case.id]))
        else:
            gaps((name or 'cases', arm), *other)
    return errors


def measured_after_code(root, meta):
    """whether the build of a results file contains the last commit that changed src/, sql/ or the control file (a file without a build is not judged)"""
    build, last = meta.get('build'), git(root, 'log', '-1', '--format=%H', '--', 'src', 'sql', 'pg_sift.control').strip()
    return not build or not last or subprocess.run(['git', '-C', root, 'merge-base', '--is-ancestor', last, build], capture_output=True).returncode == 0


def host_plan(root, since=None):
    """the commands that measure what `check` found missing: the cases without current rows, and the release pass when pages still say "not measured" """
    errors, _ = check(root, strict=True, since=since)
    ids = sorted({c for _, cs in errors for c in cs})
    cur = results(root, 'cases')
    rows = (cur[0].get('orders') if cur else None) or ROWS
    out = [f'bench/bench cases --cases {",".join(ids)} --rows {rows} --docs'] if ids else []
    if any('table footer' in m for m, _ in errors):
        out.append('bench/bench release --remeasure-all --budget 160')
    return out


def cmd_coverage(args):
    root = os.path.abspath(args.root or REPO)
    if args.host_plan:
        print('\n'.join(host_plan(root, args.since)))
        return 0
    errors, notes = check(root, args.strict, args.since)
    for n in notes:
        print('coverage: ' + n)
    for m, _ in errors:
        print(('coverage: ' if args.strict else 'coverage: warning: ') + m)
    gaps = sum(1 for c in read_registry(root) if c.gap)
    print('coverage: %d claims unmet, %d known gaps%s' % (len(errors), gaps, '' if args.strict else ' (a warning: --strict is the release gate)'))
    figures = claim_figures(root, [args.records] if args.records else None, args.strict)
    return 1 if (errors and args.strict) or figures else 0


def figure_lines(root):
    return [l for l in unreleased(root) if CLAIM.search(VERSION.sub('', l))]


def claim_figures(root, records=None, strict=False):
    """the problems of the figures the CHANGELOG states. Every `## Unreleased` line that states a figure has a row in bench/claims.tsv (its phrase, statement, size, arm, metric and the
    value it gives) or a `figure PHRASE<TAB>reason` line in ALLOW, and a waiver of a line that has a row, or of no line, fails. With RECORDS (files of steps/records) the rows whose statement and
    size are in them are checked against the median of the records (a figure off by more than its tolerance fails, naming the line and the run); the rows of other sizes wait for their run,
    and STRICT (the release gate) fails them"""
    sys.path.insert(0, os.path.join(HERE, 'records'))
    import claims
    import schema
    path = os.path.join(root, 'bench', 'claims.tsv')
    rows = claims.read_claims(path) if os.path.exists(path) else []
    waived = {k[len(FIGURE):]: why for k, why in allow_list(root).items() if k.startswith(FIGURE)}
    problems, used = [], set()
    for line in figure_lines(root):
        covered = any(c['phrase'] in line for c in rows)
        waiver = next((k for k in waived if k in line), None)
        if waiver and covered:
            problems.append(f'{ALLOW}: "{FIGURE}{waiver}" waives a line that has a row in bench/claims.tsv: remove the waiver')
        elif waiver:
            used.add(waiver)
        elif not covered:
            problems.append(f'CHANGELOG.md: "{line[2:90]}" states a figure and bench/claims.tsv has no row for it (a row: phrase, statement, size, arm, metric, value; or a reasoned `{FIGURE}PHRASE` line in {ALLOW})')
    problems += [f'{ALLOW}: "{FIGURE}{k}" matches no line that states a figure' for k in waived if k not in used and not any(k in l for l in figure_lines(root))]
    waiting = 0
    if records:
        recs = [r for f in records for r in schema.read(f)]
        have = {(r['statement'], r['rows']) for r in recs}
        here = [c for c in rows if (c['statement'], c['rows']) in have or strict]
        waiting = len(rows) - len(here)
        with open(os.path.join(root, 'CHANGELOG.md')) as f:
            problems += claims.verify(here, recs, f.read(), claims_file=path)
    for p in problems:
        print('claims: ' + p)
    print('claims: %d figure lines, %d rows in bench/claims.tsv, %d waived%s, %d problems' % (len(figure_lines(root)), len(rows), len(used), (', %d rows wait for a run at their size' % waiting) if records else '', len(problems)))
    return problems
