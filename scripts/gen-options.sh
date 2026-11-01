#!/usr/bin/env bash
# The options of pg_sift: every reloption of the index kinds and every sift.* setting, read from the C sources. The block of README.md between the
# sift-options markers (a pointer), the two tables of docs/USAGE.md (sift-settings, sift-reloptions) and the case of every option in docs/OPTIONS.md (sift-cases) are generated. Needs python3 and no PostgreSQL (a second
# or less); --live asks a server.
#
#   scripts/gen-options.sh             check, then write README.md and docs/USAGE.md
#   scripts/gen-options.sh --check     check, and that both files are the generated ones (lint, scripts/ci.sh)
#   scripts/gen-options.sh --live      compare the settings (pg_settings) and the reloptions (sift._test_reloptions) of a new database with the sources (PG* in
#                                      the environment, a SIFT_TESTING build; scripts/ci.sh)
#   scripts/gen-options.sh --selftest  prove in memory that a new option without a row, a row without an option and a changed default are caught
#   scripts/gen-options.sh --missing   list the defaults and opt-ins with no measured gain, growth or write cost, no S, save, G, W or R of the default formula (a structure or an opt-in) and, where there is a gain, no benchmark (each needs a measurement by the lane that owns it)
#   scripts/gen-options.sh --snapshot  print scripts/options-released.tsv for the current sources (a release replaces the file with it)
#
# What the sources say is read, not copied: the DefineCustom*Variable() calls of src/ give a setting its name, type, default, range, context, unit and
# description; the add_*_reloption() calls give a reloption its name, type, default, range and description. Default and range are the values the library
# is compiled with (a #define is evaluated), so a change of one reaches the tables with `make options`. What a call cannot say (the index kind or object
# a setting is for, what it buys, what it costs, where to read more, the grammar of a string option) is one row of docs/options.tsv per option; settings
# of one family may share a README row (group). The check fails when
#   - an option of src/ has no row, or a row names an option that does not exist (a setting under #ifdef SIFT_TESTING is not documented),
#   - a row lacks its `applies`, `buys` or `cost` (a member of a group takes the first row's), names an unknown index kind, or links to a file or heading that is not there,
#   - a row lacks its class (default, opt-in, diagnostic, safety, deprecated) or its advice, an opt-in lacks the term of the default formula it failed (`why`), a bool
#     opt-in defaults to on (or a bool default to off), or a pr, since or bench cell names nothing real,
#   - the cells s, save, g, w, r (speedup in instructions, thousands of instructions saved a query, growth %, write cost %, relevance 1 or 0.5) are not numbers, an opt-in passes the formula (B = R*log2(S)/log2(1.05) at least C = G/3 + W/5, S at least 1.05,
#     100k instructions saved; growth is a term of C, not a ceiling), an opt-in's `why` does not name the terms it fails (gain, cost), or a default fails it without `Formula waived: reason` in default_why,
#   - a reloption is registered without an entry of the relopt_parse_elt table of its access method, or the reverse,
#   - a default that is not a literal cannot be evaluated (add the PostgreSQL constant to CONST),
#   - an option is new since the last release, has another default or writes a newer column format minor than that release reads, and its `upgrade` cell (what a user of that
#     release gets after ALTER EXTENSION UPDATE, and whether it needs a REINDEX) is empty,
#   - README.md, docs/USAGE.md or docs/OPTIONS.md is not what is generated.
# `n/a: reason` in a gain, growth, write or bench cell says there is nothing to measure (a planning switch has no write cost; no GPU here); `test:NAME` in bench names the regress or TAP test that exercises the option.
# A gain, growth or write cell that reads `not measured` is no failure: it is a measurement nobody made (`--missing`, and **not measured** in docs/OPTIONS.md); empty is a failure.
# `new in <version>` and `was <default>` in the tables come from scripts/options-released.tsv, the defaults of the last release; `new in` is the default_version of
# pg_sift.control unless the `since` cell of the row names the release that really has the option.
set -euo pipefail

cd "$(dirname "$0")/.."
mode=${1:-write}
[[ $mode == write || $mode == --check || $mode == --live || $mode == --selftest || $mode == --snapshot || $mode == --missing ]] || { echo "usage: $0 [--check|--live|--selftest|--snapshot|--missing]" >&2; exit 2; }
exec python3 -I "$PWD/scripts/gen-options/main.py" "$mode"
