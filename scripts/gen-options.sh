#!/usr/bin/env bash
# The options of pg_sift: every sift.* GUC, sift/sift_col reloption, and sift_table option read from C sources.
# Generates the compact tables in docs/OPTIONS.md: the 63 knobs of 6.0 (sift-settings, sift-reloptions, sift-table-options) and the 40 deprecated or Format 1
# compatibility options that stay accepted (sift-deprecated-options).
# Needs python3 and no PostgreSQL (a second or less); --live asks a server.
#
#   scripts/gen-options.sh             check, then write docs/OPTIONS.md
#   scripts/gen-options.sh --check     check, and that docs/OPTIONS.md is the generated one (lint, scripts/ci.sh)
#   scripts/gen-options.sh --live      compare pg_settings, sift._test_reloptions, and the options of a Format 2.0 sift_table of a new database with the sources (SIFT_TESTING build)
#   scripts/gen-options.sh --selftest  prove in memory that a new option without a row, a row without an option, a bad link, or a wrong replacement is caught
#   scripts/gen-options.sh --missing   report missing option rows (compact 6.0 schema)
#   scripts/gen-options.sh --snapshot  print scripts/options-released.tsv for the current sources
#
# Defaults and ranges come from DefineCustom*Variable(), add_*_reloption(), and the option name arrays of src/table/f2/f2_options.c; purpose, values overrides,
# applies, links, since, and the replacement of a deprecated option live in docs/options.tsv. An option is deprecated when src/ says so (a description that starts
# with 'Format 1 only (deprecated' or 'Deprecated', a hidden sift_table option, or a diagnostic). The check fails when:
#   - an option of src/ has no row in docs/options.tsv, or a row names an option that does not exist,
#   - a row has an invalid applies value, empty purpose, purpose > 140 chars, invalid since, or broken docs/ link,
#   - a deprecated option has no replacement, an option that is not deprecated has one, or the replacement is not the one its description says,
#   - a reloption is registered without a matching relopt_parse_elt entry (or uses PG < 19 isset_offset),
#   - docs/OPTIONS.md does not match the generated tables.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check || $mode == --live || $mode == --selftest || $mode == --snapshot || $mode == --missing ]] || { echo "usage: $0 [--check|--live|--selftest|--snapshot|--missing]" >&2; exit 2; }
exec python3 -I "$PWD/scripts/gen-options/main.py" "$mode"
