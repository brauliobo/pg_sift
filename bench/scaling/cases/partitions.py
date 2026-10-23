"""Partitions: P range partitions of 20 rows each; the size is P. Planning time is the cost under test."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

ROWS = 20


def setup(size):
    return f"""create table t (k int, v int) partition by range (k);
select format('create table t_%s partition of t for values from (%s) to (%s)', i, i * {ROWS}, (i + 1) * {ROWS}) from generate_series(0, {size - 1}) i \\gexec
insert into t select i, i % 100 from generate_series(0, {size * ROWS - 1}) i;
create index ix on t using sift_col (k, v);
vacuum freeze t;
analyze t;"""


CASES = [Case('partitions', [16, 64, 256, 1024, 4096], setup=setup, unit='partitions', quick=3, per_size=True,
              queries=[
    Query('one_partition', lambda p: "select count(*), sum(v) from t where k between 5 and 10", 'linear', needs_sift=False),
    Query('all_partitions', lambda p: "select count(*), sum(v) from t", 'linear'),
])]
