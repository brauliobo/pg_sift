"""Request size: the same aggregate over the rows of N keys listed in IN (...); the table is 1M rows of 100k keys, ordered by key."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

KEYS = 100000


def setup(size):
    return f"""create table t as select (i / 10) as k, i % 97 as s, i % 1000 as v from generate_series(0, {KEYS * 10 - 1}) i order by 1;
vacuum freeze t;
create index ix on t using sift_col (k, s, v);
analyze t;"""


def keys(n):
    return ', '.join(str(i * (KEYS // n) + 1) for i in range(n))


CASES = [Case('requests', [32, 500, 5000, 50000], setup=setup, unit='values', quick=3, queries=[
    Query('count_sum', lambda n: f"select count(*), sum(v) from t where k in ({keys(n)})", 'linear'),
    Query('by_group', lambda n: f"select s, count(*), sum(v) from t where k in ({keys(n)}) group by s", 'linear'),
])]
