"""Partitions: P range partitions of ROWS rows each; the size is P. A request of 51 keys touches 51 partitions whatever P is (flat: planning follows the partitions
touched, not the partitions that exist); a query over all of them is linear (planning and execution)."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

ROWS = 500


def build(rows):
    def setup(size):
        return f"""create table t (k int, v int) partition by range (k);
select format('create table t_%s partition of t for values from (%s) to (%s)', i, i * {rows}, (i + 1) * {rows}) from generate_series(0, {size - 1}) i \\gexec
insert into t select i, i % 100 from generate_series(0, {size * rows - 1}) i;
create index ix on t using sift_col (k, v);
vacuum freeze t;
analyze t;"""
    return setup


def request(rows):
    # 51 keys, one in each of 51 partitions spread over the first 51 * 8 partitions (every size of the case has at least 64 partitions)
    return ', '.join(str(p * 8 * rows + 3) for p in range(51))


def queries(rows):
    return [
        Query('touched_51', lambda p: f"select count(*), sum(v) from t where k in ({request(rows)})", 'flat', needs_sift=False),
        # information: this is PostgreSQL's own planning and scan of every partition (superlinear at 400-2000 partitions, the native plan the same), not a sift cost
        Query('all_partitions', lambda p: "select count(*), sum(v) from t", None, needs_sift=False),
    ]


CASES = [Case('partitions', [16, 64, 256, 1024, 4096], setup=build(ROWS), unit='partitions', quick=3, per_size=True,
              queries=[Query('one_partition', lambda p: "select count(*), sum(v) from t where k between 5 and 10", 'linear'),
                       Query('all_partitions', lambda p: "select count(*), sum(v) from t", 'linear')])]
GATE = [Case('partitions', [416, 512, 768, 1024, 2048], setup=build(50), unit='partitions', per_size=True, queries=queries(50))]
