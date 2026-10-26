#!/bin/sh
# The id of the source tree, the version of the module magic (pg_get_loaded_modules() on PostgreSQL 18): "3.5.0+2a7e475" for a checkout at v3.5.0. A checkout gives it from
# git; a `git archive` (the deb and rpm builds) from BUILD_ID, which export-subst fills in (.gitattributes; the sha alone when the clone has no tags); a tarball with neither, "unknown". make SIFT_BUILD_ID=x overrides it.
cd "$1" || exit 1
if [ -e .git ]; then id=$(git describe --tags --always --dirty 2>/dev/null)+$(git rev-parse --short HEAD 2>/dev/null)
else id=$(sed -n 1p BUILD_ID 2>/dev/null); fi
case $id in ''|+|\$*) echo unknown ;; +*) echo "${id#+}" ;; *) echo "${id#v}" ;; esac
