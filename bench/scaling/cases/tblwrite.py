"""Table writes: UPDATEs by key of rows that keep their index keys, on a sift_table whose write buffer holds `size` rows that interleave with the
old rows in the pages of a secondary index. Each update makes the index test the TIDs of a page for dead versions (bottom-up deletion), which
the table AM answers from its superblocks: the cost of an update must not grow with the rows the write buffer holds."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from model import Case, Query

ROWS = 200000                                 # promoted rows


def setup(size):
    return f"""create table t (id int, k int, grp int, v int, note text) using sift_table with (autovacuum_enabled = false);
insert into t select g, (g * 7919) % 100000, g % 97, g, 'n' || g from generate_series(1, {ROWS}) g;
alter table t add primary key (id);
create index t_k on t (k);
create index t_grp on t (grp);
select sift.table_maintain('t');
vacuum analyze t;
insert into t select g + 1000000, (g * 7919) % 100000, g % 97, g, 'w' || g from generate_series(1, {size}) g;
analyze t;"""


# 500 promoted rows spread over the table, their keys unchanged (the statement commits: every run adds 500 rows to the write buffer, a drift well below the sizes)
def upd(stride):
    return lambda size: f"update t set v = v + 1 where id in (select g * {stride} + 7 from generate_series(1, 500) g)"


CASES = [Case('tblwrite', [1000, 10000, 100000], setup=setup, per_size=True, native=False, unit='buffered rows', queries=[
    Query('update_scattered', upd(397), 'flat', needs_sift=False),
    Query('update_consecutive', lambda size: "update t set v = v + 1 where id between 50001 and 50500", 'flat', needs_sift=False),
])]
