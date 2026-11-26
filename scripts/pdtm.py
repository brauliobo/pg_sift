#!/usr/bin/env python3
"""PDTM: where the seconds of a test go, whether a change of a test or a tool made it faster, and which timing tables went stale.

Opt-in only: nothing runs it unless asked, so a normal run of the suites pays nothing for it.

  scripts/pdtm.py phase-split TARGET... [--json|--tsv]      the seconds of each phase of a test (scripts/wait-profile.sh --phase-split)
      test/sql/X.sql   statement by statement on the cluster of the PG* environment (eval "$(scripts/pgtest.sh env)"), in a database of its own
      test/t/X.sh      command by command from a bash xtrace (stdout and stderr of the test untouched)
      anything else    a command line: its wall and CPU seconds (with the processes it left behind) and its peak RSS
  scripts/pdtm.py profile JOB [--json|--tsv]                 phase-split of a job of the plan (scripts/ci.sh --profile JOB): installcheck-X, tap-shell-X, else the job run alone
  scripts/pdtm.py ab BASELINE CANDIDATE [--trials N] [--warmup K] [--normalize-re RE] [--json]   (scripts/wait-profile.sh --ab)
      the two commands interleaved A B B A ..., exit 2 when they ever differ in exit status or (normalized) stdout; the median ratio with a bootstrap 95% interval
  scripts/pdtm.py drift [--results FILE...] [--from-log FILE...] [--from-run RUN...] [--regress-logs DIR...] [--summaries FILE...] [--major 18]
                        [--threshold 0.25] [--update] [--json]                                        (scripts/regress-times.sh --drift)
      scripts/weights-quick.tsv against measured job seconds (the results of a kept run, `ci: PASS` log lines, or GitHub runs through scripts/ci-times.py),
      scripts/regress-times.tsv and SLOW_REGRESS against regression logs (median of the runs given), scripts/scratch.tsv against retained summaries;
      --update writes the tables that have a measured source (weights only from --from-run, the generator of that table)
  scripts/pdtm.py --selftest
"""
import argparse, hashlib, json, os, random, re, statistics, subprocess, sys, tempfile, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PHASES = ('boot', 'ddl_load', 'vac_chk', 'query_assert', 'sleep_poll', 'teardown', 'other')
SQL_RULES = (   # the first that matches a statement (comments removed) names its phase; none is query_assert
    ('sleep_poll', re.compile(r'\bpg_sleep\w*\s*\(|\bsift\._test_wait\w*', re.I)),
    ('vac_chk', re.compile(r'^\s*(VACUUM|ANALYZE|CHECKPOINT|REINDEX|CLUSTER)\b|\bsift\.\w*(compact|flush|maintain)\w*\s*\(', re.I)),
    ('boot', re.compile(r'^\s*CREATE\s+(EXTENSION|DATABASE)\b', re.I)),
    ('teardown', re.compile(r'^\s*DROP\b', re.I)),
    ('ddl_load', re.compile(r'^\s*(CREATE|ALTER|INSERT|COPY|UPDATE|DELETE|TRUNCATE|MERGE)\b|^\s*SELECT\b(?:(?!\bFROM\b).)*\bINTO\b', re.I | re.S)),
)
SQL_WORD = re.compile(r'\b(VACUUM|ANALYZE|CHECKPOINT|REINDEX|CLUSTER|CREATE|DROP|ALTER|INSERT|COPY|UPDATE|DELETE|TRUNCATE|SELECT|WITH|EXPLAIN|DO)\b', re.I)
SH_RULES = (
    ('sleep_poll', re.compile(r'^(sleep|wait_\w+|pg_isready|poll\w*|until|timeout)\b')),
    ('boot', re.compile(r'pgtest\.sh\s+(start|init|install|env)\b|\binitdb\b|\bpg_ctl\b.*\bstart\b|\bnode_(init|start)\b')),
    ('teardown', re.compile(r'\bpg_ctl\b.*\bstop\b|pgtest\.sh\s+(stop|destroy|clean)\b|^rm -r|^finish\b|\bnode_stop\b')),
)
PSQL = re.compile(r'\bpsql\b|^(q|sql|safe_psql|psql_\w+)\b')


def strip_sql_comments(s):
    return re.sub(r'/\*.*?\*/|--[^\n]*', '', s, flags=re.S)


def sql_phase(stmt):
    s = strip_sql_comments(stmt)
    return next((name for name, rx in SQL_RULES if rx.search(s)), 'query_assert')


def sh_phase(cmd):
    hit = next((name for name, rx in SH_RULES if rx.search(cmd)), None)
    if hit:
        return hit
    if PSQL.search(cmd):
        m = SQL_WORD.search(cmd)
        return sql_phase(cmd[m.start():]) if m else 'query_assert'
    return 'other'


def report(target, kind, rc, wall, cpu, events, rss_kb=None):
    """the phase-split record: EVENTS are (phase, seconds, location, text)"""
    phases = {p: 0.0 for p in PHASES}
    for phase, dur, _, _ in events:
        phases[phase] += dur
    top = sorted(events, key=lambda e: -e[1])[:5]
    return {'schema': 'pdtm.phase-split.v1', 'target': target, 'kind': kind, 'exit_code': rc, 'wall_s': round(wall, 3),
            'cpu_s': None if cpu is None else round(cpu, 3), 'cpu_eff_pct': None if cpu is None or not wall else round(100 * cpu / wall, 1),
            'max_rss_kb': rss_kb, 'phases_s': {p: round(v, 3) for p, v in phases.items()},
            'top_slowest': [{'rank': i + 1, 'phase': p, 'dur_s': round(d, 3), 'loc': loc, 'snippet': ' '.join(t.split())[:120]} for i, (p, d, loc, t) in enumerate(top)]}


def run_measured(argv, env=None, stdout=None, pass_fds=()):
    """(exit status, wall s, cpu s, peak rss kb) of ARGV under scripts/cpu_time.py: a postmaster it daemonized is reaped and counted too"""
    with tempfile.NamedTemporaryFile('r') as cpu:
        t = time.perf_counter()
        p = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'scripts', 'cpu_time.py'), cpu.name, *argv], env=env, stdout=stdout, pass_fds=pass_fds)
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
        wall = time.perf_counter() - t
        text = cpu.read().strip()
    return p.returncode, wall, float(text) if text else ru.ru_utime + ru.ru_stime, ru.ru_maxrss


# ---- SQL: psql echoes each line it reads (-a) and times each statement (\timing); the results go to /dev/null
def parse_psql(out, path):
    """(events, backend cpu s): the lines between two `Time:` lines are the statement that took that time"""
    lines = out.split('\n')
    begin = max(i for i, l in enumerate(lines) if l == 'PDTM-BEGIN')
    end = lines.index('PDTM-END', begin)
    events, buf, first, n, cpu = [], [], None, 0, None
    for l in lines[end + 1:]:
        if l.startswith('PDTMCPU '):
            f = l.rsplit(')', 1)[-1].split()
            cpu = (int(f[11]) + int(f[12])) / os.sysconf('SC_CLK_TCK') if len(f) > 12 else None
    for l in lines[begin + 1:end]:
        m = re.match(r'^Time: ([0-9.]+) ms', l)
        if m:
            text = '\n'.join(buf)
            events.append((sql_phase(text), float(m.group(1)) / 1000, '%s:%d' % (path, first or n), strip_sql_comments(text).strip()))
            buf, first = [], None
        else:
            n += 1
            buf.append(l)
            if first is None and l.strip() and not l.lstrip().startswith('--'):
                first = n
    return events, cpu


PROLOGUE = '\\timing on\nSELECT pg_backend_pid() AS pdtm_pid \\gset\n\\echo PDTM-BEGIN\n'
EPILOGUE = '\\echo PDTM-END\n\\echo PDTMCPU `cat /proc/:pdtm_pid/stat 2>/dev/null`\n'   # the CPU of the backend, while it lives (a server on this host)


def split_sql(path):
    db = 'pdtm_%d' % os.getpid()
    psql = ['psql', '-X', '-q', '-v', 'ON_ERROR_STOP=0']
    env = {**os.environ, 'LC_ALL': 'C'}   # `Time: 1.5 ms`, not a translation with a decimal comma
    t = time.perf_counter()
    r = subprocess.run([*psql, '-d', 'postgres', '-c', 'CREATE DATABASE ' + db], capture_output=True, text=True)
    if r.returncode:
        sys.exit('pdtm: no server to run %s on (the PG* environment: eval "$(scripts/pgtest.sh env)"): %s' % (path, r.stderr.strip()))
    boot = time.perf_counter() - t
    try:
        with tempfile.TemporaryFile('w+') as out, tempfile.NamedTemporaryFile('w', suffix='.sql') as pro, tempfile.NamedTemporaryFile('w', suffix='.sql') as epi:
            pro.write(PROLOGUE)
            epi.write(EPILOGUE)
            pro.flush()
            epi.flush()
            rc, wall, cpu, rss = run_measured([*psql, '-a', '-o', os.devnull, '-d', db, '-f', pro.name, '-f', path, '-f', epi.name], env=env, stdout=out)
            out.seek(0)
            events, server = parse_psql(out.read(), path)
    finally:
        t = time.perf_counter()
        subprocess.run([*psql, '-d', 'postgres', '-c', 'DROP DATABASE IF EXISTS ' + db], capture_output=True)
        down = time.perf_counter() - t
    events += [('boot', boot, 'pdtm', 'CREATE DATABASE'), ('teardown', down, 'pdtm', 'DROP DATABASE')]
    return report(path, 'sql', rc, wall + boot + down, cpu + (server or 0), events, rss)


# ---- TAP shell: a bash xtrace on its own fd; each traced command lasts until the next one starts
PS4 = '+\x01${EPOCHREALTIME}\x01${BASH_SOURCE##*/}:${LINENO}\x01'


def parse_xtrace(text, end):
    rows = []
    for l in text.split('\n'):
        m = re.match(r'^\++\x01(\d+)[.,](\d+)\x01([^\x01]*)\x01(.*)$', l)
        if m:
            rows.append([float(m.group(1) + '.' + m.group(2)), m.group(3), m.group(4)])
        elif rows:
            rows[-1][2] += '\n' + l
    return [(sh_phase(cmd), max(0.0, (rows[i + 1][0] if i + 1 < len(rows) else end) - t), loc, cmd) for i, (t, loc, cmd) in enumerate(rows)]


def split_sh(path):
    with tempfile.NamedTemporaryFile('w+') as trace:
        fd = os.open(trace.name, os.O_WRONLY | os.O_APPEND)
        try:
            env = {**os.environ, 'BASH_XTRACEFD': str(fd), 'PS4': PS4}
            rc, wall, cpu, rss = run_measured(['bash', '-x', path], env=env, pass_fds=(fd,))
        finally:
            os.close(fd)
        end = time.time()
        return report(path, 'tap', rc, wall, cpu, parse_xtrace(trace.read(), end), rss)


def split_cmd(argv):
    rc, wall, cpu, rss = run_measured(argv)
    return report(' '.join(argv), 'command', rc, wall, cpu, [('other', wall, '-', ' '.join(argv))], rss)


def phase_split(target):
    if len(target) == 1 and target[0].endswith('.sql'):
        return split_sql(target[0])
    if len(target) == 1 and target[0].endswith('.sh'):
        return split_sh(target[0])
    return split_cmd(target)


def job_target(job):
    """the test file of a job of the plan, else the job run alone by scripts/ci.sh"""
    for prefix, d, ext in (('installcheck-', 'test/sql', '.sql'), ('tap-shell-', 'test/t', '.sh')):
        path = os.path.join(d, job[len(prefix):] + ext)
        if job.startswith(prefix) and os.path.exists(os.path.join(ROOT, path)):
            return [path]
    return ['env', 'SIFT_CI_JOBS=^%s$' % re.escape(job), os.path.join(ROOT, 'scripts', 'ci.sh'), 'quick']


def tsv(r):
    top = r['top_slowest'][0] if r['top_slowest'] else None
    return '\t'.join(map(str, [r['target'], r['wall_s'], r['cpu_s'], r['cpu_eff_pct'], *r['phases_s'].values(), '%s:%s' % (top['loc'], top['dur_s']) if top else '-']))


def text_report(r):
    out = ['%s (%s, exit %s): %.2f s wall, %s s cpu' % (r['target'], r['kind'], r['exit_code'], r['wall_s'], r['cpu_s'])]
    out += ['  %-13s %8.3f s' % (p, v) for p, v in r['phases_s'].items() if v]
    out += ['  #%d %-12s %8.3f s %s %s' % (t['rank'], t['phase'], t['dur_s'], t['loc'], t['snippet']) for t in r['top_slowest']]
    return '\n'.join(out)


# ---- A/B
def run_once(cmd, norm):
    with tempfile.TemporaryFile() as out:
        t = time.perf_counter()
        p = subprocess.Popen(['bash', '-c', cmd], stdout=out, stderr=subprocess.DEVNULL)
        _, status, ru = os.wait4(p.pid, 0)
        p.returncode = os.waitstatus_to_exitcode(status)
        wall = time.perf_counter() - t
        out.seek(0)
        text = out.read().decode(errors='replace')
    return {'rc': p.returncode, 'sha': hashlib.sha256((norm.sub('', text) if norm else text).encode()).hexdigest(), 'wall': wall, 'cpu': ru.ru_utime + ru.ru_stime}


def quantile(v, q):
    s = sorted(v)
    k = (len(s) - 1) * q
    lo = int(k)
    return s[lo] + (s[min(lo + 1, len(s) - 1)] - s[lo]) * (k - lo)


def hodges_lehmann(a, b):
    """the median of every ratio A/B: above 1, B is faster"""
    return statistics.median(x / y for x in a for y in b if y > 0)


def bootstrap_ci(a, b, n=1000, seed=110):
    rng = random.Random(seed)
    est = sorted(hodges_lehmann(rng.choices(a, k=len(a)), rng.choices(b, k=len(b))) for _ in range(n))
    return est[int(0.025 * n)], est[int(0.975 * n) - 1]


def summarize(a, b):
    arm = lambda t: {'median_s': round(statistics.median(x['wall'] for x in t), 4), 'iqr_s': round(quantile([x['wall'] for x in t], .75) - quantile([x['wall'] for x in t], .25), 4),
                     'p95_s': round(quantile([x['wall'] for x in t], .95), 4), 'median_cpu_s': round(statistics.median(x['cpu'] for x in t), 4)}
    wa, wb = [x['wall'] for x in a], [x['wall'] for x in b]
    lo, hi = bootstrap_ci(wa, wb)
    verdict = 'FASTER' if lo > 1.05 else 'SLOWER' if hi < 0.95 else 'NOISE'
    return {'schema': 'pdtm.ab.v1', 'trials': len(a), 'baseline': arm(a), 'candidate': arm(b), 'speedup': round(statistics.median(wa) / statistics.median(wb), 3),
            'hl_ratio': round(hodges_lehmann(wa, wb), 3), 'ci95': [round(lo, 3), round(hi, 3)], 'verdict': verdict}


def ab(base, cand, trials, warmup, norm_re):
    norm = re.compile(norm_re) if norm_re else None
    a, b, diff = [], [], []
    for i in range(warmup + trials):
        pair = {}
        for arm in (('A', 'B') if i % 2 == 0 else ('B', 'A')):
            pair[arm] = run_once(base if arm == 'A' else cand, norm)
        if (pair['A']['rc'], pair['A']['sha']) != (pair['B']['rc'], pair['B']['sha']):
            diff.append('trial %d: baseline exit %d sha %s, candidate exit %d sha %s' % (i, pair['A']['rc'], pair['A']['sha'][:12], pair['B']['rc'], pair['B']['sha'][:12]))
        if i >= warmup:
            a.append(pair['A'])
            b.append(pair['B'])
    return summarize(a, b), diff


# ---- drift
def read_table(path, col=1):
    """{key: float} of a TSV with comments; the value is column COL"""
    out = {}
    for l in open(path):
        f = l.rstrip('\n').split('\t')
        if l.strip() and not l.startswith('#') and len(f) > col:
            out[f[0]] = float(f[col])
    return out


def job_secs(results=(), logs=()):
    """{job: median seconds} of the passing jobs: the results file of a kept run (job, _, status, wall, ...) and `ci: PASS ID N s` log lines"""
    per = {}
    for path in results:
        for l in open(path):
            f = l.split()
            if len(f) >= 4 and f[2] in ('PASS', 'pass', 'ok') and f[3].isdigit():
                per.setdefault(f[0], []).append(float(f[3]))
    for path in logs:
        for m in re.finditer(r'ci: PASS (\S+) (\d+) s', open(path, errors='replace').read()):
            per.setdefault(m.group(1), []).append(float(m.group(2)))
    return {k: statistics.median(v) for k, v in per.items()}


def compare(actual, table, threshold, plan=None):
    """missing (measured or planned, no row), stale (a row the plan has not), drift (|actual - row| / actual > THRESHOLD, largest gap first)"""
    known = set(plan) if plan is not None else set(actual)
    drift = sorted(((k, actual[k], table[k]) for k in actual if k in table and actual[k] and abs(actual[k] - table[k]) / actual[k] > threshold), key=lambda x: -abs(x[1] - x[2]))
    return {'missing': sorted(k for k in known | set(actual) if k not in table), 'stale': sorted(k for k in table if plan is not None and k not in known), 'drift': drift}


def plan_jobs(major):
    out = subprocess.run([os.path.join(ROOT, 'scripts', 'ci.sh'), '--plan', 'quick'], env={**os.environ, 'SIFT_CI_PG_MAJOR': major, 'FLAVOUR': major, 'SIFT_ALLOW_LOCAL_CI': '1'},
                         capture_output=True, text=True, check=True).stdout
    return [l.split()[0] for l in out.splitlines() if l.strip()]


def slow_regress():
    text = open(os.path.join(ROOT, 'scripts', 'ci', 'weights.sh')).read()
    slow = re.search(r'^SLOW_REGRESS="([^"]*)"', text, re.M).group(1).split()
    return slow, float(re.search(r'\bREGRESS_BATCH_SECS=(\d+)', text).group(1))


def regress_runs(dirs):
    """{test: median seconds over the runs}: each DIR is one run, its time the one scripts/regress-times.sh records"""
    per = {}
    for d in dirs:
        out = subprocess.run([os.path.join(ROOT, 'scripts', 'regress-times.sh'), d], capture_output=True, text=True, check=True).stdout
        for l in out.splitlines():
            if l and not l.startswith('#'):
                t, s = l.split('\t')
                per.setdefault(t, []).append(float(s))
    return {k: statistics.median(v) for k, v in per.items()}


def regress_drift(actual, table, tests, slow, batch_secs, threshold):
    r = compare(actual, table, threshold, plan=tests)
    times = {**table, **actual}
    r['stragglers'] = sorted((t for t, s in times.items() if s >= batch_secs / 2 and t not in slow), key=lambda t: -times[t])
    r['obsolete_slow'] = sorted(t for t in slow if t in times and times[t] < 5)
    return r


def write_regress(path, table, actual):
    head = [l for l in open(path) if l.startswith('#')]
    merged = {**table, **actual}
    with open(path + '.tmp', 'w') as f:
        f.writelines(head)
        f.writelines('%s\t%.2f\n' % (t, merged[t]) for t in sorted(merged))
    os.replace(path + '.tmp', path)


def drift(args):
    out, tsvdir = {}, os.path.join(ROOT, 'scripts')
    if args.results or args.from_log:
        col = {'17': 1, '18': 2, '19': 3, 'arm': 4}[args.major]
        out['weights'] = compare(job_secs(args.results, args.from_log), read_table(os.path.join(tsvdir, 'weights-quick.tsv'), col), args.threshold, plan=plan_jobs(args.major))
    if args.from_run:
        new = subprocess.run([sys.executable, '-I', os.path.join(tsvdir, 'ci-times.py'), '--weights', *args.from_run], capture_output=True, text=True, check=True).stdout
        with tempfile.NamedTemporaryFile('w', suffix='.tsv') as t:
            t.write(new)
            t.flush()
            col = {'17': 1, '18': 2, '19': 3, 'arm': 4}[args.major]
            out['weights_run'] = compare(read_table(t.name, col), read_table(os.path.join(tsvdir, 'weights-quick.tsv'), col), args.threshold)
        if args.update:
            open(os.path.join(tsvdir, 'weights-quick.tsv'), 'w').write(new)
    if args.regress_logs:
        path = os.path.join(tsvdir, 'regress-times.tsv')
        tests = [os.path.relpath(os.path.join(d, f), os.path.join(ROOT, 'test', 'sql'))[:-4] for d, _, fs in os.walk(os.path.join(ROOT, 'test', 'sql')) for f in fs if f.endswith('.sql')]
        actual, table = regress_runs(args.regress_logs), read_table(path)
        out['regress'] = regress_drift(actual, table, tests, *slow_regress(), args.threshold)
        if args.update:
            write_regress(path, table, actual)
    if args.summaries:
        new = subprocess.run([os.path.join(ROOT, 'scripts', 'ci.sh'), '--scratch', *args.summaries], capture_output=True, text=True, check=True).stdout
        with tempfile.NamedTemporaryFile('w', suffix='.tsv') as t:
            t.write(new)
            t.flush()
            out['scratch'] = compare(read_table(t.name), read_table(os.path.join(tsvdir, 'scratch.tsv')), args.threshold)
        if args.update:
            open(os.path.join(tsvdir, 'scratch.tsv'), 'w').write(new)
    return out


def drift_text(out):
    lines = []
    for table, r in out.items():
        lines.append('%s: %d missing, %d stale, %d drifted%s' % (table, len(r['missing']), len(r['stale']), len(r['drift']),
                     ''.join(', %d %s' % (len(r[k]), k.replace('_', ' ')) for k in ('stragglers', 'obsolete_slow') if k in r)))
        lines += ['  drift %-48s measured %8.2f table %8.2f' % d for d in r['drift'][:20]]
        lines += ['  %s %s' % (k.rstrip('s').replace('_', ' '), x) for k in ('missing', 'stale', 'stragglers', 'obsolete_slow') for x in r.get(k, [])[:20]]
    return '\n'.join(lines)


# ---- selftest: no server; the parsers, the classifiers, the statistics, the identity guard and the drift tables on made-up input
def selftest():
    for stmt, want in (('VACUUM (FREEZE) t;', 'vac_chk'), ('SELECT sift.col_compact(\'i\');', 'vac_chk'), ('-- load\nINSERT INTO t SELECT g FROM generate_series(1, 9) g;', 'ddl_load'),
                       ('SELECT 1 INTO x;', 'ddl_load'), ('SELECT a FROM t WHERE b = 1;', 'query_assert'), ('DO $$ BEGIN PERFORM pg_sleep(0.1); END $$;', 'sleep_poll'),
                       ('CREATE EXTENSION pg_sift;', 'boot'), ('DROP TABLE t;', 'teardown'), ('/* VACUUM */ EXPLAIN SELECT 1;', 'query_assert')):
        assert sql_phase(stmt) == want, (stmt, sql_phase(stmt), want)
    for cmd, want in (('sleep 0.2', 'sleep_poll'), ('scripts/pgtest.sh start', 'boot'), ('psql -c "VACUUM t"', 'vac_chk'), ('psql -Atc "SELECT 1"', 'query_assert'),
                      ('rm -rf /tmp/x', 'teardown'), ('echo hi', 'other'), ('q "INSERT INTO t VALUES (1)"', 'ddl_load')):
        assert sh_phase(cmd) == want, (cmd, sh_phase(cmd), want)
    out = 'noise\nPDTM-BEGIN\n-- a comment\nCREATE TABLE t (a int);\nTime: 1.500 ms\nINSERT INTO t\n  SELECT 1;\nTime: 2000.000 ms (00:02.000)\nPDTM-END\nPDTMCPU 7 (postgres) S 1 2 3 4 5 6 7 8 9 10 100 50 0\n'
    events, cpu = parse_psql(out, 'x.sql')
    assert [(p, d, loc) for p, d, loc, _ in events] == [('ddl_load', 0.0015, 'x.sql:2'), ('ddl_load', 2.0, 'x.sql:3')], events
    assert cpu == 150 / os.sysconf('SC_CLK_TCK'), cpu
    ev = parse_xtrace('+\x0110,0\x01t.sh:1\x01sleep 1\n++\x0111.5\x01common.bash:9\x01psql -c "VACUUM t"\nmore\n+\x0112.0\x01t.sh:2\x01echo done', 12.25)
    assert [(p, d, loc) for p, d, loc, _ in ev] == [('sleep_poll', 1.5, 't.sh:1'), ('vac_chk', 0.5, 'common.bash:9'), ('other', 0.25, 't.sh:2')], ev
    r = report('t', 'tap', 0, 2.25, 1.0, ev)
    assert r['phases_s']['sleep_poll'] == 1.5 and r['top_slowest'][0]['loc'] == 't.sh:1' and r['cpu_eff_pct'] == 44.4, r
    with tempfile.TemporaryDirectory() as d:   # a real trace: the sleep is the phase it is
        open(os.path.join(d, 't.sh'), 'w').write('x=1\nsleep 0.3\ntrue\n')
        r = split_sh(os.path.join(d, 't.sh'))
        assert r['exit_code'] == 0 and 0.25 <= r['phases_s']['sleep_poll'] < 2 and r['top_slowest'][0]['loc'] == 't.sh:2', r
    assert hodges_lehmann([2, 2, 2], [1, 1, 1]) == 2 and bootstrap_ci([2.0, 2.1, 1.9], [1.0, 1.05, 0.95]) == bootstrap_ci([2.0, 2.1, 1.9], [1.0, 1.05, 0.95])
    assert summarize(*[[{'wall': w, 'cpu': w} for w in v] for v in ([2.0, 2.1, 1.9, 2.0], [1.0, 1.05, 0.95, 1.0])])['verdict'] == 'FASTER'
    assert summarize(*[[{'wall': w, 'cpu': w} for w in v] for v in ([1.0, 1.1, 0.9, 1.0], [1.0, 1.1, 0.9, 1.0])])['verdict'] == 'NOISE'
    s, diff = ab('echo same; true', 'echo same', 2, 1, None)
    assert not diff and s['trials'] == 2, (s, diff)
    assert len(ab('echo a', 'echo b', 2, 0, None)[1]) == 2 and len(ab('exit 0', 'exit 1', 1, 0, None)[1]) == 1
    assert not ab('echo run 1', 'echo run 22', 1, 0, r'\d+')[1], 'the normalization does not strip what differs'
    r = compare({'a': 10, 'b': 100, 'c': 5}, {'a': 10, 'b': 50, 'z': 1}, 0.25, plan=['a', 'b', 'c', 'd'])
    assert r == {'missing': ['c', 'd'], 'stale': ['z'], 'drift': [('b', 100, 50)]}, r
    r = regress_drift({'x/slow': 40.0, 'y': 1.0}, {'y': 1.0, 'old/slow': 3.0}, ['x/slow', 'y', 'new'], ['old/slow'], 60, 0.25)
    assert r['missing'] == ['new', 'x/slow'] and r['stragglers'] == ['x/slow'] and r['obsolete_slow'] == ['old/slow'], r
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, 'r.tsv')
        open(p, 'w').write('# head\nb\t2.00\na\t1.00\n')
        write_regress(p, read_table(p), {'b': 3.0, 'c': 0.5})
        assert open(p).read() == '# head\na\t1.00\nb\t3.00\nc\t0.50\n', open(p).read()
        res = os.path.join(d, 'results')
        open(res, 'w').write('j1 x PASS 10 y 1 z w 1.0\nj1 x PASS 30 y 1 z w 1.0\nj2 x FAIL 99 y 1 z w 1.0\n')
        log = os.path.join(d, 'log')
        open(log, 'w').write('ci: PASS j1 20 s (lane1)\nci: FAIL j3 5 s (lane2)\n')
        assert job_secs([res], [log]) == {'j1': 20.0}, job_secs([res], [log])
    assert job_target('installcheck-no/such') == ['env', 'SIFT_CI_JOBS=^installcheck\\-no/such$', os.path.join(ROOT, 'scripts', 'ci.sh'), 'quick']
    sql = next(os.path.relpath(os.path.join(d, f), os.path.join(ROOT, 'test', 'sql')) for d, _, fs in os.walk(os.path.join(ROOT, 'test', 'sql')) for f in sorted(fs) if f.endswith('.sql'))
    assert job_target('installcheck-' + sql[:-4]) == [os.path.join('test/sql', sql)]
    slow, secs = slow_regress()
    assert slow and secs > 0
    print('pdtm selftest: ok (phases of SQL and shell, a real xtrace, A/B statistics and identity guard, drift of the weight, regression and scratch tables)')


def main():
    if sys.argv[1:] == ['--selftest']:
        return selftest()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='mode', required=True)
    for name in ('phase-split', 'profile'):
        p = sub.add_parser(name)
        p.add_argument('target', nargs='+' if name == 'phase-split' else 1)
        p.add_argument('--json', action='store_true')
        p.add_argument('--tsv', action='store_true')
    p = sub.add_parser('ab')
    p.add_argument('baseline')
    p.add_argument('candidate')
    p.add_argument('--trials', type=int, default=7)
    p.add_argument('--warmup', type=int, default=1)
    p.add_argument('--normalize-re')
    p.add_argument('--json', action='store_true')
    p = sub.add_parser('drift')
    for opt in ('--results', '--from-log', '--from-run', '--regress-logs', '--summaries'):
        p.add_argument(opt, nargs='+', default=[])
    p.add_argument('--major', default='18', choices=('17', '18', '19', 'arm'))
    p.add_argument('--threshold', type=float, default=0.25)
    p.add_argument('--update', action='store_true')
    p.add_argument('--json', action='store_true')
    a = ap.parse_args()
    if a.mode in ('phase-split', 'profile'):
        targets = [job_target(a.target[0])] if a.mode == 'profile' else ([a.target] if not all(t.endswith(('.sql', '.sh')) for t in a.target) else [[t] for t in a.target])
        rc = 0
        for t in targets:
            r = phase_split(t)
            rc = rc or r['exit_code']
            print(json.dumps(r, indent=2) if a.json else tsv(r) if a.tsv else text_report(r))
        return rc
    if a.mode == 'ab':
        s, diff = ab(a.baseline, a.candidate, a.trials, a.warmup, a.normalize_re)
        s['identity'] = diff or 'same exit status and output in every trial'
        print(json.dumps(s, indent=2) if a.json else '%s: speedup %.3fx, median ratio %.3f (95%% %.3f..%.3f); baseline %s s, candidate %s s; %s' % (
            s['verdict'], s['speedup'], s['hl_ratio'], *s['ci95'], s['baseline']['median_s'], s['candidate']['median_s'], '; '.join(diff) if diff else s['identity']))
        return 2 if diff else 0
    out = drift(a)
    if not out:
        ap.error('drift: name a source (--results, --from-log, --from-run, --regress-logs or --summaries)')
    print(json.dumps(out, indent=2) if a.json else drift_text(out))
    return 0


if __name__ == '__main__':
    sys.exit(main())
