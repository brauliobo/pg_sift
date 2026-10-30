# The table of families and the variants (settings) each query runs under.
# A family is a generator, a renderer and the share of runs it gets.

import re

from .db import BASE
from .fam_basic import gen_colcmp, gen_distleaf, gen_keys, gen_lookup, gen_numsum, gen_oneleaf, gen_partition, gen_placeholder, gen_preagg, gen_setkeys, gen_widew, render_lookup, render_oneleaf, render_partition, render_placeholder, render_preagg, render_setkeys
from .fam_shapes import gen_decor, gen_dirty, gen_exists, gen_lateralagg, gen_latest, gen_leftlateral, gen_toplat, gen_topn, gen_union, render_decor, render_dirty, render_exists, render_lateralagg, render_latest, render_leftlateral, render_toplat, render_topn, render_union
from .fam_lists import gen_distinctrows, gen_listpage, gen_oneentity, gen_point, gen_pointagg, gen_same, gen_trail, gen_twolevel, gen_txnmix, render_distinctrows, render_listpage, render_oneentity, render_point, render_pointagg, render_same, render_trail, render_twolevel, render_txnmix
from .fam_units import gen_arrlen, gen_dead, gen_floor, gen_keycut, gen_nullzone, gen_window, render_keycut, render_window
from .fam_cte import gen_ctecopies, gen_dictcount, gen_exceptions, gen_flatkeys, gen_gorder, gen_keylist, gen_keytable, gen_zonekey, render_ctecopies, render_exceptions, render_flatkeys, render_gorder, render_keylist

def base_of(parts, nl):
    return BASE + (';enable_nestloop=off' if nl else '') + (';TimeZone=%s' % parts['tz'] if parts.get('tz') else '')


FAMILIES = {
    'preagg': (gen_preagg, render_preagg, 4), 'partition': (gen_partition, render_partition, 3), 'lookup': (gen_lookup, render_lookup, 3),
    'dirty': (gen_dirty, render_dirty, 4), 'union': (gen_union, render_union, 2), 'topn': (gen_topn, render_topn, 2), 'trail': (gen_trail, render_trail, 3), 'twolevel': (gen_twolevel, render_twolevel, 3),
    'samelookup': (gen_same, render_same, 5), 'keys': (gen_keys, render_preagg, 4),
    'leftlateral': (gen_leftlateral, render_leftlateral, 4), 'lateralagg': (gen_lateralagg, render_lateralagg, 3), 'point': (gen_point, render_point, 3), 'pointagg': (gen_pointagg, render_pointagg, 4),
    'keycut': (gen_keycut, render_keycut, 4), 'floor': (gen_floor, render_keycut, 4), 'dead': (gen_dead, render_keycut, 5), 'latest': (gen_latest, render_latest, 3), 'toplat': (gen_toplat, render_toplat, 4), 'decor': (gen_decor, render_decor, 4), 'exists': (gen_exists, render_exists, 4), 'nullzone': (gen_nullzone, render_preagg, 4), 'arrlen': (gen_arrlen, render_preagg, 4),
    'placeholder': (gen_placeholder, render_placeholder, 3),
    'distinctrows': (gen_distinctrows, render_distinctrows, 3), 'exceptions': (gen_exceptions, render_exceptions, 5),
    'numsum': (gen_numsum, render_preagg, 4), 'colcmp': (gen_colcmp, render_preagg, 4), 'oneleaf': (gen_oneleaf, render_oneleaf, 3), 'setkeys': (gen_setkeys, render_setkeys, 3), 'window': (gen_window, render_window, 4), 'gorder': (gen_gorder, render_gorder, 3), 'widew': (gen_widew, render_preagg, 4), 'distleaf': (gen_distleaf, render_preagg, 4),
    'oneentity': (gen_oneentity, render_oneentity, 3),
    'listpage': (gen_listpage, render_listpage, 3),
    'flatkeys': (gen_flatkeys, render_flatkeys, 4), 'ctecopies': (gen_ctecopies, render_ctecopies, 4), 'keylist': (gen_keylist, render_keylist, 4), 'txnmix': (gen_txnmix, render_txnmix, 3), 'zonekey': (gen_zonekey, render_preagg, 4), 'keytable': (gen_keytable, render_preagg, 4), 'dictcount': (gen_dictcount, render_preagg, 4),
}
# the variants of the pushdown (settings after BASE); the reference is REF
LEADER = ['on', 'off']
PAR = 'max_parallel_workers_per_gather=%d;parallel_setup_cost=0;parallel_tuple_cost=0;min_parallel_table_scan_size=0;min_parallel_index_scan_size=0;parallel_leader_participation=%s'


POINT = 'sift.enable_col_scan=on;sift.enable_agg=off;enable_seqscan=off;max_parallel_workers_per_gather=0;'


def simd_levels():
    """The levels of sift.simd below the top one this host runs, scalar first (the top is the default of every other variant); the host's CPU flags decide."""
    flags = ''
    try:
        flags = re.search(r'^(?:flags|Features)\s*:(.*)$', open('/proc/cpuinfo').read(), re.M).group(1).split()
    except (OSError, AttributeError):
        pass
    mid = ['avx2'] if 'avx512f' in flags and 'avx2' in flags else []
    return ['scalar'] + mid


SIMD_LEVELS = simd_levels()


def variants(r, fam):
    if fam == 'point':  # the access method: an index scan, a bitmap scan; the reference is the normal plan
        return [('idx', POINT + 'enable_bitmapscan=off'), ('bmp', POINT + 'enable_indexscan=off;enable_indexonlyscan=off')]
    v = [('pre', 'sift.enable_agg=on;max_parallel_workers_per_gather=0'),
         ('scan', 'sift.enable_agg=on;sift.agg_preagg=off;max_parallel_workers_per_gather=0')]
    # every SIMD level below the top one the host supports, on the scan: the answers are the same bit for bit
    v += [('simd' + lvl, 'sift.enable_agg=on;sift.agg_preagg=off;sift.simd=%s;max_parallel_workers_per_gather=0' % lvl) for lvl in SIMD_LEVELS]
    if fam in ('topn', 'trail', 'toplat'):  # the heap lowered: the sort of every row that passes runs on the rows of the tables (a testing build)
        v.append(('topnsort', 'sift.enable_agg=on;sift.test_limits=' + r.choice(['topn_rows=20', 'topn_keys=1', 'topn_rows=20,topn_keys=2']) + ';max_parallel_workers_per_gather=0'))
    if fam == 'dead':  # the dead rows scanned, and any share of them netted
        v.append(('nodelta', 'sift.enable_agg=on;sift.agg_dead_delta=0;max_parallel_workers_per_gather=0'))
        v.append(('alldead', 'sift.enable_agg=on;sift.agg_dead_delta=100;max_parallel_workers_per_gather=0'))
    if fam == 'window':  # the index scan alone, with the pushdown off: the run answers a range in the access method too
        v.append(('amscan', 'sift.enable_agg=off;sift.enable_col_scan=on;max_parallel_workers_per_gather=0'))
    if fam == 'numsum':  # every SIMD level of the host (a level it lacks runs the next lower one): the sums must be the same bytes
        v += [('simd_' + lv, f'sift.enable_agg=on;sift.agg_preagg=off;sift.simd={lv};max_parallel_workers_per_gather=0') for lv in ('scalar', 'avx2', 'avx512', 'neon')]
    if fam in ('widew', 'distleaf'):  # 0 (the scan above), 2 and 4 workers, the leader in and out of the scan
        v += [('par2', 'sift.enable_agg=on;sift.agg_preagg=off;' + PAR % (2, 'on')), ('par4', 'sift.enable_agg=on;sift.agg_preagg=off;' + PAR % (4, 'off'))]
    if fam == 'colcmp':  # every SIMD level of the host (a level it lacks runs the next lower one): the bitmaps must be the same
        v += [('simd_' + lv, f'sift.enable_agg=on;sift.agg_preagg=off;sift.simd={lv};max_parallel_workers_per_gather=0') for lv in ('scalar', 'avx2', 'avx512', 'neon')]
    if fam == 'gorder':  # the scan of the units, which the arrays stand for
        v.append(('nogorder', 'sift.enable_agg=on;sift.test_topn_gorder=off;max_parallel_workers_per_gather=0'))
        v += [('gpar%d' % n, 'sift.enable_agg=on;' + PAR % (n, 'on')) for n in (2, 4)]
    if fam == 'keys':  # the units' key lists and bitmaps off: the pre-aggregate scan reads the key chunks
        v.append(('nodistinct', 'sift.enable_agg=on;sift.agg_preagg_distinct=off;max_parallel_workers_per_gather=0'))
    if fam in ('twolevel', 'union', 'lookup', 'oneleaf', 'setkeys'):
        v.append(('arms', 'sift.enable_agg=on;sift.agg_two_level=off;sift.agg_fuse=off;max_parallel_workers_per_gather=0'))
    extra = [
        ('par', 'sift.enable_agg=on;' + PAR % (r.choice([1, 2, 3, 4]), r.choice(LEADER))),
        ('parscalar', 'sift.enable_agg=on;sift.simd=scalar;' + PAR % (r.choice([2, 3]), r.choice(LEADER))),
        ('plainpar', 'sift.enable_agg=off;' + PAR % (2, 'on')),
        ('nolookup', 'sift.enable_agg=on;sift.agg_lookup_pushdown=off;sift.agg_share_lookups=off;max_parallel_workers_per_gather=0'),
        ('nodense', 'sift.enable_agg=on;sift.agg_dense=off;sift.simd=scalar;max_parallel_workers_per_gather=0'),
        ('notopn', 'sift.enable_agg=on;sift.enable_topn=off;max_parallel_workers_per_gather=0'),
        ('bitset', 'sift.enable_agg=on;sift.test_limits=bits_set_values=4;max_parallel_workers_per_gather=0'),
        ('childwarm', 'sift.enable_agg=on;sift.test_child_warm=on;max_parallel_workers_per_gather=0'),
    ]
    v += r.sample(extra, r.choice([1, 2]))
    return v
