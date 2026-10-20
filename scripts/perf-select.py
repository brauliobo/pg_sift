#!/usr/bin/env python3
"""Select focused cases from the same dependency map as ci.sh affected.

Diffs use --no-renames so both the removed and added paths contribute. Every
non-documentation change includes smoke; unmapped paths select every group.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[1]
ALL = ('smoke', 'bm25', 'agg', 'storage')
# Column storage feeds table readers as well as aggregate pushdown.
OWNERS = {
    'bm25': ('bm25',),
    'col':  ('agg', 'storage'),
    'agg':  ('agg',),
    'tbl':  ('storage',),
}


def read_map(path):
    groups, rows = {}, []
    for line in path.read_text().splitlines():
        if not line or line.startswith('#'):
            continue
        key, value = line.split('\t', 1)
        if key.startswith('='):
            groups[key[1:]] = value
        else:
            rows.append((re.compile(key), value))
    return groups, rows


def job_names(root):
    plan = subprocess.check_output(
        ['bash', str(root / 'scripts/ci.sh'), '--plan', 'quick'],
        env={**os.environ, 'SIFT_CI_PG_MAJOR': '18',
             'SIFT_CI_JOBS': '', 'SIFT_CI_AFFECTED': '', 'SIFT_CI_SHARD': ''},
        text=True)
    return {line.split()[0] for line in plan.splitlines()} | {
        'installcheck-' + path.stem for path in (root / 'test/sql').glob('*.sql')}


def documentation(path):
    return path.startswith('docs/') or path.endswith('.md') or path in ('LICENSE', 'NOTICE')


def select(paths, groups, rows, jobs):
    picked = set()
    for path in paths:
        if documentation(path):
            continue
        picked.add('smoke')
        matches = [(pattern.search(path), value) for pattern, value in rows]
        matches = [(match, value) for match, value in matches if match]
        if not matches:
            picked.update(ALL)
        for match, value in matches:
            if value == '.' or '{perf}' in value:
                picked.update(ALL)
                continue
            if value == '-':
                continue
            # Resolve explicit owners directly: intersecting their job patterns
            # would falsely connect every owner through shared lifecycle tests.
            def expand(token):
                owner = token[1]
                if owner in OWNERS:
                    picked.update(OWNERS[owner])
                    return '(?!)'
                return '(' + groups[owner] + ')'

            expanded = re.sub(r'\{([^}]+)\}', expand, value)
            expanded = re.sub(r'\\([1-3])', lambda m: match[int(m[1])] or '', expanded)
            affected = [job for job in jobs if re.search(expanded, job)]
            for owner, benchmarks in OWNERS.items():
                if any(re.search(groups[owner], job) for job in affected):
                    picked.update(benchmarks)
    return [group for group in ALL if group in picked]


def changed_paths(root, baseline, candidate):
    raw = subprocess.check_output(['git', '-C', str(root), 'diff', '--name-only',
                                   '--no-renames', '-z', baseline, candidate, '--'])
    return [path.decode('utf-8', 'surrogateescape') for path in raw.split(b'\0') if path]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref', required=True)
    parser.add_argument('--candidate-ref', required=True)
    parser.add_argument('--tier', choices=('pr', 'nightly'), default='pr')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    paths = changed_paths(ROOT, args.baseline_ref, args.candidate_ref)
    groups, rows = read_map(ROOT / 'scripts/areas.tsv')
    selected = list(ALL) if args.tier == 'nightly' else select(paths, groups, rows, job_names(ROOT))
    result = {'baseline_ref': args.baseline_ref, 'candidate_ref': args.candidate_ref,
              'tier': args.tier, 'paths': paths, 'groups': selected}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('groups=' + ','.join(selected))
    print('run=' + str(bool(selected)).lower())


if __name__ == '__main__':
    main()
