#!/usr/bin/env python3
"""Select the focused performance checks of a change from scripts/areas.tsv, the map `ci.sh affected` reads.

The map names, for each changed path, the areas of scripts/perf/gate.py it can move (`{perf_AREA}` rows). A path that no row maps to an area (a core file, a new
source directory) selects the smoke canaries, never the whole suite; a path no job builds or runs (documentation, tests, the gate itself) selects nothing, since both
revisions would build the same extension. Diffs use --no-renames so that the removed and the added path both count. The selection is static: no build, no PostgreSQL.
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AREAS = ('star', 'clickbench', 'coexist', 'jsonb', 'table', 'bm25', 'gpu', 'smoke')
OWNERS = tuple('perf_' + area for area in AREAS) + ('perf_plan', 'perf_scaling_ops', 'layout')
SCALING = {'perf_scaling_ops': 'check-scaling-ops.sh --sql-only', 'layout': 'tap-shell-scaling_slope'}
LANE_FLAGS = {'stock': (), 'stock-4t': ('--lane', 'stock-4t'), 'table': ('--table',), 'tuned': ('--lane', 'tuned'), 'gpu': ('--lane', 'gpu')}
SLICE_LANES = {'table': ('table',), 'gpu': ('gpu',)}
DEFAULT_LANES = ('stock', 'stock-4t')
AB_GROUPS = {'smoke': 'smoke', 'bm25': 'bm25', 'table': 'storage'}
AB_ORDER = ('smoke', 'bm25', 'agg', 'storage')


def owners_by_path(paths, map_path):
    out = subprocess.check_output(['bash', str(ROOT / 'scripts/areas.sh'), '--owners', str(map_path), *OWNERS, '--', *paths], input='', text=True)
    chunks, owners = [], set()
    for line in out.splitlines():
        if line == '--':
            chunks.append(owners)
            owners = set()
        else:
            owners.add(line)
    return chunks


def route(owners):
    if owners == {'-'}:
        return set(), set(), set()
    areas = {area for area in AREAS if 'perf_' + area in owners}
    slices = set(areas)
    if 'perf_plan' in owners:
        slices.add('smoke')
    elif not areas:
        areas = slices = {'smoke'}
    return areas, slices, {SCALING[owner] for owner in owners & SCALING.keys()}


def ordered(wanted, order):
    return [item for item in order if item in wanted]


def lanes_of(slices):
    return {lane for name in slices for lane in SLICE_LANES.get(name, DEFAULT_LANES)}


def select(paths, map_path, tier='pr'):
    if tier == 'nightly':
        areas = slices = set(AREAS) - {'smoke'}
        scaling, lanes = set(SCALING.values()), set(LANE_FLAGS)
    else:
        routes = [route(owners) for owners in owners_by_path(paths, map_path)]
        areas, slices, scaling = (set().union(*part) for part in zip(*routes)) if routes else (set(), set(), set())
        lanes = lanes_of(slices)
    ab = {AB_GROUPS.get(area, 'agg') for area in areas | {'smoke'}} if areas else set()
    return {'tier': tier, 'groups': ordered(areas, AREAS), 'ab_groups': ordered(ab, AB_ORDER), 'plangate_slices': ordered(slices, AREAS),
            'plangate_lanes': ordered(lanes, LANE_FLAGS), 'scaling_checks': sorted(scaling), 'run': bool(areas)}


def commands(selection):
    gate = ['python3', '-I', 'scripts/perf/gate.py']
    groups = [group for group in selection['groups'] if group != 'smoke']
    nightly = selection['tier'] == 'nightly'
    gate_runs = [gate + ['--all']] if nightly else ([gate + ['--areas', ','.join(groups)]] if groups else [])
    if 'smoke' in selection['groups']:
        gate_runs.append(gate + ['--smoke'])
    plan_runs = []
    for lane in selection['plangate_lanes']:
        slices = ['all'] if nightly else [name for name in selection['plangate_slices'] if lane in SLICE_LANES.get(name, DEFAULT_LANES)]
        if slices:
            plan_runs.append(['scripts/plangate.sh', '--runs', '0', *LANE_FLAGS[lane], '--slice', ','.join(slices)])
    return {'gate': gate_runs, 'plangate': plan_runs}


def changed_paths(root, baseline, candidate):
    raw = subprocess.check_output(['git', '-C', str(root), 'diff', '--name-only', '--no-renames', '-z', baseline, candidate, '--'])
    return [path.decode('utf-8', 'surrogateescape') for path in raw.split(b'\0') if path]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-ref')
    parser.add_argument('--candidate-ref')
    parser.add_argument('--files', nargs='+', help='the changed paths, instead of the diff of the two refs')
    parser.add_argument('--tier', choices=('pr', 'nightly'), default='pr')
    parser.add_argument('--output', type=Path)
    parser.add_argument('--run', choices=('gate', 'plangate'), help='run the selected canaries (gate) or plangate slices (plangate); the first failure ends the run')
    args = parser.parse_args()
    if args.files is None and not (args.baseline_ref and args.candidate_ref):
        parser.error('name --files, or --baseline-ref and --candidate-ref')
    paths = args.files if args.files is not None else changed_paths(ROOT, args.baseline_ref, args.candidate_ref)
    selection = select(paths, ROOT / 'scripts/areas.tsv', args.tier)
    result = {'baseline_ref': args.baseline_ref, 'candidate_ref': args.candidate_ref, 'paths': paths, **selection, 'commands': commands(selection)}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2) + '\n')
    for key in ('groups', 'ab_groups', 'plangate_slices', 'plangate_lanes', 'scaling_checks'):
        print(key + '=' + ','.join(result[key]))
    print('run=' + str(result['run']).lower())
    for argv in result['commands'][args.run] if args.run else ():
        returncode = subprocess.run(argv, cwd=ROOT).returncode
        if returncode:
            sys.exit(returncode)


if __name__ == '__main__':
    main()
