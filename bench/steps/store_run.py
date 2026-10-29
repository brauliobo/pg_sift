"""The orders benchmark's measurements: the warm and cold latency of the questions and the interference check (steps/store.py run, cold, interference)."""
import benchlib
import functools
import os
import statistics
import sys
import threading
import time
from store_common import difference, host_fp, max_rel, percentile, rotated, tolerance, write
from store_data import Rows, effective, servers
from stats import summary
from store_engine import Client, Window, ask, docker, gate, io_read, psql_value, restart
from store_lib import NOISY, PG_ENGINES, container, is_ch, load_queries, log


def measure_query(q, clients, args, rows):
    """the cell of one question on every engine, in a window whose external noise is recorded; (the results that differ, why the window was NOISY)"""
    for c in clients.values():
        c.busy = c.cpu = 0.0
        c.cpus = []
    with Window() as w:
        stop = run_query(q, clients, args, rows)
    for e, c in clients.items():   # the cores the container used while this client's requests ran: its CPU over the wall time of those requests (not the window: the other engines are asked in it)
        cores = c.cpu / c.busy if c.busy and e not in args.untimed else None
        quota = args.cpus * 1.05
        if cores is not None and cores > quota:
            log(f'{q.id} {e}: {cores:.2f} cores in the requests, more than the quota of {args.cpus}: not recorded')
        elif cores is not None:
            rows.add(e, q.id, 'warm', 'cpu_cores', 'cores', value=round(cores, 2))
    return stop, w.record(rows, q.id, 'warm', w.judge({container(e) for e in clients}, io=True))


def sample(c, st, times):
    """one timed request: its milliseconds go to times, the container's CPU milliseconds of it to c.cpus"""
    times.append(c.run(st)[0])
    c.cpus.append(c.cpu_ms)


def one_container(lanes, stmts, clients, args, times):
    """the untimed warm-ups, then the timed runs of the lanes of one container, the first lane rotating every run (the lanes of one container share its CPUs, so they never run together)"""
    for _ in range(args.warmups - 1):
        for e in lanes:
            clients[e].run(stmts[e])
    for i in range(args.runs):
        for e in rotated(lanes, i):
            sample(clients[e], stmts[e], times[e])


def time_cell(live, stmts, clients, args):
    """the timed runs of one question: one engine at a time, the engines interleaved run by run (the method of the published tables), or with --parallel-engines N the containers in
    waves of N at the same time, each on cores of its own (time_wave)"""
    times = {e: [] for e in live}
    boxes = list(dict.fromkeys(container(e) for e in live))
    if args.parallel <= 1 or len(boxes) < 2:
        for _ in range(args.warmups - 1):
            for e in live:
                clients[e].run(stmts[e])
        for i in range(args.runs):
            for e in rotated(live, i):
                sample(clients[e], stmts[e], times[e])
        return times
    cores = physical_cores()
    n = min(args.parallel, benchlib.max_parallel(cores, args.cpus))
    boxes = rotated(boxes, args.cell)   # which container is in which wave moves from question to question
    for w in range(0, len(boxes), n):
        time_wave(boxes[w:w + n], live, stmts, clients, args, times, cores, args.cell + w)
    return times


@functools.lru_cache(maxsize=None)
def physical_cores():
    return benchlib.physical_cores()


def pin(box, cores):
    """docker --cpuset-cpus: the container runs on these CPUs only; None frees it again (all CPUs of the host)"""
    docker('update', '--cpuset-cpus', ','.join(map(str, cores)) if cores else f'0-{os.cpu_count() - 1}', box)


def time_wave(boxes, live, stmts, clients, args, times, cores, rotation):
    """the containers of a wave run their timed requests at the same time, each on `--cpus` physical cores of its own (equal sets, moved round the cores from wave to wave so that no engine
    always gets the same ones; the other container threads of the host are not pinned). The cores are freed again afterwards"""
    sets, errors = benchlib.cpuset_plan(cores, args.cpus, len(boxes), rotation), []

    def work(box):
        try:
            one_container([e for e in live if container(e) == box], stmts, clients, args, times)
        except Exception as x:
            errors.append(x)

    try:
        for box, s in zip(boxes, sets):
            pin(box, s)
        threads = [threading.Thread(target=work, args=(box,)) for box in boxes]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
    finally:
        for box in boxes:
            pin(box, None)
    if errors:
        raise errors[0]


def run_query(q, clients, args, rows):
    stmts = {e: q.statement(e, args.rows, e == 'es_cached') for e in clients}
    out, bad = {}, {}
    for e, c in clients.items():   # the first run of each engine: its result is checked, the plan is read (an untimed engine is only the reference: no rows of it)
        try:
            out[e] = c.run(stmts[e], True)[1]
            plan = c.explain(stmts[e])
            if plan is not None and e not in args.untimed:
                rows.add(e, q.id, '-', 'plan', 'node', value=plan)
        except Exception as x:
            bad[e] = str(x)
            log(f'{q.id} {e}: {x}')
    ref = out.get('pg', next(iter(out.values()), None))
    stop = []
    for e in out:
        if e == 'pg' or ref is None:
            verdict = 'reference' if e == 'pg' else 'unchecked'
        else:
            d = difference(ref, out[e], q.key, tolerance(e, stmts[e]))
            verdict = 'ok' if d is None else 'DIFF: ' + d
            if d is None:
                rows.add(e, q.id, '-', 'rel_diff', 'fraction', value=float(f'{max_rel(ref, out[e], q.key):.3g}'))
            if d is not None:
                stop.append(f'{q.id} {q.title}: {e} differs from PostgreSQL: {d}')
        if e not in args.untimed:
            rows.add(e, q.id, '-', 'result', 'check', value=verdict)
    for e, msg in bad.items():
        rows.add(e, q.id, '-', 'result', 'check', value='error: ' + msg)
    live = [e for e in clients if e in out and e not in args.untimed]
    times = time_cell(live, stmts, clients, args)
    for e in live:
        rows.add(e, q.id, 'warm', 'latency_ms', 'ms', st=summary(times[e]))
        rows.add(e, q.id, 'warm', 'cpu_ms', 'ms', st=summary(clients[e].cpus))
        mb = clients[e].read_mb(stmts[e])
        rows.add(e, q.id, 'warm', 'read_mb', 'MB', **({'value': mb} if isinstance(mb, str) else {'st': summary([mb])}))
    log(f'{q.id}: ' + ' '.join(f'{e} {sorted(times[e])[len(times[e]) // 2]:.3g}' for e in live))
    return stop


def cmd_run(args):
    rows = Rows(args.tier)
    queries = load_queries(args.queries)
    args.threads = effective(args)
    clients = {e: Client(e) for e in args.engines}
    problems = []
    log(f'{len(queries)} queries, engines {" ".join(args.engines)}, {args.runs} runs after {args.warmups} warm-up(s), thread budget {args.cpus}')
    gate('run', args.engines)
    noisy, again = {}, []
    try:
        for i, q in enumerate(queries):
            args.cell = i
            stop, why = measure_query(q, clients, args, rows)
            problems += stop
            if why:
                noisy[q.id] = why
            if problems and not args.keep_going:
                break
        for q in [q for q in queries if q.id in noisy and not problems][:args.max_rerun]:   # a NOISY cell is measured again once, at the end of the stage, not waited on
            log(f'{q.id}: NOISY ({noisy[q.id]}), measured again')
            rows.rows = [r for r in rows.rows if r['class'] != q.id]
            args.cell = queries.index(q) + 1   # another wave order than the first time
            stop, why = measure_query(q, clients, args, rows)
            problems += stop
            again.append(q.id)
            if why:
                NOISY.append(f'{q.id}: {why}')
    finally:
        for c in clients.values():
            c.close()
        write(args, rows, 'warm', {'servers': servers(args.engines), 'runs': args.runs, 'warmups': args.warmups, 'rerun': again})
    for p in problems:
        print('DIFFERENT RESULT: ' + p, file=sys.stderr)
    return 3 if problems else 0


COLD_HOW = {'es': 'restart'}   # emptying the memory of Elasticsearch is not as cold as a restart, so its cold cells restart the container


def evict(e):
    """what a restart empties, without one: the memory the server keeps its data and caches in. The operating system's page cache stays, as after a restart (dropping it needs root)"""
    if e in PG_ENGINES:   # every shared buffer, from the maintenance database (a new backend starts each cell: no relation, catalog or plan caches)
        psql_value('pg' if e == 'pg' else e, 'SELECT pg_buffercache_evict_all()', 'postgres')
    elif is_ch(e):        # the mark, uncompressed-block, primary-key and skipping-index caches; the parts stay attached, as after a restart they are loaded again
        for what in ('MARK CACHE', 'UNCOMPRESSED CACHE', 'PRIMARY INDEX CACHE', 'INDEX MARK CACHE', 'INDEX UNCOMPRESSED CACHE', 'QUERY CACHE'):
            ask('ch', 'POST', '/', f'SYSTEM DROP {what}'.encode())
    else:                 # the node's caches, and the shards' readers (terms dictionaries, doc value and points structures) closed and opened again
        ask('es', 'POST', '/orders/_cache/clear')
        ask('es', 'POST', '/orders/_close')
        ask('es', 'POST', '/orders/_open?wait_for_active_shards=all')
        ask('es', 'GET', '/_cluster/health/orders?wait_for_status=green&timeout=60s')


def cold_cell(e, q, stmt, args, how='evict'):
    """the first run of a question after the server's memory was emptied (`evict`: its caches and buffers, `--cold-runs` times; `restart`: the container, `--restart-runs` times:
    a restart takes seconds, so it has a count of its own), each on a new connection: (the times, the windows of the runs added up, the container's CPU milliseconds of each run and the MB read
    past the engine's caches); no times when the engine fails. The MB: one more first run after the emptying (Client.read_mb), or the container's disk reads of the run itself after a restart"""
    ts, w, cpus, mbs = [], Window(), [], []
    for _ in range(args.restart_runs if how == 'restart' else args.cold_runs):
        c = None
        try:
            with Window() as x:   # the emptying and the first run: the request alone is a few milliseconds, too short for the host's counters
                restart(e) if how == 'restart' else evict(e)
                c = Client(e)
                disk = io_read(c.cg)
                ts.append(c.run(stmt)[0])
                cpus.append(c.cpu_ms)
                disk = (io_read(c.cg) - disk) / 2**20
            w.add(x)
            if how == 'restart':
                mbs.append(disk)
            else:
                evict(e)
                c.close()
                c = Client(e)
                mbs.append(c.read_mb(stmt, cold=True))
        except RuntimeError as err:   # an error is a result, as in the warm run
            log(f'cold {e} {q.id}: {err}')
            return None, w, [], []
        finally:
            if c:
                c.close()
    return ts, w, cpus, mbs


def med(xs):
    return sorted(xs)[len(xs) // 2]


def cold_check(e, q, stmt, how, args, rows, cache):
    """the check that emptying the memory is the cold of a restart: the same question the other way. It is not run unless asked for (--cold-check), and its result is kept in the cache once
    per version of the engine (the image the container runs): (milliseconds of the other way, the date it was measured when it was read from the cache)"""
    other = 'restart' if how == 'evict' else 'evict'
    inputs = dict(engine=e, image=docker('inspect', '-f', '{{.Image}}', container(e)).strip(), rows=args.rows, cpus=args.cpus, question=q.id, how=other, runs=args.restart_runs if other == 'restart' else args.cold_runs)
    hit, why = cache.lookup('check', e, inputs) if cache else (None, '')
    if hit:
        rows.rows += hit['rows']
        return med([r for r in hit['rows'] if r['metric'] == 'latency_ms'][0]['values']), hit['date']
    ts = cold_cell(e, q, stmt, args, other)[0]
    if not ts:
        return None, None
    mine = Rows(args.tier)
    mine.add(e, q.id, 'cold-alt', 'latency_ms', 'ms', st=summary(ts))
    rows.rows += mine.rows
    if cache:
        cache.put('check', e, inputs, dict(rows=mine.rows))
    return med(ts), None


def cmd_cold(args):
    rows = Rows(args.tier)
    queries = load_queries(args.queries)
    engines = [e for e in args.engines if e != 'es_cached' and e not in args.untimed]
    stmts = {(q.id, e): q.statement(e, args.rows) for q in queries for e in engines}
    how = {e: COLD_HOW.get(e, 'evict') for e in engines}
    effective(args)
    for e in engines:   # one restart of each server, then every cell empties its memory without one (a server whose cells restart it needs no restart before)
        if how[e] != 'restart':
            restart(e)
    for e in engines:
        if e in PG_ENGINES:
            psql_value(e, 'CREATE EXTENSION IF NOT EXISTS pg_buffercache', 'postgres')
    gate('cold', args.engines)
    cells = [(e, q) for e in engines for q in queries]
    got, again = {}, []
    for e in engines:
        rows.add(e, '-', 'cold', 'cold_how', 'text', value=how[e])
    for e, q in cells:
        got[e, q.id] = cold_cell(e, q, stmts[q.id, e], args, how[e])
        if got[e, q.id][0]:
            log(f'cold {e} {q.id}: {med(got[e, q.id][0]):.3g} ms')
    for e, q in cells:   # a NOISY cell is measured again once, at the end of the stage, not waited on (at most --max-rerun of them: a restart cell takes a minute)
        why = got[e, q.id][1].judge({container(e)})
        if got[e, q.id][0] and why and len(again) < args.max_rerun:
            log(f'cold {e} {q.id}: NOISY ({why}), measured again')
            got[e, q.id] = cold_cell(e, q, stmts[q.id, e], args, how[e])
            again.append(f'{e} {q.id}')
    for e, q in cells:
        ts, w, cpus, mbs = got[e, q.id]
        if not ts:
            continue
        why = w.record(rows, q.id, f'cold-{e}', w.judge({container(e)}))
        if why:
            NOISY.append(f'cold {e} {q.id}: {why}')
        rows.add(e, q.id, 'cold', 'latency_ms', 'ms', st=summary(ts))
        rows.add(e, q.id, 'cold', 'cpu_ms', 'ms', st=summary(cpus))
        rows.add(e, q.id, 'cold', 'read_mb', 'MB', st=summary(mbs))
    checked = {}
    for q in [q for q in queries if q.id in args.cold_check]:
        for e in engines:
            alt, date = cold_check(e, q, stmts[q.id, e], how[e], args, rows, args.cache)
            if alt is not None:
                checked[f'{e} {q.id}'] = date or 'measured'
                log(f'cold check {e} {q.id}: {alt:.3g} ms ({"restart" if how[e] == "evict" else "evict"}{", from the cache " + date if date else ""}), {med(got[e, q.id][0]):.3g} ms ({how[e]})')
    args.threads = effective(args)   # after the restarts: the settings are the container's, not the process's
    write(args, rows, 'cold', {'cold_runs': args.cold_runs, 'restart_runs': args.restart_runs, 'rerun': again, 'cold_check': args.cold_check, 'cold_check_dates': checked})
    return 0


def cmd_interference(args):
    """Does running engines at the same time change what one of them measures? The requests of the target are timed (1) unpinned and alone, the method of the published tables, (2) pinned to
    `--cpus` physical cores of its own and alone, and (3) pinned while `--concurrent` other containers loop the same question on disjoint cores of their own. Per question the median and p95
    of each, and the change of (3) against (2): what the neighbours' memory bandwidth and L3 traffic cost"""
    target, queries = args.target, load_queries(args.queries)
    others = [e for e in args.engines if container(e) != container(target) and e != 'es_cached'][:args.concurrent]
    boxes, cores = [container(target)] + [container(e) for e in others], physical_cores()
    clients = {e: Client(e) for e in [target] + others}
    effective(args)
    gate('interference', [target] + others)
    out = []

    def sample(stmt):
        for _ in range(args.warmups):
            clients[target].run(stmt)
        return [clients[target].run(stmt)[0] for _ in range(args.samples)]

    stats = lambda xs: dict(median=statistics.median(xs), p95=percentile(xs, 95), n=len(xs))
    for qi, q in enumerate(queries):
        stmts = {e: q.statement(e, args.rows) for e in clients}
        for e in clients:
            clients[e].run(stmts[e], True)
        for b in boxes:
            pin(b, None)
        free = stats(sample(stmts[target]))
        for b, s in zip(boxes, benchlib.cpuset_plan(cores, args.cpus, len(boxes), qi)):   # the target's cores move from question to question
            pin(b, s)
        stop = threading.Event()
        loops = [threading.Thread(target=lambda e=e: [clients[e].run(stmts[e]) for _ in iter(stop.is_set, True)]) for e in others]
        try:
            alone = stats(sample(stmts[target]))
            for t in loops:
                t.start()
            time.sleep(0.5)
            together = stats(sample(stmts[target]))
        finally:
            stop.set()
            for t in loops:
                if t.is_alive():
                    t.join()
            for b in boxes:
                pin(b, None)
        out.append(dict(question=q.id, title=q.title, unpinned=free, pinned=alone, concurrent=together, median_change=together['median'] / alone['median'] - 1, p95_change=together['p95'] / alone['p95'] - 1,
                        pinning_change=alone['median'] / free['median'] - 1))
        log(f'{q.id}: {target} unpinned {free["median"]:.3g} ms, pinned {alone["median"]:.3g} ms, with {len(others)} others {together["median"]:.3g} ms '
            f'(median {out[-1]["median_change"]:+.1%}, p95 {out[-1]["p95_change"]:+.1%})')
    worst = max(abs(r['median_change']) for r in out)
    log(f'{target} with {" ".join(others)} running at the same time on cores of their own: the median changed by at most {worst:.1%}, the p95 by at most {max(abs(r["p95_change"]) for r in out):.1%}')
    benchlib.write_json(os.path.join(args.out, f'interference-{target}-{args.tier}.json'), dict(target=target, others=others, cpus=args.cpus, rows=args.rows, samples=args.samples, cells=out,
                                                                                            date=benchlib.now_iso(), host=host_fp(), threshold=args.threshold, ok=worst <= args.threshold))
    return 0 if worst <= args.threshold else 1
