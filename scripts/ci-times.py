#!/usr/bin/env python3
"""Where the time of a GitHub run of the workflow `ci` went.

  scripts/ci-times.py RUN...              one table per run: every job (queue, run, test step, lanes, lane minutes, the longest
                                          tests) and the slowest tests of the full tier
  scripts/ci-times.py --weights [--tier quick|full] RUN...    scripts/weights-*.tsv from the passing jobs of the runs (the median of the runs that passed a job:
                                          name three or more; a major that passed a job in no run gets the mean of its sibling slices)

The job logs come from `gh api` and are kept in --cache (default $TMPDIR/ci-times) so that a run is fetched once. The test times are the
`ci: STATUS ID N s (LANE)` lines scripts/ci.sh prints; a failed job is left out of the weights (--fail-fast ends it early).
"""
import argparse, json, os, re, statistics, subprocess, tempfile
from collections import defaultdict
from datetime import datetime

JOB_LINE = re.compile(r'ci: (PASS|FAIL) (\S+) (\d+) s \((\w+)\)')
WALL_LINE = re.compile(r'wall (\d+)s \((\d+) lanes\)')
PG_NAME = re.compile(r'PostgreSQL (\d+)')


def gh(*args):
    return subprocess.run(['gh', *args], check=True, capture_output=True, text=True).stdout


def ts(s):
    return datetime.fromisoformat(s.replace('Z', '+00:00')) if s and not s.startswith('0001') else None


def minutes(a, b):
    return (b - a).total_seconds() / 60 if a and b else 0.0


def run_jobs(run):
    return json.loads(gh('run', 'view', str(run), '--json', 'jobs,createdAt,event,headBranch,headSha,conclusion'))


def job_log(run, job_id, cache):
    path = os.path.join(cache, f'{run}-{job_id}.log')
    if not os.path.exists(path):
        out = gh('api', '--allow-escape-sequences', f'repos/{{owner}}/{{repo}}/actions/jobs/{job_id}/logs')
        with open(path, 'w') as f:
            f.write(re.sub(r'\x1b\[[0-9;]*m', '', out))
    return open(path).read()


def full_tier(job):
    return 'full tier' in job['name']


def quick_tier(job):
    # the shards of the quick tier (the assertions jobs run a few tests on a flavor of the server)
    return job['name'].startswith('PostgreSQL') and not re.search('full tier|assertions', job['name'])


def flavour(job):
    # the column of the weights a job feeds: its PostgreSQL major and arm64 each have their own timings
    return 'arm' if 'arm64' in job['name'] else int(PG_NAME.search(job['name'])[1])


FLAVOURS = (17, 18, 19, 'arm')
FALLBACK = {17: (18,), 18: (17,), 19: (18, 17), 'arm': (18, 17)}


def parse(log):
    jobs = [(m[2], m[1], int(m[3])) for m in JOB_LINE.finditer(log)]
    wall = WALL_LINE.search(log)
    return jobs, (int(wall[1]), int(wall[2])) if wall else (0, 0)


def step_minutes(job, prefix):
    for s in job['steps']:
        if s['name'].startswith(prefix):
            return minutes(ts(s['startedAt']), ts(s['completedAt']))
    return 0.0


def report(run, cache):
    d = run_jobs(run)
    t0 = ts(d['createdAt'])
    ends = [ts(j['completedAt']) for j in d['jobs'] if ts(j['completedAt'])]
    work = [j for j in d['jobs'] if ts(j['startedAt']) and ts(j['completedAt'])]
    total = sum(minutes(ts(j['startedAt']), ts(j['completedAt'])) for j in work)
    print(f"\n## Run {run}: {d['event']} {d['headBranch']} {d['headSha'][:9]}, {d['conclusion']}\n")
    print(f"Dispatch to the last job: {minutes(t0, max(ends)):.0f} min; runner time {total:.0f} min over {len(work)} jobs.\n")
    print('| job | queue | run | test step | lanes | tests | lane min | longest tests (s) | failed |')
    print('|---|--:|--:|--:|--:|--:|--:|---|---|')
    slow = []
    for j in sorted(work, key=lambda j: (not full_tier(j), j['name'])):
        s, e = ts(j['startedAt']), ts(j['completedAt'])
        row = f"| {j['name']} | {minutes(t0, s):.0f} | {minutes(s, e):.0f} |"
        if j['name'].startswith('PostgreSQL'):
            tests, (wall, lanes) = parse(job_log(run, j['databaseId'], cache))
            top = sorted(tests, key=lambda t: -t[2])[:3]
            slow += [(t[2], t[0], j['name']) for t in tests if t[1] == 'PASS']
            bad = [t[0] for t in tests if t[1] != 'PASS']
            step = step_minutes(j, 'Full test tier') or step_minutes(j, 'Quick test tier')
            row += f" {step:.0f} | {lanes} | {len(tests)} | {sum(t[2] for t in tests) / 60:.0f} | {', '.join(f'{t[0]} {t[2]}' for t in top)} | {', '.join(bad[:4])}{' ...' if len(bad) > 4 else ''} |"
        else:
            row += ' | | | | | |'
        print(row)
    print('\nSlowest tests of the run (lane seconds):\n')
    print('| s | test | job |\n|--:|---|---|')
    for secs, test, name in sorted(slow, reverse=True)[:15]:
        print(f'| {secs} | {test} | {name} |')


def med(v):
    return int(statistics.median(v))


def pick(per, test, flav):
    """The weight of a test on a flavour: the median of its passing times in the runs (one hosted run is noisy: the same test took 179 and 362 s beside different jobs),
    else the mean of the medians of its sibling slices (difftest-col-8 of difftest-col-N), else the median on the nearest flavour that has it, else the default 10."""
    mine = per[flav].get(test)
    if mine:
        return med(mine)
    family = re.sub(r'-\d+$', '', test)
    sib = [med(v) for t, v in per[flav].items() if t != test and re.sub(r'-\d+$', '', t) == family]
    if sib:
        return int(statistics.mean(sib))
    return next((med(per[o][test]) for o in FALLBACK[flav] if per[o].get(test)), 10)


def weights(runs, cache, floor, in_tier):
    per = {f: defaultdict(list) for f in FLAVOURS}
    for run in runs:
        for j in run_jobs(run)['jobs']:
            if not in_tier(j) or not PG_NAME.search(j['name']):
                continue
            for test, status, secs in parse(job_log(run, j['databaseId'], cache))[0]:
                if status == 'PASS':
                    per[flavour(j)][test].append(secs)
    print('# job\tPostgreSQL 17 lane seconds\t18\t19\tarm64 (18): the median of the passing jobs of runs ' + ' '.join(map(str, runs)) + ' (scripts/ci-times.py --weights; name three or more); the shards are cut by these, the other jobs weigh 10')
    rows = [(test, *(pick(per, test, f) for f in FLAVOURS)) for test in set().union(*(set(per[f]) for f in FLAVOURS))]
    for row in sorted(rows, key=lambda r: (-max(r[1:]), r[0])):
        if max(row[1:]) >= floor:
            print('\t'.join(map(str, row)))


def selftest():
    per = {f: defaultdict(list) for f in FLAVOURS}
    per[18]['t'] = [100, 110, 400]       # the newest run is the noisy one
    per[18]['s-1'] = [50, 60, 70]
    per[18]['s-2'] = [90]
    per[17]['o'] = [30, 50]
    ok = (pick(per, 't', 18) == 110 and pick(per, 's-3', 18) == int(statistics.mean([60, 90])) and pick(per, 'o', 18) == 40 and pick(per, 'none', 17) == 10
          and pick(per, 't', 'arm') == 110 and pick(per, 'o', 19) == 40)
    print('ci-times selftest: ' + ('the weight is the median of the runs, the mean of the sibling slices, the nearest flavour, else 10' if ok else 'FAIL: pick() differs'))
    return 0 if ok else 1


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('runs', nargs='*', type=int)
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--weights', action='store_true')
    ap.add_argument('--tier', choices=('quick', 'full'), default='full', help='the jobs the weights come from (--weights)')
    ap.add_argument('--floor', type=int, default=20, help='seconds under which a job weighs the default (--weights)')
    ap.add_argument('--cache', default=os.path.join(tempfile.gettempdir(), 'ci-times'))
    a = ap.parse_args()
    if a.selftest:
        raise SystemExit(selftest())
    ap.error('name the runs') if not a.runs else None
    os.makedirs(a.cache, exist_ok=True)
    if a.weights:
        weights(a.runs, a.cache, a.floor, quick_tier if a.tier == 'quick' else full_tier)
    else:
        for r in a.runs:
            report(r, a.cache)
