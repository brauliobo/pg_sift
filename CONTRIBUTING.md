# Contributing

pg_sift needs PostgreSQL 17 or 18 (19 is experimental) with the server headers (`pg_config` on `PATH`) and gcc or clang.

```sh
make                                # builds pg_sift.so; PG_CONFIG=/path/to/pg_config picks another installation
make installcheck                   # regression tests against a running server that can load pg_sift
make check                          # quick tier: every suite in a throwaway cluster, about 2-3 minutes
make check-full                     # full tier: ASan build, long soak and fuzz runs, all mutants
```

`scripts/pgtest.sh install && scripts/pgtest.sh start` gives a throwaway cluster (ports 56100-56169, nothing
installed system-wide); `scripts/pgtest.sh psql` connects to it and `scripts/pgtest.sh destroy` removes it.
`scripts/replication.sh` (also part of `make check`) tests physical and logical replication on temporary clusters;
`scripts/replication.sh --container` runs it with every node in its own docker container (`PG_IMAGE`, default `postgres:18`).
`SIFT_TESTING=1` (the default for `pgtest.sh` and `make check`) also builds the test-only `pg_sift_test` extension and the
`sift._test_*` functions; a normal `make install` never installs them. Switching it needs `make clean`.

Every suite runs under one leak gate, `scripts/leakcheck.sh`, without any test referring to it: the server logs of every
cluster and node are scanned for what PostgreSQL logs when a transaction or backend leaks (buffer, relcache, TupleDesc,
snapshot, lock and file references, `you don't own a lock`), for assertion failures, crashes and sanitizer reports; a
`SANITIZE=1` server also reports malloc leaks as each process exits (LeakSanitizer, `scripts/lsan.supp`); every cluster is
checked when it stops (no session left in a transaction or holding locks, no prepared transaction, no temporary file left,
no long-lived process grown); the soak fails on a backend whose memory keeps growing. `make check` ends with the scan of
everything it ran. A test that crashes or leaks on purpose declares it with `scripts/leakcheck.sh expect COUNT REGEX`.

Before sending a change:

- run `make check` and add a regression test (`test/sql/NAME.sql` plus `test/expected/NAME.out`) or a TAP test
  (`test/t/`) for a fix or a new behavior;
- keep the build free of compiler warnings;
- start the commit subject with the module or feature it changes: `query: parse fuzzy terms with a distance above 2`.

Contributions are licensed under the [PostgreSQL License](LICENSE) like the rest of the project.
