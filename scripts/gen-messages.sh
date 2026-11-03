#!/usr/bin/env bash
# The messages of pg_sift: every NOTICE, WARNING and INFO that src/ and sql/ send to the client, one row each in docs/messages.tsv (what it says, where and when, the decision, its benefit and
# trade-off, how to switch it off), and docs/MESSAGES.md generated from the rows. Needs python3 and no PostgreSQL.
#
#   scripts/gen-messages.sh             check, then write docs/MESSAGES.md
#   scripts/gen-messages.sh --check     check, and that docs/MESSAGES.md is the generated one (lint)
#   scripts/gen-messages.sh --selftest  prove in memory that a message without a row, a row without a message, a reworded message or level, a changed hint, an invented number and an unknown option are caught
#   scripts/gen-messages.sh --skeleton  print the rows (text, level, DETAIL and HINT filled in) of the messages that have none, to complete and paste into docs/messages.tsv
#   scripts/gen-messages.sh --missing   list the rows whose benefit or trade-off says that nobody measured it
#
# The emitters are the ereport/elog calls of src/ at NOTICE, WARNING and INFO (or a level variable, as VACUUM VERBOSE passes) outside test-only files and #ifdef SIFT_TESTING, and the RAISE NOTICE/WARNING/INFO of
# sql/ outside the test helper extensions. The check fails when
#   - an emitter has no row, or a row names a message (level and text) that no emitter raises, or the file of the row does not raise it,
#   - the DETAIL or the HINT of a row differs from the literal of the source (one built at run time with "%s" is not compared),
#   - a row lacks its id, group, level, source, message, fires, when, decision, benefit, tradeoff, switch or doc, a title (a row of the glance table) lacks default, gain or cost, or doc is not a file and heading of docs/,
#   - a number of gain, cost, benefit or tradeoff is in no row of docs/options.tsv named in `option` and in no file named in `evidence` (a number is quoted, never made up), or a cell has no number and does
#     not say "no measured number yet" (or "n/a: reason"),
#   - docs/MESSAGES.md is not what the rows generate.
# A new message needs its row: --skeleton prints it. Wording of a message changes the row (and the generated page) in the same commit.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check || $mode == --selftest || $mode == --skeleton || $mode == --missing ]] || { echo "usage: $0 [--check|--selftest|--skeleton|--missing]" >&2; exit 2; }
exec python3 -I "$PWD/scripts/gen-messages/main.py" "$mode"
