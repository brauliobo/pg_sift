#!/usr/bin/env python3
"""Where the time of a GitHub run of the workflow `ci` went.

  scripts/ci-times.py RUN...              one table per run: every job (queue, run, test step, lanes, lane minutes, the longest
                                          tests) and the slowest tests of the full tier
  scripts/ci-times.py --weights RUN...    scripts/weights-deep.tsv from the passing jobs of the runs (the newest run that passed a job wins:
                                          name the runs oldest first; a major that passed a job in no run gets the mean of its sibling slices)

The job logs come from `gh api` and are kept in --cache (default $TMPDIR/ci-times) so that a run is fetched once. The test times are the
`ci: ID STATUS Ns lane L` lines scripts/ci.sh prints; a failed job is left out of the weights (--fail-fast ends it early).
"""
import argparse, json, os, re, statistics, subprocess, tempfile
from collections import defaultdict
from datetime import datetime

JOB_LINE = re.compile(r'ci: (\S+)\s+(PASS|FAIL)\s+(\d+)s lane (\S+)')
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


def parse(log):
    jobs = [(m[1], m[2], int(m[3])) for m in JOB_LINE.finditer(log)]
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


def weights(runs, cache, floor):
    per = {17: defaultdict(list), 18: defaultdict(list)}
    for run in runs:
        for j in run_jobs(run)['jobs']:
            m = PG_NAME.search(j['name'])
            if not full_tier(j) or not m:
                continue
            for test, status, secs in parse(job_log(run, j['databaseId'], cache))[0]:
                if status == 'PASS':
                    per[17 if int(m[1]) == 17 else 18][test].append(secs)
    print('# job\tPostgreSQL 17 lane seconds\tPostgreSQL 18 lane seconds: the newest passing job of runs ' + ' '.join(map(str, runs)) + ' (scripts/ci-times.py --weights); the shards are cut by these, the other jobs weigh 10')
    def seconds(test, major):
        # the time of the newest run that passed it on this major, else the mean of its sibling slices (difftest-col-8 of difftest-col-N), else the other major's time
        mine = per[major].get(test)
        if mine:
            return mine[-1]
        family = re.sub(r'-\d+$', '', test)
        sib = [v[-1] for t, v in per[major].items() if t != test and re.sub(r'-\d+$', '', t) == family]
        other = per[35 - major].get(test)
        return int(statistics.mean(sib)) if sib else other[-1] if other else 10
    rows = [(test, seconds(test, 17), seconds(test, 18)) for test in set(per[17]) | set(per[18])]
    for test, a, b in sorted(rows, key=lambda r: (-max(r[1], r[2]), r[0])):
        if max(a, b) >= floor:
            print(f'{test}\t{a}\t{b}')


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('runs', nargs='+', type=int)
    ap.add_argument('--weights', action='store_true')
    ap.add_argument('--floor', type=int, default=20, help='seconds under which a job weighs the default (--weights)')
    ap.add_argument('--cache', default=os.path.join(tempfile.gettempdir(), 'ci-times'))
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    if a.weights:
        weights(a.runs, a.cache, a.floor)
    else:
        for r in a.runs:
            report(r, a.cache)
