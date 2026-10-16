# Security policy

pg_sift is C code that runs inside the PostgreSQL server and parses user-supplied documents and queries. A crash,
memory error, wrong-result or index-corruption bug reachable from SQL is in scope, in particular anything that a
crafted query string, a crafted document, concurrent DML, or recovery after a crash can trigger.

Please report such problems privately with GitHub's "Report a vulnerability" button (Security tab of the repository)
instead of opening a public issue, and include the PostgreSQL version, the `CREATE INDEX` statement and a reproduction
if you have one. Only the latest release and the `main` branch receive fixes.
