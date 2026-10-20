# Set filter benchmarks

The data and the timed cells of [docs/SET-FILTERS.md](../../docs/SET-FILTERS.md): a fact table of events, a dimension table of entities and a membership table of groups, all synthetic and deterministic.

```sh
psql -d sets -v np=200000 -v nr=3000000 -v sch=sets -f bench/sets/gen.sql     # about 4 minutes
psql -d sets -v sch=sets -f bench/sets/build.sql                               # the fact table, its btrees and the sift_col index
bench/sets/run.sh docs/results/sets-array-param.tsv --design 1 --reps 5       # timed cells; --list prints them
```

`run.sh` runs `run.py` in a systemd scope (`MemoryMax=12G`, `CPUQuota=300%`) and `run.py` takes the host lock `/tmp/sift-perf.lock` (blocking `flock`) for the whole run;
the connection is the libpq environment, the database `sets` unless `PGDATABASE` says another. A cell is one query, one plan mode and one session; the cells interleave in
random order, the first pass is the warm-up, and the median of the timed runs is the figure. The answer of every cell is compared with the one of the normal plan.
