# Fresh-backend mode

`scripts/pgtest.sh fresh [TEST...]` runs the regression tests of `test/sql` with a **new backend for every top-level statement**, to find the bugs of the
first statement of a backend: a table or a shared structure that is attached lazily, a cache that is empty, a memory context that does not exist yet. A test
that passes in one long session and fails here (or the other way round) has a statement that depends on what the statements before it left in its backend.
Both wrong answers and the cold-backend crash of a table slot made in a short-lived context are of this kind.

How: `scripts/fresh-psql.py` stands first in `PATH` as `psql` for `pg_regress` only. It reads the script that `pg_regress` feeds on stdin and puts a silent
`\connect` before every statement that is not inside a transaction block, replaying the `SET`/`RESET` of the session first. The output is psql's, so the
expected files apply unchanged; the first difference is what the cold backend does differently.

From the first statement that binds the session to its backend (`PREPARE`, `DECLARE`, `LISTEN`, `LOAD`, a temporary table, a session advisory lock,
`set_config`, `setseed`, `\if`, `\i`) the rest of the file runs in the backend it has, because a new connection would lose what the file uses afterwards.
`FRESH_STATS=FILE` appends a line `new backends<TAB>statements` per run (`pgtest.sh fresh` writes it next to the results).

A setting of the extension (`sift.*`) is unknown to a backend that has not loaded the library, as for any extension that is not preloaded: a test that sets one
out of range or shows one fails in the default mode. `FRESH_LOAD=pg_sift` loads the library first in every new backend, as `session_preload_libraries` would, and
leaves the cold first statement to the lazily attached state only (`FRESH_LOAD=pg_sift scripts/pgtest.sh fresh TEST...`).

Limits that keep it safe on a host: the shim refuses to run without `FRESH_PSQL` (the real psql, set by `pgtest.sh`), inside itself (`FRESH_ACTIVE`) or as its
own psql, so it cannot start itself without end; `pgtest.sh fresh` does not nest and takes one of `SIFT_FRESH_RUNS` locks (`$XDG_RUNTIME_DIR/sift-fresh.N.lock`; default 1, at most 4), so no more fresh runs than that are at work
on a host, each on a cluster of its own, and the tests of a run follow one another. Calls of psql with a command of their own (`-c`, `-f`) are not made fresh.

Cost: a connection per statement, about 2 ms with the unix socket, so a file of a few hundred statements takes a second or two longer. Run it over the files
that touch `src/agg/exec`, `src/col/am` and `src/table` (the focus of `scripts/ci.sh affected`), and over the whole suite when the lazily attached state changes.
Self-test: `test/t/fresh_backend.sh`; the crash class of a slot in the wrong context is the `slot_owner` test of the assert lane.
