"""Groups: one grouping column per cardinality on a table of ROWS rows; the size is the number of groups."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query


def build(sizes, rows):
    def setup(size):
        cols = ', '.join(f"(hashint4(i) & 2147483647) % {n} as g{n}" for n in sizes)
        return f"""create table t as select i as id, {cols}, i % 1000 as v from generate_series(1, {rows}) i;
vacuum freeze t;
create index ix on t using sift_col (id, {', '.join(f'g{n}' for n in sizes)}, v);
analyze t;"""
    return setup


QUERIES = [
    Query('count_sum', lambda n: f"select g{n}, count(*), sum(v) from t group by 1", 'linear'),
    Query('count_distinct', lambda n: f"select g{n}, count(distinct v) from t group by 1", 'linear'),
    Query('top10', lambda n: f"select g{n}, count(*) c from t group by 1 order by c desc, 1 limit 10", 'linear'),
]
SIZES = [1000, 10000, 100000, 1000000]
GATE_SIZES = [1000, 4000, 16000, 64000, 256000]
CASES = [Case('groups', SIZES, setup=build(SIZES, 1000000), unit='groups', quick=3, queries=QUERIES)]
GATE = [Case('groups', GATE_SIZES, setup=build(GATE_SIZES, 262144), unit='groups', queries=QUERIES)]
