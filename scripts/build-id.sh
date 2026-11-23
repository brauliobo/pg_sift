#!/bin/sh
# The id of the source tree, the version of the module magic (pg_get_loaded_modules() on PostgreSQL 18): "3.5.0+2a7e475" for a checkout at v3.5.0. A checkout gives it from
# git; a `git archive` (the deb and rpm builds) from BUILD_ID, which export-subst fills in (.gitattributes); a tarball with neither, "unknown". A clone without release tags takes the version of META.json: "6.0.0+2a7e475". make SIFT_BUILD_ID=x overrides it.
cd "$1" || exit 1
meta=$(sed -n 's/^  "version": "\(.*\)",$/\1/p' META.json 2>/dev/null | sed -n 1p)
if [ -e .git ]; then
	id=$(git describe --tags --match 'v[0-9]*' --dirty 2>/dev/null || echo "$meta")+$(git rev-parse --short HEAD 2>/dev/null)
else id=$(sed -n 1p BUILD_ID 2>/dev/null); fi
case $id in ''|+|\$*) echo unknown ;; +*) echo "$meta$id" ;; *) echo "${id#v}" ;; esac
