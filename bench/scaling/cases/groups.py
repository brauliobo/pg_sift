"""Groups: 1M rows, one grouping column per cardinality; the size is the number of groups."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

SIZES = [1000, 10000, 100000, 1000000]


def setup(size):
    cols = ', '.join(f"(hashint4(i) & 2147483647) % {n} as g{n}" for n in SIZES)
    return f"""create table t as select i as id, {cols}, i % 1000 as v from generate_series(1, 1000000) i;
vacuum freeze t;
create index ix on t using sift_col (id, {', '.join(f'g{n}' for n in SIZES)}, v);
analyze t;"""


CASES = [Case('groups', SIZES, setup=setup, unit='groups', quick=3, queries=[
    Query('count_sum', lambda n: f"select g{n}, count(*), sum(v) from t group by 1", 'linear'),
    Query('count_distinct', lambda n: f"select g{n}, count(distinct v) from t group by 1", 'linear'),
    Query('top10', lambda n: f"select g{n}, count(*) c from t group by 1 order by c desc, 1 limit 10", 'linear'),
])]
