#!/usr/bin/env python3
"""The claims of the CHANGELOG checked against records.

    python3 -I claims.py [--claims bench/claims.tsv] [--changelog CHANGELOG.md] [--tolerance 25] RECORDS...

A claim is a row of bench/claims.tsv: a phrase of the CHANGELOG line that states the figure, the statement and size it was measured on, the arm, the metric and the value the line gives,
with a tolerance in percent (the default is --tolerance). The check finds the line by its phrase, the statement in the records, and fails when the median of the records is farther from the
claimed value than the tolerance, naming both: the line and the run that measured. A phrase that matches no line is stale and fails as well. The phrase `-` is a claim that no CHANGELOG
line states (a design note, a page): its note says where it is made, and it is checked against the records alone, so a figure outside the CHANGELOG has a run behind it too.

    phrase  statement  rows  arm  option  metric  value  tolerance(optional)  note(optional)

statement is the name of the statement, and may be narrowed to one table state and one mode of the records, `name@dataset#variant` (either part optional): a statement measured on
several table states or under several index options has a claim for each.

arm is heap, btree, gin, sift or picked (the plan the planner chose); metric is exec_ms, plan_ms, wall_ms, instr_m, est_cost, blocks (warm hit + read of every kind) or cold_blocks
(blocks read after the eviction), heap_blocks (warm hit + read of the heap alone).
"""
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import check  # noqa: E402
import schema  # noqa: E402

KINDS = ('heap', 'btree', 'gin', 'sift', 'other')
METRICS = ('exec_ms', 'plan_ms', 'wall_ms', 'instr_m', 'est_cost', 'blocks', 'heap_blocks', 'cold_blocks', 'instr_leader_m', 'instr_workers_m', 'index_bytes', 'wal_bytes', 'segments', 'immutable_segments', 'buffer_segments')
NO_LINE = '-'   # the phrase of a claim that no CHANGELOG line states (a page, a design note, a patch description): it is checked against the records alone


def metric(r, name):
    if name == 'blocks':
        return sum(r.get('%s_%s' % (k, p)) or 0 for k in KINDS for p in ('hit', 'read'))
    if name == 'cold_blocks':
        return sum(r.get('%s_read' % k) or 0 for k in KINDS)
    if name == 'heap_blocks':
        return (r.get('heap_hit') or 0) + (r.get('heap_read') or 0)
    return r.get(name)


def measured(records, statement, rows, arm, option, name):
    """(median, the rows it comes from) of a metric for the arm of the statement at ROWS; arm `picked` is the default plan"""
    temp = 'cold' if name == 'cold_blocks' else 'warm'
    statement, _, variant = statement.partition('#')
    statement, _, dataset = statement.partition('@')
    rs = [r for r in records if r['statement'] == statement and (not dataset or r['dataset'] == dataset) and (not variant or r['variant'] == variant) and r['rows'] == rows and r['temp'] == temp and
          ((arm == 'picked' and r['mode'] == 'default') or (arm != 'picked' and r['mode'] == 'forced' and r['arm'] == arm and r['arm_option'] == option))]
    vals = [metric(r, name) for r in rs if metric(r, name) is not None]
    return (statistics.median(vals) if vals else None), rs


def read_claims(path):
    out = []
    with open(path) as f:
        lines = f.readlines()
    for n, line in enumerate(lines, 1):
        if line.startswith('#') or not line.strip():
            continue
        f = line.rstrip('\n').split('\t')
        if f[0] == 'phrase':
            continue
        if len(f) < 7:
            raise ValueError('%s:%d: %d fields, 7 are required' % (path, n, len(f)))
        out.append(dict(line=n, phrase=f[0], statement=f[1], rows=int(f[2].replace(',', '')), arm=f[3], option=f[4] or 'default', metric=f[5], value=float(f[6]),
                        tol=float(f[7]) if len(f) > 7 and f[7] else None, note=f[8] if len(f) > 8 else ''))
    return out


def verify(claims, records, changelog, tolerance=25.0, claims_file='claims.tsv'):
    """the problems of the claims: a stale phrase, a claim with no record, a value off by more than its tolerance"""
    # a claim names the run it reads by its statement, dataset and variant, so the records are checked run by run: the arms of one run were measured together, and runs of
    # several index options (one table each) are not arms of one comparison
    problems = [p for run in sorted({r['run_id'] for r in records}) for p in check.check([r for r in records if r['run_id'] == run])]
    if problems:
        return ['the records are refused (check.py): ' + problems[0]] + problems[1:5]
    lines = [l for l in changelog.split('\n') if l.startswith('- ')]
    out = []
    for c in claims:
        where = '%s:%d' % (os.path.basename(claims_file), c['line'])
        if c['metric'] not in METRICS:
            out.append('%s: metric %s is none of %s' % (where, c['metric'], '/'.join(METRICS)))
            continue
        line = next((l for l in lines if c['phrase'] in l), None) if c['phrase'] != NO_LINE else 'no CHANGELOG line (%s)' % c['note']
        if line is None:
            out.append('%s: STALE CLAIM: no CHANGELOG line holds "%s"' % (where, c['phrase']))
            continue
        got, rs = measured(records, c['statement'], c['rows'], c['arm'], c['option'], c['metric'])
        if got is None:
            out.append('%s: NO RECORD for %s at %s rows, arm %s %s, %s: the line "%s" cannot be checked' % (where, c['statement'], format(c['rows'], ','), c['arm'], c['option'], c['metric'], line[:90]))
            continue
        tol = c['tol'] if c['tol'] is not None else tolerance
        off = 100 * abs(got - c['value']) / abs(c['value']) if c['value'] else (0 if got == 0 else float('inf'))
        if off > tol:
            out.append('%s: CLAIM NOT MET: the CHANGELOG says %s %s = %g ("%s"), run %s measured %g (%.0f%% off, tolerance %g%%)' % (
                where, c['statement'], c['metric'], c['value'], line[:110], rs[0]['run_id'], got, off, tol))
    return out


def main(argv):
    claims_file, changelog, tol, files, i = 'bench/claims.tsv', 'CHANGELOG.md', 25.0, [], 0
    while i < len(argv):
        if argv[i] == '--claims':
            claims_file, i = argv[i + 1], i + 2
        elif argv[i] == '--changelog':
            changelog, i = argv[i + 1], i + 2
        elif argv[i] == '--tolerance':
            tol, i = float(argv[i + 1]), i + 2
        else:
            files.append(argv[i])
            i += 1
    if not files:
        sys.exit(__doc__)
    records = []
    records += schema.read_files(files, say=lambda m: print('note: ' + m, file=sys.stderr))
    claims = read_claims(claims_file)
    problems = verify(claims, records, open(changelog).read(), tol, claims_file)
    for p in problems:
        print(p)
    print('claims: %d checked, %d problems' % (len(claims), len(problems)), file=sys.stderr)
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
