# The commands of steps/store.py that render: render, table, compare and gap.
# They write the blocks into the files that hold their markers.
import benchlib
import os
from store_lib import LANES
from .results import Results, md_table, ms
from .tables import QUERY_TITLES, bench_table
from .notes import cpu_name, ratio
from .blocks import benchmarks_block, readme_block
from .updates import updates_benchmarks, updates_readme
from .compression import compression_block


def replace_blocks(args):
    import readme  # steps/readme.py: replace_block
    import section_render   # steps/section_render.py: imports this module
    R = Results(args.results)
    S = section_render.Section(args.updates) if os.path.exists(args.updates) else None
    if args.readme:
        readme.replace_block(args.readme, 'store', readme_block(R))
        readme.replace_block(args.readme, 'store-updates', section_render.readme_block(S) if S else updates_readme(R))
    if args.benchmarks:
        readme.replace_block(args.benchmarks, 'store', benchmarks_block(R))
        readme.replace_block(args.benchmarks, 'store-updates', updates_benchmarks(R))
        if S:
            readme.replace_block(args.benchmarks, 'updates', section_render.benchmarks_block(S))
        if os.path.exists(args.compression):
            readme.replace_block(args.benchmarks, 'store-compression', compression_block(Results(args.compression)))
    if S and args.img:
        section_render.write_chart(S, args.img)


def cmd_render(args):
    if args.stdout:
        R = Results(args.results)
        print('\n'.join(readme_block(R) + [''] + updates_readme(R) + [''] + benchmarks_block(R) + [''] + updates_benchmarks(R)))
    else:
        replace_blocks(args)
    return 0


def all_lanes(R, tier):
    return R.engines(tier, list(LANES))


def set_title(R, tier):
    return f'{R.meta["tiers"][tier]["rows"]:,} orders' + (f', {cpu_name(R.cpus(tier))}' if R.cpus(tier) else '')


def cmd_table(args):
    """the warm medians of every result set (size and thread budget), as a Markdown table (stdout)"""
    R = Results(args.file)
    for tier in R.all_tiers:
        engines = all_lanes(R, tier)
        head, body = bench_table(R, tier, engines, {e: LANES[e][0] for e in LANES})
        print(f'\n{set_title(R, tier)}, warm median ms\n')
        print('\n'.join(md_table(head, body)))
    return 0


def cmd_compare(args):
    """two result files side by side: the warm median of every question and system in each result set both have, the ratio B / A (below 1: B is faster) with the 95% interval of the
    ratio of the medians of the runs, and * where the interval excludes 1 and a Mann-Whitney test of the runs rejects equality (benchlib.changed)"""
    A, B = Results(args.a), Results(args.b)
    body, flagged = [], 0
    for tier in [t for t in A.all_tiers if t in B.all_tiers]:
        for q in A.queries(tier):
            for e in all_lanes(A, tier):
                ra, rb = (X.get(tier, e, q, 'warm', 'latency_ms') for X in (A, B))
                if not (ra and rb and ra.get('values') and rb.get('values')):
                    continue
                ratio, lo, hi, p, diff = benchlib.changed(ra['values'], rb['values'])
                flagged += diff
                body.append([tier, q, QUERY_TITLES[q][:44], LANES[e][0], ms(ra['value']), ms(rb['value']), f'{ratio:.2f} [{lo:.2f}, {hi:.2f}]' + (' *' if diff else ''), f'{p:.3f}'])
    print('\n'.join(md_table(['set', 'q', 'question', 'system', 'A ms', 'B ms', 'B / A [95%]', 'p'], body, ['---'] * 4 + ['---:'] * 4)))
    print(f'\n* {flagged} of {len(body)} cells changed (interval of the ratio excludes 1 and Mann-Whitney p < 0.05)')
    return 0


def gap_rows(R, tier):
    """one row per question: the warm medians of sift_col, sift_table, PostgreSQL, ClickHouse and Elasticsearch (request cache off, on), and sift_col / ClickHouse, sift_col / Elasticsearch"""
    out = []
    for q in R.queries(tier):
        v = {e: R.num(tier, e, q, 'warm', 'latency_ms') for e in ('sift', 'sift_table', 'pg', 'ch', 'es', 'es_cached')}
        rel = lambda e: f'**{v["sift"] / v[e]:.2f}**' if v['sift'] and v[e] and v['sift'] > v[e] else f'{v["sift"] / v[e]:.2f}' if v['sift'] and v[e] else 'n/a'
        out.append([q, QUERY_TITLES[q], *(ms(v[e]) for e in v), rel('ch'), rel('es'), rel('es_cached')])
    return out


def gap_table(R):
    head = ['q', 'question', 'sift_col', 'sift_table', 'PostgreSQL', 'ClickHouse', 'Elasticsearch', 'ES, cache on', 'sift_col / ClickHouse', 'sift_col / ES', 'sift_col / ES cache on']
    out = []
    for tier in R.all_tiers:
        out += [f'#### {set_title(R, tier)} (warm median, ms; a ratio above 1, in bold, means pg_sift is slower)', '']
        out += md_table(head, gap_rows(R, tier), ['---:', '---'] + ['---:'] * 9) + ['']
    return out


def cmd_gap(args):
    print('\n'.join(gap_table(Results(args.file))))
    return 0
