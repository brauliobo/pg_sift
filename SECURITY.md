# Security policy

pg_sift is C code that runs inside the PostgreSQL server and parses user-supplied documents and queries. A crash,
memory error, wrong-result or index-corruption bug reachable from SQL is in scope, in particular anything that a
crafted query string, a crafted document, concurrent DML, or recovery after a crash can trigger.

Please report such problems privately with GitHub's "Report a vulnerability" button (Security tab of the repository)
instead of opening a public issue, and include the PostgreSQL version, the `CREATE INDEX` statement and a reproduction
if you have one. Only the latest release and the `main` branch receive fixes.

The SQLite extension (`sift` for SQLite, in `sqlite/`) is C code that runs inside the process of the application that loads it and
parses documents and queries the application supplies; the same classes of problem are in scope (a crash, memory error, data race, wrong
result or index corruption reachable from SQL), reported the same way with the SQLite version, the `CREATE VIRTUAL TABLE` statement and a
reproduction. Loading an extension is code execution by design, so only load the `sift` file from a source you trust (check it against the
release's `SHA256SUMS`). The shadow tables (`<table>_chunks`, `_segs`, `_meta`, `_config`) are written by the extension: a database whose shadow
tables were edited by hand is not a supported input, though a crash on one is still welcome as a report.
