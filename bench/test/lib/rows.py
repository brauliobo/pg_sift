"""The rows the records tests are made of: a clean set, and a mutant is the clean set with one field changed."""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'steps', 'records'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'steps'))
import check  # noqa: E402
import collect  # noqa: E402

SHAPES = {'sift': 'Custom Scan SiftAgg t', 'btree': 'Index Scan t_ix', 'heap': 'Seq Scan t'}


def shape(structure):
    """the plan columns of a read whose plan names STRUCTURE"""
    return dict(plan_shape=SHAPES[structure], plan_hash=collect.plan_hash(SHAPES[structure]))


BASE = dict(run_id='abc123456789-20261101T000000Z-0001', tip='abc123456789', harness='test records-1', dataset='orders', variant='main', rows=300000, keys=3000, rows_per_key=100.0, heap_order='arrival',
            heap_corr=0.01, table_state='fresh_vacuumed', allvisible_pct=100.0, structures='btree,heap,sift', statement='top20', sql_hash='aaaaaaaaaaaa', kind='read', protocol='single', mode='forced',
            arm='sift', arm_option='default', picked=1, structure='sift', temp='warm', rep=1, est_cost=100.0, plan_ms=0.1, exec_ms=1.0, wall_ms=1.3, instr_m=5.0, cycles_m=4.0,
            instr_scope='cluster', workers_requested=0, workers_launched=0, heap_hit=10, heap_read=0, btree_hit=0, btree_read=0, gin_hit=0, gin_read=0, sift_hit=30, sift_read=0,
            other_hit=0, other_read=0, os_cache_dropped=None, wal_bytes=0, fpw=1, cpus=4.0, psi_cpu_start=1.0, psi_cpu_end=2.0, psi_io_start=0.0, psi_io_end=0.0, shared_buffers_mb=256.0,
            work_mem_mb=64.0, pg_version='18.1', settings='', settings_hash='da39a3ee5e', maintenance='default', **shape('sift'))

CHURN = 'upd=5;ins=0;del=0;rounds=1'


def clean():
    rs = []
    for arm, option, structure, picked, mode, ms in (('sift', 'default', 'sift', 1, 'forced', 1.0), ('btree', 'default', 'btree', 0, 'forced', 2.0), ('heap', 'default', 'heap', 0, 'forced', 40.0),
                                                     ('sift', 'default', 'sift', 1, 'default', 1.0)):
        for temp in ('warm', 'cold'):
            for rep in (1, 2, 3):
                r = dict(BASE, arm=arm, arm_option=option, structure=structure, picked=picked, mode=mode, temp=temp, rep=rep, exec_ms=ms, wall_ms=ms * 1.3, _file='t.tsv', _line=len(rs) + 2, **shape(structure))
                if temp == 'cold':
                    r.update(os_cache_dropped=0, sift_read=30 if arm == 'sift' else 0, btree_read=4 if arm == 'btree' else 0, heap_read=2000 if arm == 'heap' else 0)
                rs.append(r)
    return rs


def build_rows(**kw):
    """the three warm repetitions of a build of a sift index: the leader and two workers counted apart, three segments, none declared"""
    rows = []
    for rep in (1, 2, 3):
        r = dict(BASE, kind='build', mode='forced', picked=0, arm='sift', arm_option='default', structure='sift', temp='warm', rep=rep, plan_ms=None, exec_ms=None, structures='heap,sift', statement='build',
                 index_bytes=1000, segments=3, immutable_segments=3, buffer_segments=0, index_options='default', instr_m=10.0, instr_leader_m=4.0, instr_workers_m=6.0, workers_requested=2,
                 workers_launched=2, wal_bytes=5, _file='t.tsv', _line=rep + 1)
        r.update(kw)
        rows.append(r)
    return rows


def mutate(**kw):
    rs = clean()
    for k, v in kw.items():
        rs[-1][k] = v
    return rs


def problems(rs):
    return check.check(rs)
