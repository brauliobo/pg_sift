"""The store benchmark's passes: the plan of a pass, the cache of the cells, merging and comparing part files (steps/store.py plan, merge, noisy, patch, stable)."""
import benchlib
import json
import os
import re
from store_common import dataset_id, host, image_ids, make_cache
import store_common
from store_updates import PHASES, UPDATE_SET
from store_lib import DDL, LANES, load_queries, log, read
from store_render import QUERY_TITLES, Results, all_lanes


def group_inputs(group, lanes, args):
    """everything a cached cell of the group depends on: the key of its entry is the hash of this. Not the pg_sift lanes (they are measured every pass)"""
    tag, iid = image_ids(group)
    qs = load_queries(args.queries)
    method = dict(runs=args.runs, warmups=args.warmups, cold_runs=args.cold_runs, restart_runs=args.restart_runs, cold_check=args.cold_check, parts=args.parts, points=args.points, load_cpus=args.cpus, parallel=args.parallel)
    if 'updates' in args.parts:
        method['updates'] = dict(load_seconds=args.load_seconds, probes=args.probes, rates=args.rates, phases=[p[0] for p in PHASES], set=UPDATE_SET)
    files = sorted({DDL[e] for e in lanes if e in DDL} | {{'ch': 'ch.sql', 'es': 'es.json'}.get(group, '')} - {''})
    return dict(group=group, lanes=lanes, image=tag, image_id=iid, dataset=dataset_id(args), memory=os.environ.get('STORE_MEMORY', ''),
                server_flags=os.environ.get('STORE_PG_FLAGS', '') if group in ('pg', 'citus') else '', statements={e: benchlib.key_of([q.statement(e, args.rows, e == 'es_cached') for q in qs], 12) for e in lanes},
                files={f: benchlib.key_of(read(f), 12) for f in files}, code=store_common.MEASURE_CODE, method=method)


def cmd_plan(args):
    """which lanes this pass measures and which it reads from the cache (--remeasure-all: none), written to plan.json; the lanes that are measured include PostgreSQL as the (untimed)
    reference of the result checks. Prints the decision of every server"""
    cache, hits, groups = make_cache(args), {}, {}
    every = list(dict.fromkeys(args.engines + args.update_engines))
    for g in dict.fromkeys(benchlib.group_of(e) for e in every):
        lanes = [e for e in every if benchlib.group_of(e) == g]
        why = ('pg_sift: measured every pass' if not benchlib.cacheable(g) else 'the cache is off (--remeasure-all)' if args.remeasure_all or not cache else 'named in --remeasure' if g in args.remeasure
               else 'compression variants: sizes are measured' if args.compression else '')
        inputs = group_inputs(g, lanes, args) if benchlib.cacheable(g) else {}
        entry = None
        if not why:
            entry, why = cache.lookup('cells', g, inputs)
        groups[g] = dict(lanes=lanes, status='cached' if entry else 'measure', inputs=inputs, path=cache.path('cells', g, inputs) if entry else None)
        if entry:
            hits[g] = entry
            log(f'plan {g}: read from the cache, measured {entry["date"]}, key {entry["key"][:8]}, host {entry["fp"]}')
        else:
            log(f'plan {g}: measured in this pass ({why})')
    run, untimed = benchlib.plan_engines(args.engines, hits)
    upd, _ = benchlib.plan_engines(args.update_engines, hits)
    benchlib.write_json(args.plan, dict(groups=groups, run=run, untimed=untimed, update=upd, fingerprint=args.fingerprint))
    log(f'plan: lanes {" ".join(run) or "none"}' + (f' (reference only: {" ".join(untimed)})' if untimed else '') + f'; updates {" ".join(upd) or "none"}; written {args.plan}')
    return 0


def cmd_planget(args):
    """one field of plan.json as a comma separated list (run, untimed, update, cached, measured)"""
    p = json.load(open(args.plan))
    print(','.join({'cached': [g for g, v in p['groups'].items() if v['status'] == 'cached'], 'measured': [g for g, v in p['groups'].items() if v['status'] == 'measure']}.get(args.field) or p.get(args.field, [])))
    return 0


def sub_meta(group, meta):
    """the part of a result set's servers and threads that describes one group's servers"""
    return ({k: v for k, v in meta.get('servers', {}).items() if k == group or k.startswith(group + '_')}, {c: v for c, v in meta.get('threads', {}).items() if c == LANES[group][1]})


def cmd_cache_save(args):
    """put the cells of the cacheable servers this pass measured into the cache (not those with a window that was still NOISY after the second measurement)"""
    plan, merged, cache = json.load(open(args.plan)), json.load(open(args.results)), make_cache(args)
    for g, v in plan['groups'].items():
        if v['status'] != 'measure' or not cache or not benchlib.cacheable(g):
            continue
        rows = [r for r in merged['rows'] if benchlib.group_of(r['engine']) == g]
        if not rows:
            continue
        if any(r['metric'] == 'noisy' for r in rows):
            log(f'cache {g}: not kept, a window was still NOISY after the second measurement')
            continue
        tiers = {}
        for t in sorted({r['tier'] for r in rows}):
            tm = merged['meta']['tiers'][t]
            servers, threads = sub_meta(g, tm)
            tiers[t] = dict(meta={k: x for k, x in tm.items() if k not in ('parts', 'servers', 'threads', 'cached')}, parts=tm['parts'], servers=servers, threads=threads)
        log(f'cache {g}: kept {cache.put("cells", g, v["inputs"], dict(rows=rows, tiers=tiers))}')
    return 0


def restart_cold(args, metas, rows):
    """cold cells of some engines taken from another results file, whose cells restarted the server for every run: for the engines where emptying the memory is not as cold
    (the check of `cold`), the numbers of this run's emptied cells stay as the check (`cold-alt`) of the questions it was made for"""
    d = json.load(open(args.restart_cold))
    engines = args.restart_engines
    for tier in {r['tier'] for r in d['rows'] if r['variant'] == 'cold'} & {t for t, m in metas.items() if 'cold' in m['parts']}:   # the sets this run has cold cells for
        part = d['meta']['tiers'][tier]['parts']['cold']
        src = [r for r in d['rows'] if r['tier'] == tier and r['engine'] in engines and r['variant'] == 'cold' and r['metric'] == 'latency_ms']
        keep = []
        for r in rows:
            if r['tier'] == tier and r['engine'] in engines and r['variant'] == 'cold' and r['metric'] == 'latency_ms':
                if r['class'] in metas[tier].get('cold_check', []):
                    r = dict(r, variant='cold-alt')
                else:
                    continue
            if r['tier'] == tier and r['engine'] == '-' and r['variant'].startswith('cold-') and r['variant'][5:] in engines:
                continue
            if r['tier'] == tier and r['metric'] == 'cold_how' and r['engine'] in engines:
                continue
            keep.append(r)
        rows = keep + src + [dict(dataset='store', tier=tier, engine=e, **{'class': '-'}, variant='cold', metric='cold_how', unit='text', value='restart', min='restart', max='restart', stderr=None, n=1, values=['restart']) for e in engines]
        metas[tier]['restart_cold'] = dict(engines=engines, date=part['date'], runs=d['meta']['tiers'][tier]['cold_runs'], noisy=[n for n in part['noisy'] if n.split()[1] in engines])
    return rows


def cmd_merge(args):
    """the files of the parts of a pass (store-<set>-<part>.json), the entries of the cache its plan read, and the sets of an earlier results file (--keep) that this pass did not
    measure again, into one results file. Cells of another host are never mixed in"""
    metas, rows, fp = {}, [], args.fingerprint
    for path in args.files:
        d = json.load(open(path))
        tier = d['meta']['tier']
        metas.setdefault(tier, {}).update({k: v for k, v in d['meta'].items() if k not in ('date', 'loadavg', 'loadavg_end', 'pressure', 'noisy', 'baseline', 'rerun', 'utilization')})
        metas[tier].setdefault('parts', {})[os.path.basename(path).split('-')[-1][:-5]] = dict(date=d['meta']['date'], loadavg=d['meta']['loadavg'], loadavg_end=d['meta'].get('loadavg_end'), pressure=d['meta'].get('pressure'), noisy=d['meta'].get('noisy', []), baseline=d['meta'].get('baseline'), rerun=d['meta'].get('rerun', []), utilization=d['meta'].get('utilization'))
        metas[tier].pop('loadavg_end', None)
        rows += d['rows']
    entries = [json.load(open(v['path'])) for plan in args.plan for v in json.load(open(plan))['groups'].values() if v['status'] == 'cached']
    benchlib.check_homogeneous(entries, fp)
    for e in entries:   # what was measured in this pass wins; a column read from the cache says when and under which key it was measured
        rows += e['rows']
        for tier, t in e['tiers'].items():
            m = metas.setdefault(tier, dict(t['meta'], parts={}))
            for k in ('servers', 'threads'):
                m[k] = {**t[k], **m.get(k, {})}
            m.setdefault('cached', {})[e['name']] = dict(date=e['date'], key=e['key'], fp=e['fp'], parts={p: x['date'] for p, x in t['parts'].items()})
    for keep in args.keep:
        old = json.load(open(keep))
        if not benchlib.same_host(old['meta'].get('host', {}), fp):
            log(f'WARN {keep} was measured on another host ({old["meta"].get("host", {}).get("cpu", "?")}, {old["meta"].get("host", {}).get("kernel", "?")}): its sets are not carried over')
            continue
        for tier, t in old['meta']['tiers'].items():
            if tier not in metas and not any(t['rows'] == m['rows'] and t.get('cpus') == m.get('cpus') for m in metas.values()):
                metas[tier] = t
                rows += [r for r in old['rows'] if r['tier'] == tier]
    if args.restart_cold:
        rows = restart_cold(args, metas, rows)
    with open(args.o, 'w') as f:
        f.write('{"meta": ' + json.dumps(dict(tiers=metas, host=host())) + ',\n "rows": [\n' + ',\n'.join(' ' + json.dumps(r) for r in rows) + '\n]}\n')
    log(f'written: {args.o}')
    return 0


def noisy_cells(meta):
    """the cells a part file reports as still NOISY: ('warm', question) / ('cold', engine, question) / ('updates', engine), from the `noisy` list of its meta"""
    out = []
    for s in meta.get('noisy', []):
        m = re.match(r'(?:(cold) (\w+) (\d+)|(updates) (\w+)|(\d+)): ', s)
        if m:
            out.append(('cold', m[2], m[3]) if m[1] else ('updates', m[5]) if m[4] else ('warm', m[6]))
    return out


def cmd_noisy(args):
    """the cells of a part file that are still NOISY, one per line: `warm QUESTION`, `cold ENGINE QUESTION`, `updates ENGINE` (bench/bench final measures them again)"""
    for c in noisy_cells(json.load(open(args.file))['meta']):
        print(' '.join(c))
    return 0


def cmd_patch(args):
    """replace in a part file the cells a second file measured again (same sets, engines and questions), and drop them from its list of NOISY cells"""
    old, new = json.load(open(args.file)), json.load(open(args.fresh))

    def measured_again(s):   # a NOISY entry of the old file whose cell the new file holds
        c = noisy_cells({'noisy': [s]})
        return bool(c) and any(r['class'] == c[0][1] if c[0][0] == 'warm' else (r['engine'], r['class']) == c[0][1:] for r in new['rows'] if c[0][0] != 'updates')

    old['rows'] = benchlib.replace_rows(old['rows'], new['rows'])
    old['meta']['noisy'] = [s for s in old['meta'].get('noisy', []) if not measured_again(s)] + new['meta'].get('noisy', [])
    old['meta']['rerun'] = sorted(set(old['meta'].get('rerun', [])) | set(new['meta'].get('rerun', [])) | {'patched'})
    benchlib.write_json(args.file, old)
    log(f'patched {args.file} with {args.fresh}')
    return 0


HEADLINE = [('sift', 'pg'), ('sift', 'citus'), ('sift', 'es'), ('sift', 'ch'), ('sift_table', 'pg')]   # the claims of the README: pg_sift against each system


def warm_by_cpus(path):
    """{cpus: {(engine, question): warm median}} of a results file"""
    R = Results(path)
    return {R.cpus(t): {(e, q): R.num(t, e, q, 'warm', 'latency_ms') for e in all_lanes(R, t) if e != 'es_cached' for q in R.queries(t)} for t in R.all_tiers}


def cmd_stable(args):
    """do two results files (two sizes) tell the same story? Per thread budget both have: the ratios of pg_sift to each system on every question must stay within a factor of
    1 + --tol, and no pair separated by more than --sep in both may change order. Exit status 1 when not (the ladder of presets is checked with this)"""
    a, b, bad = warm_by_cpus(args.a), warm_by_cpus(args.b), []
    for c in sorted(set(a) & set(b)):
        r = benchlib.stable({k: v for k, v in a[c].items() if v}, {k: v for k, v in b[c].items() if v}, args.tol, args.sep, HEADLINE)
        print(f'\n{c} CPUs: {"stable" if r["ok"] else "NOT stable"} (ratios within {1 + args.tol:g}x, order flips beyond {args.sep:g}x counted)')
        for q, i in r['queries'].items():
            print(f'  {q} {QUERY_TITLES.get(q, "")[:40]:40} pairs {i["pairs"]} flips {i["flips"]} worst drift {i["worst"][0]:.2f}x ({i["worst"][1]}/{i["worst"][2]})')
        bad += r['violations']
    return 1 if bad or not set(a) & set(b) else 0
