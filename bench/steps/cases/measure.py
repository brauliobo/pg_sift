# Timing a case: the interleaved rounds, the IO and instruction counts, the load measurement and the summary.
# Every answer is compared with the native one.

import os
import re
import signal
import statistics
import subprocess
import time

import iolib

from . import runcfg
from .runcfg import FORCED, HERE, IO_HOW, MIN_RUNS, SCAN, die, log
from .server import Fresh, IDS, build, equal, equal_ties, plan_note, plan_shape, plan_text, psql, server_session, tie_keys


def expand(sql, case):
    """`{ids:TABLE:N}` is the list of the ids of the rows of TABLE with n = N, in the statement: a literal IN list of 50,000 or 500,000 values is not a thing to write in a file"""
    def ids(m):
        key = (case.schema(), m.group(1), m.group(2))
        if key not in IDS:
            IDS[key] = psql("SELECT string_agg(id::text, ',' ORDER BY id) FROM %s.%s WHERE n = %s" % key)
        return IDS[key]
    return re.sub(r'\{ids:(\w+):(\d+)\}', ids, sql)


def tie_rows(case, native):
    """the rows of the ordering with the key values of a row (`--@ ties`, `{3}` the value of the third column of the row), from the lane of PostgreSQL's plan"""
    def rows(values):
        sql = re.sub(r'\{(\d+)\}', lambda m: values[int(m.group(1)) - 1].replace("'", "''"), case.sec['ties'])
        return native.run(sql)[1]
    return rows


def measure_query(case, lanes, rounds, runs, keep_plan=True):
    """rows of results: per lane the timed runs of the interleaved rounds, the answer of the warm-up compared with the native one; a plan other than the case's is an error
    unless keep_plan is false (the scale points: the planner's choice at each size is what the curve shows)"""
    ordered = case.opt.get('ordered', 'no')
    ties = tie_keys(ordered)
    sql = {lane: expand(case.statement(lane), case) for lane in lanes}
    notes, answers, times, errors, texts = {}, {}, {lane: [] for lane in lanes}, {}, {}
    for lane, s in lanes.items():
        text = texts[lane] = plan_text(s, sql[lane], lane != 'pg_duckdb')
        notes[lane] = plan_note(text)
        if lane == 'pg_duckdb' and 'DuckDBScan' not in text:
            errors[lane] = 'not supported: it runs on the PostgreSQL executor'
        ms, answers[lane], err, _ = s.run(sql[lane])   # the warm-up, whose answer is checked
        if err:
            errors.setdefault(lane, err[:110])
        if lane == 'sift':
            # the plan of the runs that are timed is the one after the first: where the first statement of a backend is planned another way (the cases cluster loads the
            # library at its start, so it is rare), the note says so
            again = plan_text(s, sql[lane])
            if plan_shape(again) != plan_shape(text):
                notes[lane] = '%s (the first plan of a backend was %s)' % (plan_note(again), notes[lane])
                text = texts[lane] = again
            if keep_plan and not re.search(case.plan, text, re.S):
                errors[lane] = 'PLAN LOST: expected /%s/, the plan is %s' % (case.plan, notes[lane])
            elif keep_plan and case.fallback and SCAN.search(text):
                errors[lane] = 'PLAN CHANGED: the planner chooses the scan now (%s): drop the fallback: of the plan column' % notes[lane]
            if case.fallback:
                notes['why'] = why(s, sql[lane])
    if FORCED[0] in texts and not SCAN.search(texts[FORCED[0]]) and FORCED[0] not in errors:
        notes[FORCED[0]] = 'no scan: ' + why(lanes[FORCED[0]], sql[FORCED[0]])   # the shape the scan cannot run: a feature gap, not a cost
    if 'native' in texts and 'sift' in texts and notes['sift'] == notes['native'] and plan_shape(texts['sift']) != plan_shape(texts['native']):
        notes['sift'] += ' (a different plan below it)'   # the first scan is the same: a block must not call two different plans "the same plan"
    ref = answers.get('native')
    for lane in lanes:
        if lane not in errors and lane != 'native' and ref is not None and not (equal_ties(ref, answers[lane], ties, tie_rows(case, lanes['native'])) if ties else equal(ref, answers[lane], ordered == 'yes')):
            errors[lane] = 'RESULT DIFFERS from native (%d rows against %d)' % (len(answers[lane]), len(ref))
    for r in range(rounds):
        order = [l for l in lanes if l not in errors]
        order = order[r % len(order):] + order[:r % len(order)] if order else order
        for lane in order:
            for _ in range(runs):
                if sum(times[lane]) / 1000 > runcfg.LANE_BUDGET and len(times[lane]) >= MIN_RUNS:   # a lane of seconds a run: the median of a few runs, not of fifteen
                    break
                times[lane].append(lanes[lane].run(sql[lane])[0])
    return times, notes, errors, texts


INDEX_IN_PLAN = re.compile(r'(?:Index(?: Only)? Scan(?: Backward)? using|Bitmap Index Scan on) (\w+)|Index: (\w+)')


def io_load(session, case, lane, sql, text, mode):
    """what a lane reads, whatever the storage (steps/iolib.py): the IO of the first run after the case's tables were emptied from shared buffers and the OS cache (cold), then of the run
    after it (warm), and the bytes of the indexes the plan reads (a btree, the column index of a scan). MODE warm (and off) skips the eviction: one warm run. Without pg_buffercache the one run is warm.
    Returns ([measurements], index names, index bytes)"""
    s = session.persistent() if isinstance(session, Fresh) else session
    try:
        tables = iolib.schema_tables(s, case.schema() + '_base' if lane == 'base' else case.schema())
        ios = [iolib.measure(s, sql, tables if mode == 'cold' else None)]
        if ios[0]['cache'] == 'cold':
            IO_HOW.add(ios[0]['how'])
            ios.append(iolib.measure(s, sql))
        elif mode == 'cold':
            log('%s on %s: warm only (%s)' % (case.id, lane, ios[0]['how']))
        names = sorted({a or b for a, b in INDEX_IN_PLAN.findall(text)})
        size = sum(int(s.run("SELECT pg_relation_size('%s')" % n)[1][0]) for n in names) if names else 0
        return ios, names, size
    except RuntimeError as e:   # a statement the lane cannot EXPLAIN or finish in the timeout has no IO rows; the run goes on
        log('%s on %s: no IO rows (%s)' % (case.id, lane, str(e)[:100]))
        return [], [], 0
    finally:
        if s is not session:
            s.close()


def why(session, sql):
    """sift.explain_why of the statement (planned, not run): the first reason each arm of the aggregate pushdown gave up"""
    out = session.run("SELECT arm || ' ' || slug || ': ' || reason FROM sift.explain_why($q$%s$q$) WHERE NOT used" % sql.rstrip(';'))[1]
    return '; '.join(out)[:300] or 'sift.explain_why names no reason'


PERF = os.path.join(HERE, 'perf.sh')


def minstr(session, sql, runs=3):
    """user instructions (millions) of the backend of the session for one run of the statement, median of `runs` (steps/perf.sh, perf stat on the backend with no parallel workers: the count does
    not depend on the load of the host); None when perf may not count them"""
    session.run('SET max_parallel_workers_per_gather = 0')
    pid = session.run('SELECT pg_backend_pid()')[1][0]
    session.run(sql)
    counts = []
    for _ in range(runs):
        p = subprocess.Popen(['bash', PERF, pid], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, start_new_session=True)
        time.sleep(1)   # perf.sh traps SIGTERM only once bash has started: on a loaded host 0.3 s was too short and the signal killed bash, leaving perf to count to the end of the backend
        session.run(sql)
        p.send_signal(signal.SIGTERM)
        try:
            fields = p.communicate(timeout=60)[0].split()   # a SIGTERM that lands before perf.sh traps it leaves perf counting for ever
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            return None
        if len(fields) != 3 or not fields[1].isdigit():
            return None
        counts.append(int(fields[1]) / 1e6)
    return statistics.median(counts)


def stats(ms):
    return dict(value='%.3f' % statistics.median(ms), min='%.3f' % min(ms), max='%.3f' % max(ms), n=len(ms))


def wal_lsn(s):
    return int(s.run("SELECT pg_wal_lsn_diff(pg_current_wal_lsn(), '0/0')::bigint")[1][0])


def cpu_ns(pid):
    with open('/proc/%s/schedstat' % pid) as f:
        return int(f.read().split()[0])


def measure_load(case, csv, rounds, rows, instr=False):
    """a load case: per lane and round one fresh table (`--@ prepare`, untimed, with {am} and {csv}), the statements of `--@ load` timed with its WAL and the backend's CPU; metrics wal_bytes (per row), ms, cpu_ms, and with INSTR the backend's user instructions of the whole load (minstr)"""
    lanes = {'sift': ('sift_table', []), 'native': ('heap', [])}
    for name, settings in case.variants:
        lanes[name] = ('sift_table', ['SET %s' % s for s in settings.split(';')])
    out, errors, plans = {lane: {'wal_bytes': [], 'ms': [], 'cpu_ms': [], 'io_extend_mb': [], 'io_write_mb': []} | ({'minstr': []} if instr else {}) for lane in lanes}, {}, {}
    for r in range(rounds):
        names = list(lanes)
        for lane in names[r % len(names):] + names[:r % len(names)]:
            am, sets = lanes[lane]
            s = server_session(600)
            for st in ['SET search_path = c_%s, public' % case.id] + sets:
                s.run(st)
            for stmt in re.split(r';[ \t]*\n', case.sec['prepare'].replace('{am}', am).replace('{csv}', csv)):   # the statements of the preparation, untimed (a VACUUM among them cannot share a transaction)
                err = s.run(stmt)[2]
                if err:
                    die('%s: the preparation failed on %s: %s' % (case.id, lane, err))
            pid = s.run('SELECT pg_backend_pid()')[1][0]
            s.run('CHECKPOINT')
            io0 = iolib.snapshot(s)
            lsn, cpu = wal_lsn(s), cpu_ns(pid)
            counter = subprocess.Popen(['bash', PERF, pid], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, start_new_session=True) if instr else None
            if counter:
                time.sleep(1)
            ms = 0
            for stmt in re.split(r';[ \t]*\n', case.sec.get('load_sift', case.sec['load']) if am == 'sift_table' else case.sec['load']):   # the statements of the load, their times added
                took, _, err, _ = s.run(stmt.replace('{csv}', csv))
                if err:
                    die('%s: the load failed on %s: %s' % (case.id, lane, err))
                ms += took
            cpu, lsn = cpu_ns(pid) - cpu, wal_lsn(s) - lsn
            if counter:
                counter.send_signal(signal.SIGTERM)
                fields = counter.communicate()[0].split()
                if len(fields) == 3 and fields[1].isdigit():
                    out[lane]['minstr'].append(int(fields[1]) / 1e6)
            for metric, mb in iolib.load_metrics(iolib.delta(io0, iolib.snapshot(s))).items():
                out[lane][metric].append(mb)
            checked = '\n'.join(s.run(case.sec['check'])[1]) if lane == 'sift' else ''
            plans[lane] = checked.replace('\n', ', ')
            if lane == 'sift' and not re.search(case.plan, checked, re.M):
                errors[lane] = 'PLAN LOST: the load did not write what /%s/ describes' % case.plan
            out[lane]['wal_bytes'].append(lsn / rows)
            out[lane]['ms'].append(ms)
            out[lane]['cpu_ms'].append(cpu / 1e6)
            s.close()
            build(case, rows)   # an empty schema again
    return out, plans, errors


def host():
    cpu = next((l.split(':', 1)[1].strip() for l in open('/proc/cpuinfo') if l.startswith('model name')), '?')
    mem = int(next(l.split()[1] for l in open('/proc/meminfo') if l.startswith('MemTotal'))) // 1048576
    return '%s, %d threads, %d GB memory' % (cpu, os.cpu_count(), mem)


def summarize(results, cases):
    """the table printed after a run and the loss lines: where sift is slower than native, plainly"""
    lines, losses = ['%-26s %-10s %10s %10s %8s  %s' % ('case', 'metric', 'native', 'sift', 'speedup', 'plan')], []
    for c, m in [(c, m) for c in cases for m in c.metrics]:
        cell = lambda arm: next((r for r in results if r['case'] == c.id and r['arm'] == arm and r['metric'] == m), None)  # noqa: E731
        n, s = cell('native'), cell('sift')
        if not (n and s and 'NA' not in (n['value'], s['value']) and 'error' not in (n['value'], s['value'])):
            lines.append('%-26s %-10s %10s %10s %8s  %s' % (c.id, m, n['value'] if n else '-', s['value'] if s else '-', '-', (s or {}).get('note', '')[:60]))
            continue
        if c.fallback:   # PostgreSQL's plan against itself has no ratio: the lane with the scan forced is the comparison
            f = cell(FORCED[0])
            ok = f and f['value'] not in ('NA', 'error', '')
            lines.append('%-26s %-10s %10s %10s %8s  %s' % (c.id, m, n['value'], f['value'] if ok else '-', '%.1fx' % (float(n['value']) / float(f['value'])) if ok and float(f['value']) else '-', 'scan forced: ' + f['note'][:46] if f else ''))
            continue
        speed = float(n['value']) / float(s['value']) if float(s['value']) else float('inf')
        lines.append('%-26s %-10s %10s %10s %7.1fx  %s' % (c.id, m, n['value'], s['value'], speed, s['note'][:60]))
        if speed < 0.95:
            losses.append('%s: sift %s %s against native %s (%.1fx slower)' % (c.id, s['value'], m, n['value'], 1 / speed))
    if losses:
        lines += ['', 'slower than native on %d of %d numbers:' % (len(losses), sum(len(c.metrics) for c in cases))] + ['  ' + l for l in losses]
    return '\n'.join(lines)
