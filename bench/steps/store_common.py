"""What the commands of the store benchmark share: the part files and their keys, the identity of the data and the host, the comparison of results (steps/store.py)."""
import benchlib
import json
import math
import os
import store_engine
import subprocess
import time
from stats import number
from store_engine import BASE, docker
from store_lib import NOISY, NULL, START_LOAD, START_PRESSURE, TOL, is_es, log, read


def csv_path(args):
    return os.path.join(args.data, f'orders-{args.rows}.csv')


def marker_path(data, container_, key):
    """the record of a load that finished: its rows and date, next to the CSV; the data volume of the container under the same key holds the data it describes"""
    return os.path.join(data, 'loaded', f'{container_}-{key}.json')


LOAD_CODE = None   # set in main(): the functions that decide what a loaded server holds


PARTS = {'run': 'warm', 'cold': 'cold', 'updates': 'updates'}   # the command and the part file it writes


def part_path(args, part):
    return os.path.join(args.out, f'store-{args.tier}-{part}.json')


def part_key(args, part):
    """what a part file's numbers depend on: the code, the host, the data and every setting of the command (a part with this key is not measured again by `resume`)"""
    return benchlib.key_of(dict(part=part, code=MEASURE_CODE, fp=host_fp()['id'], data=dataset_id(args), rows=args.rows, cpus=args.cpus, engines=args.engines, untimed=args.untimed,
                                queries=args.queries, runs=args.runs, warmups=args.warmups, cold=args.cold_runs, restarts=args.restart_runs, check=args.cold_check, rates=args.rates,
                                load=args.load_seconds, probes=args.probes, parallel=args.parallel), 16)


def part_done(args):
    """whether the part file of this command exists and holds this command's key: its cells were measured before and are kept"""
    path = part_path(args, PARTS[args.cmd])
    try:
        with open(path) as f:
            return json.load(f)['meta'].get('key') == part_key(args, PARTS[args.cmd])
    except (OSError, ValueError):
        return False


def write(args, rows, part, extra=None):
    os.makedirs(args.out, exist_ok=True)
    path = part_path(args, part)
    rows.rows = [r for r in rows.rows if r['engine'] not in args.untimed]   # the reference of the result checks is not a result of this pass
    util = store_engine.UTIL.report() if store_engine.UTIL else None
    if util and part != 'load' and util['wall_s'] >= 60 and util['timed_pct'] < 70:
        log(f'WARN: {part}: timed windows are {util["timed_pct"]}% of the {util["wall_s"]} s of the command (below 70%): the rest was waiting for restarts, cache drops, gates; '
            f'{util["low_busy_pct"]}% of the time fewer than {util["low_busy_below"]} cores were busy')
    meta = dict(key=part_key(args, part), date=time.strftime('%FT%TZ', time.gmtime()), baseline=BASE, utilization=util, parallel=getattr(args, 'parallel', 1), tier=args.tier, rows=args.rows, cpus=args.cpus, threads=args.threads, data_sha256=open(csv_path(args) + '.stamp').read().split()[2], loadavg=START_LOAD, loadavg_end=open('/proc/loadavg').read().split()[:3], pressure=START_PRESSURE, noisy=NOISY, **(extra or {}))
    benchlib.write_text(path, '{"meta": ' + json.dumps(meta) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows.rows) + '\n]}\n')
    log(f'written: {path}')


def rotated(items, i):
    k = i % len(items)
    return items[k:] + items[:k]


def difference(want, got, key, tol):
    """None when the results are equal, else what differs first: strings and integers exactly, numbers within tol (relative); rows sorted by the key columns when the query has them"""
    cut = lambda t: [[NULL if c == '' else c for c in l.split('\t')] for l in t.split('\n') if l != '']
    a, b = cut(want), cut(got)
    if key:
        a, b = sorted(a, key=lambda r: [r[k] for k in key]), sorted(b, key=lambda r: [r[k] for k in key])
    if len(a) != len(b):
        return f'{len(b)} rows, expected {len(a)}'
    for i, (ra, rb) in enumerate(zip(a, b)):
        if len(ra) != len(rb):
            return f'row {i}: {len(rb)} columns, expected {len(ra)}'
        for j, (x, y) in enumerate(zip(ra, rb)):
            nx, ny = number(x), number(y)
            if not (x == y or (nx is not None and ny is not None and abs(nx - ny) <= tol * max(1.0, abs(nx), abs(ny)))):
                return f'row {i} column {j + 1}: {y} expected {x} ({"|".join(ra)[:80]})'
    return None


def max_rel(want, got, key):
    """the largest relative difference between the numbers of two results (rows matched as in `difference`)"""
    cut = lambda t: sorted([l.split('\t') for l in t.split('\n') if l != ''], key=(lambda r: [r[k] for k in key]) if key else None)
    worst = 0.0
    for ra, rb in zip(cut(want), cut(got)):
        for x, y in zip(ra, rb):
            nx, ny = number(x), number(y)
            if nx is not None and ny is not None:
                worst = max(worst, abs(nx - ny) / max(1.0, abs(nx), abs(ny)))
    return worst


def tolerance(eid, st):
    return st[4] if is_es(eid) else TOL


def percentile(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, math.ceil(p / 100 * len(xs)) - 1)]


def host_fp():
    return benchlib.host_fingerprint(docker_version=docker('version', '-f', '{{.Server.Version}}').strip())


def host():
    """the fingerprint of this host (steps/benchlib.py) and the budgets of the run"""
    return dict(host_fp(), cpus=os.environ.get('STORE_CPUS', '?'), cpus_list=os.environ.get('STORE_CPUS_LIST', '?'), memory=os.environ.get('STORE_MEMORY', '?'))


def make_cache(args):
    return None if args.no_cache else benchlib.Cache(args.cache_dir, args.fingerprint, args.cache_max_age)


def image_ids(group):
    """(tag, image id) of the image of a server; pulled when it is not there, so that the key does not change with the first pull"""
    tag = os.environ[{'pg': 'STORE_PG_IMAGE', 'citus': 'STORE_CITUS_IMAGE', 'es': 'STORE_ES_IMAGE', 'ch': 'STORE_CH_IMAGE'}[group]]
    if subprocess.run(['docker', 'image', 'inspect', tag], capture_output=True).returncode:
        docker('pull', '-q', tag)
    return tag, docker('image', 'inspect', '-f', '{{.Id}}', tag).strip()


def dataset_id(args):
    """the seeded data is identified by what generates it: the rows, the generator and the image of the server it runs on"""
    return dict(rows=args.rows, generator=benchlib.key_of(read('gen.sql'), 12), image=image_ids('pg')[1])


MEASURE_CODE = None   # set in main(): the functions whose behaviour a cached number depends on
