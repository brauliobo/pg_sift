#!/usr/bin/env python3
"""The shared pieces of the benchmark steps (stdlib only; `python3 -I`): the host fingerprint, content keys, the cache of measured cells, the atomic JSON writer of result
files, the replacement of re-measured rows and the comparison of two result sets. steps/store.py imports it; a few commands are reachable from the shell:

    benchlib.py fingerprint [--id]        the fingerprint of this host (JSON; --id: its 12 hex digits)
    benchlib.py key TEXT...               the 16 hex digits of the content hash of the arguments (stage keys of bench/bench final)

Honesty rules of the cell cache (docs/BENCHMARKS.md, "Cached cells"): an entry is found only by a key that holds the engine's image, settings and statements, the dataset hash, the thread
budgets, the number of runs and the fingerprint of the host that measured it; one that was measured on another host is never used (`ForeignHost`), one older than the maximum age is
measured again, and what is read from the cache is labelled with its date, its key and its fingerprint wherever the numbers are published."""
import hashlib, json, os, sys, tempfile, time

EPOCH = 1    # bumped when the way a cell is measured changes so that older entries no longer describe what is measured now


class ForeignHost(Exception):
    """cached cells that were measured on another host than the one that publishes them"""


def canon(obj):
    return json.dumps(obj, sort_keys=True, separators=(',', ':'), default=str)


def key_of(obj, n=16):
    """the first n hex digits of the sha256 of a JSON-able value (text is hashed as it is)"""
    data = obj if isinstance(obj, str) else canon(obj)
    return hashlib.sha256(data.encode()).hexdigest()[:n]


def read_text(path):
    try:
        with open(path) as f:
            return f.read()
    except OSError:
        return ''


def host_fingerprint(read=read_text, docker_version='', kernel=None):
    """what a timing depends on besides the engines: CPU model, hardware threads, memory, kernel, docker and the cpufreq governor ("id": 12 hex digits of the rest).
    `read` returns the text of a file ('' when absent), so that tests can feed a fake /proc"""
    info = read('/proc/cpuinfo')
    cpu = next((l.split(':', 1)[1].strip() for l in info.splitlines() if l.startswith('model name')), '?')
    mem = next((int(l.split()[1]) for l in read('/proc/meminfo').splitlines() if l.startswith('MemTotal')), 0)
    fp = dict(cpu=cpu, threads=sum(1 for l in info.splitlines() if l.startswith('processor')), ram=f'{mem / 1048576:.0f} GiB',
              kernel=kernel if kernel is not None else os.uname().release, docker=docker_version,
              governor=read('/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor').strip())
    fp['id'] = key_of({k: v for k, v in fp.items() if k != 'id'}, 12)
    return fp


def same_host(meta_host, fp):
    """whether the `host` record of an older result file describes this host (the fields both have)"""
    keys = [k for k in ('cpu', 'threads', 'ram', 'kernel', 'docker') if meta_host.get(k) not in (None, '') and fp.get(k) not in (None, '')]
    return bool(keys) and all(str(meta_host[k]) == str(fp[k]) for k in keys)


def now_iso(t=None):
    return time.strftime('%FT%TZ', time.gmtime(t))


def parse_iso(s):
    return time.mktime(time.strptime(s, '%Y-%m-%dT%H:%M:%SZ')) - time.timezone


def write_json(path, obj):
    """atomically: a reader (or a crash) never sees half a file"""
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=os.path.dirname(os.path.abspath(path)), prefix='.tmp-', suffix='.json')
    with os.fdopen(fd, 'w') as f:
        json.dump(obj, f, indent=1, sort_keys=True)
        f.write('\n')
    os.replace(tmp, path)


class Cache:
    """measured cells of the engines that are not under test, one JSON file per (kind, name, key) under `root`: root/<kind>/<name>/<key>.json"""
    def __init__(self, root, fp, max_age_days=30, now=time.time):
        self.root, self.fp, self.max_age, self.now = root, fp, max_age_days * 86400, now

    def full(self, inputs):
        return {**inputs, 'fp': self.fp['id'], 'epoch': EPOCH}

    def path(self, kind, name, inputs):
        return os.path.join(self.root, kind, name, key_of(self.full(inputs)) + '.json')

    def lookup(self, kind, name, inputs):
        """(entry, '') on a hit; (None, why) on a miss. Raises ForeignHost for a file under this key that another host wrote"""
        p = self.path(kind, name, inputs)
        if not os.path.exists(p):
            other = self.foreign(kind, name, inputs)
            return None, 'not cached' + (f' ({len(other)} entries of another host fingerprint are not used)' if other else '')
        with open(p) as f:
            e = json.load(f)
        check_homogeneous([e], self.fp)
        if e.get('epoch') != EPOCH:
            return None, f'measured by an older method (epoch {e.get("epoch")})'
        age = self.now() - parse_iso(e['date'])
        if age > self.max_age:
            return None, f'measured {e["date"]}, older than {self.max_age / 86400:.0f} days'
        return e, ''

    def put(self, kind, name, inputs, payload):
        p = self.path(kind, name, inputs)
        entry = dict(kind=kind, name=name, key=os.path.basename(p)[:-5], inputs=self.full(inputs), fp=self.fp['id'], fingerprint=self.fp, date=now_iso(self.now()), epoch=EPOCH, **payload)
        write_json(p, entry)
        return p

    def foreign(self, kind, name, inputs):
        """the entries of this kind and name with the same inputs but another host fingerprint (fingerprint ids)"""
        d, want, out = os.path.join(self.root, kind, name), {k: v for k, v in inputs.items() if k not in ('fp', 'epoch')}, []
        for f in sorted(os.listdir(d)) if os.path.isdir(d) else []:
            try:
                with open(os.path.join(d, f)) as h:
                    e = json.load(h)
            except (OSError, ValueError):
                continue
            if e.get('fp') != self.fp['id'] and {k: v for k, v in e.get('inputs', {}).items() if k not in ('fp', 'epoch')} == want:
                out.append(e.get('fp'))
        return out


GROUP = {'es_cached': 'es', 'ch_mut': 'ch', 'ch_rmt': 'ch'}   # lanes that share a server (and a container) with another are cached with it


def group_of(eid):
    return GROUP.get(eid, eid)


def cacheable(eid):
    """pg_sift (`sift`, `sift_table`, the compression variants) is what the pass measures: it is never read from the cache"""
    return not eid.startswith('sift')


def plan_engines(lanes, hits, reference='pg'):
    """(run, untimed): the lanes a pass runs, given the groups read from the cache, and among them those that only serve as the reference of the result checks (PostgreSQL stays up
    and answers the first run of every question, so that what is measured now is compared with the same reference as when the cached cells were measured)"""
    measure = [e for e in lanes if group_of(e) not in hits]
    untimed = [reference] if measure and reference in lanes and reference not in measure else []
    return [e for e in lanes if e in measure or e in untimed], untimed


def check_homogeneous(entries, fp):
    """a pass never mixes cells of two hosts: every cached entry must carry this host's fingerprint"""
    for e in entries:
        if e.get('fp') != fp['id']:
            raise ForeignHost(f'cached cells of {e.get("name", "?")} (key {e.get("key", "?")}, {e.get("date", "?")}) were measured on another host (fingerprint {e.get("fp")}; this host {fp["id"]}): '
                              'they are not used; measure them here (--remeasure-all)')


def provenance(entries):
    """{name: {date, key, fp}} for the meta of a merged result file: what is printed next to a column that was read from the cache"""
    return {e['name']: dict(date=e['date'], key=e['key'], fp=e['fp'], kind=e['kind']) for e in entries}


def replace_rows(rows, fresh, cell=lambda r: (r['tier'], r['engine'], r['class'])):
    """rows without the cells that `fresh` re-measured, plus `fresh` (a NOISY cell measured again replaces the first measurement, never sits beside it)"""
    gone = {cell(r) for r in fresh}
    return [r for r in rows if cell(r) not in gone] + list(fresh)


def stable(a, b, tol=1.0, sep=1.5, pairs=None):
    """whether two result sets tell the same story. a, b: {(engine, query): median in ms}. For each query and pair of engines (`pairs`, default every pair) the ratio x / y of each set:
    the drift is the larger of ra / rb and rb / ra and must not exceed 1 + tol (default 2x); an order flip is counted only when the pair is separated by more than `sep` in both sets
    (closer pairs are ties). Returns {'queries': {q: {'pairs', 'flips', 'worst': (drift, x, y)}}, 'violations': [...], 'ok': bool}"""
    out, bad = {}, []
    engines = sorted({e for e, _ in a} & {e for e, _ in b})
    for q in sorted({q for _, q in a} & {q for _, q in b}):
        have = [e for e in engines if a.get((e, q)) and b.get((e, q))]
        info = dict(pairs=0, flips=0, worst=(1.0, '', ''))
        for x, y in pairs or [(x, y) for i, x in enumerate(have) for y in have[i + 1:]]:
            if x in have and y in have:
                ra, rb = a[x, q] / a[y, q], b[x, q] / b[y, q]
                drift = max(ra / rb, rb / ra)
                info['pairs'] += 1
                if drift > info['worst'][0]:
                    info['worst'] = (drift, x, y)
                if drift > 1 + tol:
                    bad.append((q, x, y, ra, rb, 'ratio'))
                if (ra < 1) != (rb < 1) and min(max(ra, 1 / ra), max(rb, 1 / rb)) > sep:
                    info['flips'] += 1
                    bad.append((q, x, y, ra, rb, 'order'))
        out[q] = info
    return dict(queries=out, violations=bad, ok=not bad)


def main(argv):
    if argv[:1] == ['fingerprint']:
        fp = host_fingerprint()
        print(fp['id'] if '--id' in argv else json.dumps(fp))
    elif argv[:1] == ['key'] and len(argv) > 1:
        print(key_of('\n'.join(argv[1:])))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
