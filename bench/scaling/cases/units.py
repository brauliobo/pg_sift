"""Units: a table of one row per heap page, so that one unit costs as many rows as it has pages. The index of each size is a partial sift_col index on id <= size * pages (same heap).
SCALING_UNIT_PAGES is the pages of a unit of the build under test (256, the default; 8 in a build with SIFT_SB_SHIFT=3, which reaches 64k units in 4 GB); the output says it."""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

PAGES = int(os.environ.get('SCALING_UNIT_PAGES', 256))
ROWS = PAGES                                 # rows (= pages) of a unit
print(f'# units: {PAGES} heap pages a unit', file=sys.stderr)
IN51 = ', '.join(str(1000 + 37 * i) for i in range(51))


def setup(size):
    return f"""create table t (id int, g int, v int, pad text);
alter table t alter column pad set storage plain;
insert into t select i, i % 100, i % 1000, repeat('x', 7000) from generate_series(1, {size * ROWS}) i;
vacuum freeze t;"""


def at(size):
    return f"drop index if exists ix; create index ix on t using sift_col (id, g, v) with (point_columns = 'id') where id <= {size * ROWS};"


def q(name, body, target='flat'):
    return Query(name, lambda size: f"select {body.format(bound=size * ROWS)}", target, needs_sift=target is not None)


CASES = [Case('units', [512, 1024, 2048, 4096] if PAGES > 16 else [1024, 4096, 16384, 65536], setup=setup, at=at, native=False, unit='units',
              check=lambda size: [f"select count(*), sum(v) from t where id <= {size * ROWS}"], queries=[
    q('one_unit', "g, count(*), sum(v) from t where id between 1000 and 1100 and id <= {bound} group by g"),
    q('nothing', "count(*), sum(v) from t where id <= {bound} and v > 100000", 'linear'),
    q('count', "count(*) from t where id <= {bound}", 'linear'),
    q('sum', "sum(v) from t where id <= {bound}", 'linear'),
    q('order_limit', "id from t where id <= {bound} order by id desc limit 10"),
    # information only: a point lookup and an IN list are not planned on the sift_col index of a release build (a seq scan with nothing to replace it; with the
    # partial index the scan is the bitmap of the whole index predicate), so these two measure the heap or the partial index, not a point directory
    q('point', "id, v from t where id = 777 and id <= {bound}", None),
    q('in51', f"id, v from t where id in ({IN51}) and id <= {{bound}}", None),
])]


# The CI gate (scripts/scaling-gate.sh): 8-page units (a SIFT_TESTING=1 SIFT_SB_SHIFT=3 build), 0.5k-8k units = 0.7 GB of tmpfs (the CI scratch cap is 2.4 GB for all the jobs of a lane). The point and IN queries read the
# point directory through the test function of the access method (a release plan does not reach it), flat in the units.
def point(keys):
    return lambda size: f"select count(*) from sift._test_col_tids('ix'::regclass, '0:in:{'|'.join(map(str, keys))}', 0)"


def tids_match(keys):
    ids = ', '.join(map(str, keys))
    return lambda size: [f"select count(*) = (select count(*) from t where id in ({ids}) and id <= {size * ROWS}) and count(*) = {len(keys)} "
                         f"from sift._test_col_tids('ix'::regclass, '0:in:{'|'.join(map(str, keys))}', 0)"]


GATE = [Case('units', [512, 1024, 2048, 4096, 8192], setup=setup, at=at, native=False, unit='units',
             check=lambda size: [f"select count(*), sum(v) from t where id <= {size * ROWS}"] + tids_match([777])(size) + tids_match([1000 + 37 * i for i in range(51)])(size), queries=[
    q('one_unit', "g, count(*), sum(v) from t where id between 1000 and 1100 and id <= {bound} group by g"),
    q('pruned', "count(*), sum(v) from t where id <= {bound} and v > 100000", 'linear'),
    q('count', "count(*) from t where id <= {bound}", 'linear'),
    q('sum', "sum(v) from t where id <= {bound}", 'linear'),
    q('order_limit', "id from t where id <= {bound} order by id desc limit 10"),
    Query('point', point([777]), 'flat', needs_sift=False),
    Query('in51', point([1000 + 37 * i for i in range(51)]), 'flat', needs_sift=False),
])]
if PAGES != 8:
    GATE = []                                  # the gate needs the 8-page geometry; scaling.py then has no case to run
