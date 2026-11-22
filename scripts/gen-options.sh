#!/usr/bin/env bash
# The options of pg_sift: every sift.* GUC, sift/sift_col reloption, and sift_table option read from C sources.
# Generates the three compact tables in docs/OPTIONS.md (sift-settings, sift-reloptions, sift-table-options).
# Needs python3 and no PostgreSQL (a second or less); --live asks a server.
#
#   scripts/gen-options.sh             check, then write docs/OPTIONS.md
#   scripts/gen-options.sh --check     check, and that docs/OPTIONS.md is the generated one (lint, scripts/ci.sh)
#   scripts/gen-options.sh --live      compare pg_settings and sift._test_reloptions of a new database with the sources (SIFT_TESTING build)
#   scripts/gen-options.sh --selftest  prove in memory that a new option without a row, a row without an option, or a bad link is caught
#   scripts/gen-options.sh --missing   report missing option rows (compact 6.0 schema)
#   scripts/gen-options.sh --snapshot  print scripts/options-released.tsv for the current sources
#
# Defaults and ranges come from DefineCustom*Variable(), add_*_reloption(), and opt_desc[] in src/; purpose, values overrides,
# applies, links, and since live in docs/options.tsv (58 release knobs in pg_sift 6.0). The check fails when:
#   - an option of src/ has no row in docs/options.tsv, or a row names an option that does not exist,
#   - a row has an invalid applies value, empty purpose, purpose > 140 chars, invalid since, or broken docs/ link,
#   - a reloption is registered without a matching relopt_parse_elt entry (or uses PG < 19 isset_offset),
#   - docs/OPTIONS.md does not match the generated tables.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check || $mode == --live || $mode == --selftest || $mode == --snapshot || $mode == --missing ]] || { echo "usage: $0 [--check|--live|--selftest|--snapshot|--missing]" >&2; exit 2; }
exec python3 -I "$PWD/scripts/gen-options/main.py" "$mode"
