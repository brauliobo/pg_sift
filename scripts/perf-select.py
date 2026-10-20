#!/usr/bin/env python3
"""Select focused cases from the same dependency map as ci.sh affected.

Diffs use --no-renames so both the removed and added paths contribute. Every
non-documentation change includes smoke; unmapped paths select every group.
"""
import argparse
import json
import os
from pathlib import Path
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


def select(paths, map_path, jobs):
    paths = [path for path in paths if not documentation(path)]
    if not paths:
        return []
    owners = subprocess.check_output(
        ['bash', str(ROOT / 'scripts/areas.sh'), '--owners', str(map_path),
         *OWNERS, '--', *paths], input=''.join(job+'\0' for job in sorted(jobs)), text=True).splitlines()
    picked = {'smoke'}
    for owner in owners:
        picked.update(ALL if owner=='*' else OWNERS[owner])
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
    selected = list(ALL) if args.tier == 'nightly' else select(paths, ROOT / 'scripts/areas.tsv', job_names(ROOT))
    result = {'baseline_ref': args.baseline_ref, 'candidate_ref': args.candidate_ref,
              'tier': args.tier, 'paths': paths, 'groups': selected}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print('groups=' + ','.join(selected))
    print('run=' + str(bool(selected)).lower())


if __name__ == '__main__':
    main()
