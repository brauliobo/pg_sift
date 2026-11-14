"""Statistics and verdicts of the DRY performance gate (dry-perf-gate.py): the minimum of the runs of a case, the verdict of a case from its two sides, the table and the evidence line.
Pure functions of the numbers; the limits are the options of the gate (`cfg`)."""


def spread(xs):
    return (max(xs) - min(xs)) / min(xs) * 100 if xs and min(xs) else 0.0


def summarize(runs):
    ok = [r for r in runs if r['ok']]
    if not ok:
        return dict(error=runs[-1].get('error', 'failed') if runs else 'no runs', n=0)
    m = {k: min(r[k] for r in ok) for k in ('instr', 'cycles', 'ms')}
    for k in ('hits', 'reads', 'writes', 'extends', 'wal'):
        m[k] = min(r[k] for r in ok)
        m[k + '_spread'] = max(r[k] for r in ok) - m[k]
    m.update(instr_spread=spread([r['instr'] for r in ok]), cycles_spread=spread([r['cycles'] for r in ok]), n=len(ok),
             rows=sorted({r['rows'] for r in ok}), nrows=ok[0]['nrows'])
    return m


def judge(b, c, cfg, noise, par=False):
    """Verdict of one case from the summaries of the two sides: (status, [reasons], numbers)."""
    why = []
    if 'error' in b or 'error' in c:
        return 'ERROR' if 'error' in b and 'error' in c else 'FAIL', [f"base: {b.get('error', 'ok')}; cand: {c.get('error', 'ok')}"], {}
    di = (c['instr'] - b['instr']) / b['instr'] * 100
    dc = (c['cycles'] - b['cycles']) / b['cycles'] * 100
    abs_ok = c['instr'] - b['instr'] <= cfg.instr_abs
    thr_c = max(cfg.max_cycles_pct, 1.5 * noise.get('cycles', 0), noise.get('floor', 0))
    if b['rows'] != c['rows'] or len(c['rows']) > 1 or len(b['rows']) > 1:
        why.append('rows differ')
    max_instr = max(cfg.max_instr_pct, cfg.par_instr_pct) if par else cfg.max_instr_pct
    if di > max_instr and not abs_ok:
        why.append(f'instructions {di:+.2f}% > {max_instr}%')
    warn = []
    if dc > thr_c and b['cycles'] >= cfg.min_cycles:
        if abs(di) <= cfg.layout_instr_pct and dc <= cfg.layout_cycles_pct:
            # the same instructions and blocks, a moderate cycles difference: the machine code moved (alignment, cache sets), not the work; recorded, not failed
            warn.append(f'cycles {dc:+.2f}% with equal instructions ({di:+.3f}%): code layout or noise')
        else:
            why.append(f'cycles {dc:+.2f}% > {thr_c:.2f}%')
    for k in ('hits', 'reads', 'wal'):
        tol = max(cfg.wal_tol, 2 * noise.get('wal', 0)) if k == 'wal' else cfg.blocks_tol
        if par:       # the leader's blocks and the WAL of the workers vary with the split of the work
            tol = max(tol, 16, b[k] * (cfg.par_wal_pct if k == 'wal' else cfg.par_blocks_pct) / 100)
        if abs(c[k] - b[k]) > tol:
            why.append(f'{k} {b[k]} -> {c[k]}')
    return ('FAIL' if why else 'WARN' if warn else 'PASS'), why + warn, dict(d_instr=di, d_cycles=dc, thr_cycles=thr_c)




def fmt_m(x):
    return f'{x / 1e6:,.2f}M'


def report(verdicts, res, args, meta):
    lines, fails = [], 0
    hdr = f"{'case':34s} {'instr base -> cand':>26s} {'d%':>7s} {'cycles d%':>9s} {'noise%':>6s} {'ms b/c':>12s} {'hit+read b -> c':>22s} {'wal b -> c':>16s} verdict"
    lines.append(hdr)
    for cid, (st, why, num) in verdicts.items():
        r = res[cid]
        b, c = r['sides'].get('base', {}), r['sides'].get('cand', {})
        if 'error' in b or 'error' in c:
            lines.append(f'{cid:34s} ERROR {b.get("error", "")} | {c.get("error", "")}')
            fails += st == 'FAIL'
            continue
        lines.append(f"{cid:34s} {fmt_m(b['instr']):>12s} -> {fmt_m(c['instr']):>10s} {num['d_instr']:+7.3f} {num['d_cycles']:+9.2f} {max(b['cycles_spread'], c['cycles_spread']):6.2f} "
                     f"{b['ms']:6.1f}/{c['ms']:<6.1f} {b['hits']:>9d}+{b['reads']:<5d}-> {c['hits']:>9d}+{c['reads']:<4d} {b['wal']:>7d}->{c['wal']:<7d} {st}" + (' ' + '; '.join(why) if why else ''))
        fails += st == 'FAIL'
    return lines, fails


def evidence_line(verdicts, res, meta, status):
    ok = [cid for cid in verdicts if 'd_instr' in verdicts[cid][2]]
    if not ok:
        return f"perf-gate {meta['base']} -> {meta['cand']}: {status}"
    bi = sum(res[c]['sides']['base']['instr'] for c in ok)
    ci = sum(res[c]['sides']['cand']['instr'] for c in ok)
    bc = sum(res[c]['sides']['base']['cycles'] for c in ok)
    cc = sum(res[c]['sides']['cand']['cycles'] for c in ok)
    blocks = lambda s: sum(res[c]['sides'][s]['hits'] + res[c]['sides'][s]['reads'] for c in ok)
    wal = lambda s: sum(res[c]['sides'][s]['wal'] for c in ok)
    worst = max(ok, key=lambda c: verdicts[c][2]['d_instr'])
    noise = max(max(res[c]['sides']['base']['cycles_spread'], res[c]['sides']['cand']['cycles_spread']) for c in ok)
    size = meta['cand_text'] - meta['base_text']
    return (f"perf-gate {meta['base']} -> {meta['cand']} ({len(ok)} cases, groups {','.join(sorted(meta['groups']))}): "
            f"instructions:u {fmt_m(bi)} -> {fmt_m(ci)} ({(ci - bi) / bi * 100:+.3f}%, worst {worst} {verdicts[worst][2]['d_instr']:+.3f}%), "
            f"cycles:u {fmt_m(bc)} -> {fmt_m(cc)} ({(cc - bc) / bc * 100:+.2f}%, noise {noise:.1f}%), blocks {blocks('base')} -> {blocks('cand')}, WAL {wal('base')} -> {wal('cand')} B, "
            f"text {size:+d} B, rows identical{meta.get('port_note', '')}: {status}")
