# Connected designs: how a prototype plugs in

A design is one Python module in this directory, `designs/<name>.py` (dashes in the engine name become underscores), used as the engine
`connected:<name>`: `run-example star-orders S connected:<name>`. It never touches the examples.

```python
def ddl(ex):          # -> a path to a .sql file, a string of SQL, or None (no connected variant for this example)
def session_sql():    # -> list of SQL statements run at the start of every measured session (GUCs, LOAD); optional, default []
```

`ddl(ex)` runs after the data is loaded, the btrees and the unconnected sift_col indexes exist (the connected variant adds to them, so the
write cost of "unconnected" is always included). `ex` offers what a generic design needs:

| attribute | content |
|---|---|
| `ex.id`, `ex.dir`, `ex.meta` | the example, its directory, its registry row (dict) |
| `ex.links` | the joins of the example (`links.tsv`): `Link(child_table, child_col, parent_table, parent_col, relation, reads)`; `reads` are the parent attributes the queries read |
| `ex.queries`, `ex.batches` | the typed queries and write batches |
| `ex.file(name)`, `ex.text(name)` | any file of the example directory (`connected.sql`, `sift.sql`, ...) |

A generic design builds its DDL from `ex.links` (one connected index per link); a hand-tuned one can look at `ex.id` and return a file of its own. The
design `slot` runs the example's own `connected.sql`, the placeholder each example carries for that case.

The run is the same as for the other engines: every query is compared with the oracle (plain heap plans, sift off) after the load and after every
write batch, the plan and instruction counts are recorded, and `--phases load,queries,writes,concurrency` picks the parts. A design that gives a wrong
answer fails the run and the example's oracle names the query and the batch.
