#!/usr/bin/env bash
# pg_upgrade (pg_upgrade.sh) from a cluster that runs the newest 3.2 release (a tag of this repository, built for the old major), to this tree: the extension is
# updated afterwards. Skipped when the repository has no such tag.
ref=$(git -C "$(dirname "$0")" tag -l 'v3.2.*' --sort=-v:refname | head -n 1)
[[ -n $ref ]] || { echo "1..0 # SKIP no release tag v3.2.*"; exit 0; }
SIFT_PG_OLD_REF=$ref exec bash "$(dirname "$0")/pg_upgrade.sh"
